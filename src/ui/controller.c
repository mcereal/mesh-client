#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/controller.h"

#include "mesh/ui/backend.h"
#include "mesh/ui/nav.h"
#include "mesh/utils/log.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

/*
 * Asks for one more frame in MESH_UI_FRAME_INTERVAL_MS, or stops asking.
 *
 * An all-zero it_value disarms, so "still moving" and "settled" are the same call with a
 * different answer from the backend - there is no separate stop path to forget to take.
 */
static void mesh_ui_controller_schedule_frame(struct mesh_ui_controller *controller, bool moving) {
    if (controller->frame_timer_fd < 0) {
        return;
    }

    struct itimerspec spec;
    memset(&spec, 0, sizeof spec);
    if (moving) {
        spec.it_value.tv_nsec = (long)MESH_UI_FRAME_INTERVAL_MS * 1000000L;
    }
    if (timerfd_settime(controller->frame_timer_fd, 0, &spec, NULL) < 0) {
        mesh_log_warn("ui", "frame timerfd_settime failed: %s", strerror(errno));
    }
}

/* Whether the backend says the frame it just drew has not finished moving. */
static bool mesh_ui_controller_backend_moving(const struct mesh_ui_controller *controller) {
    return controller->backend != NULL && controller->backend->animating != NULL &&
           controller->backend->animating(controller->backend_state, controller->backend_userdata);
}

static void mesh_ui_controller_present(struct mesh_ui_controller *controller,
                                       const struct mesh_ui_snapshot *snapshot) {
    if (controller->backend != NULL && controller->backend->present != NULL) {
        controller->backend->present(controller->backend_state, snapshot,
                                     controller->backend_userdata);
    }
}

static int mesh_ui_controller_event_callback(int fd, uint32_t events, void *userdata) {
    (void)fd;
    struct mesh_ui_controller *controller = (struct mesh_ui_controller *)userdata;
    if (controller == NULL) {
        return 0;
    }

    if ((events & EPOLLIN) == 0U) {
        return 0;
    }

    if (mesh_ui_store_consume_updates(controller->store, &controller->snapshot)) {
        controller->snapshot_valid = true;
        mesh_ui_controller_present(controller, &controller->snapshot);
    }

    mesh_ui_controller_schedule_frame(controller, mesh_ui_controller_backend_moving(controller));
    return 0;
}

/* Animation frames reuse the last published snapshot. Consume any real update first so
   timer/store readiness in the same epoll batch cannot render stale data. */
static int mesh_ui_controller_frame_callback(int fd, uint32_t events, void *userdata) {
    struct mesh_ui_controller *controller = (struct mesh_ui_controller *)userdata;
    if (controller == NULL || (events & EPOLLIN) == 0U) {
        return 0;
    }

    uint64_t expirations = 0U;
    if (read(fd, &expirations, sizeof expirations) < 0 && errno != EAGAIN) {
        mesh_log_warn("ui", "frame timer read failed: %s", strerror(errno));
    }

    if (mesh_ui_store_consume_updates(controller->store, &controller->snapshot)) {
        controller->snapshot_valid = true;
    } else {
        controller->snapshot.update_flags = MESH_UI_UPDATE_NONE;
    }
    if (controller->snapshot_valid) {
        mesh_ui_controller_present(controller, &controller->snapshot);
    }

    mesh_ui_controller_schedule_frame(controller, mesh_ui_controller_backend_moving(controller));
    return 0;
}

/* The timer is optional: without it a switch lands on its target on the next frame something
   else asks for, which is a UI that works and does not animate. */
static void mesh_ui_controller_setup_frame_timer(struct mesh_ui_controller *controller,
                                                 struct mesh_event_loop *loop) {
    controller->frame_timer_fd = -1;
    if (loop == NULL || controller->backend == NULL || controller->backend->animating == NULL) {
        return;
    }

    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        mesh_log_warn("ui", "frame timerfd_create failed: %s", strerror(errno));
        return;
    }
    if (mesh_event_loop_add_fd(loop, fd, EPOLLIN, mesh_ui_controller_frame_callback, controller) <
        0) {
        mesh_log_warn("ui", "Failed to watch the frame timer");
        close(fd);
        return;
    }
    controller->frame_timer_fd = fd;
}

int mesh_ui_controller_init(struct mesh_ui_controller *controller, struct mesh_ui_store *store,
                            const struct mesh_ui_backend *backend, void *backend_userdata,
                            struct mesh_event_loop *loop) {
    if (controller == NULL || store == NULL) {
        return -EINVAL;
    }

    memset(controller, 0, sizeof *controller);
    controller->frame_timer_fd = -1;
    controller->store = store;
    controller->backend = backend;
    controller->backend_userdata = backend_userdata;
    controller->loop = loop;

    if (backend != NULL && backend->init != NULL) {
        int result = backend->init(&controller->backend_state, backend_userdata);
        if (result < 0) {
            mesh_log_error("ui", "Backend init failed (%s): %d",
                           backend->name != NULL ? backend->name : "unknown", result);
            controller->backend = NULL;
            controller->backend_state = NULL;
            controller->backend_userdata = NULL;
        }
    }

    const int event_fd = mesh_ui_store_event_fd(store);
    if (loop != NULL && event_fd >= 0) {
        int add_result = mesh_event_loop_add_fd(loop, event_fd, EPOLLIN,
                                                mesh_ui_controller_event_callback, controller);
        if (add_result < 0) {
            mesh_log_error("ui", "Failed to register UI store fd: %d", add_result);
            if (controller->backend != NULL && controller->backend->shutdown != NULL) {
                controller->backend->shutdown(controller->backend_state,
                                              controller->backend_userdata);
            }
            controller->backend = NULL;
            controller->backend_state = NULL;
            controller->backend_userdata = NULL;
            return add_result;
        }
        controller->registered = true;
    }

    mesh_ui_controller_setup_frame_timer(controller, loop);

    /* The store only signals on change, so a client that comes up with no devices and no
       handshake would sit on an unpainted screen indefinitely. Ask for one snapshot now so
       the backend draws a frame as soon as the loop runs. */
    mesh_ui_store_request_refresh(store);

    return 0;
}

void mesh_ui_controller_set_action_handler(struct mesh_ui_controller *controller,
                                           mesh_ui_action_handler handler, void *userdata) {
    if (controller == NULL) {
        return;
    }
    controller->on_action = handler;
    controller->action_userdata = userdata;
}

void mesh_ui_controller_handle_key(struct mesh_ui_controller *controller, enum mesh_ui_key key) {
    if (controller == NULL || controller->store == NULL || key == MESH_UI_KEY_NONE) {
        return;
    }

    struct mesh_ui_action action;
    mesh_ui_store_handle_key(controller->store, key, &action);
    if (action.type != MESH_UI_ACTION_NONE && controller->on_action != NULL) {
        controller->on_action(controller->action_userdata, &action);
    }
}

void mesh_ui_controller_shutdown(struct mesh_ui_controller *controller) {
    if (controller == NULL) {
        return;
    }

    if (controller->loop != NULL && controller->registered) {
        const int event_fd = mesh_ui_store_event_fd(controller->store);
        mesh_event_loop_remove_fd(controller->loop, event_fd);
        controller->registered = false;
    }

    if (controller->frame_timer_fd >= 0) {
        if (controller->loop != NULL) {
            mesh_event_loop_remove_fd(controller->loop, controller->frame_timer_fd);
        }
        close(controller->frame_timer_fd);
        controller->frame_timer_fd = -1;
    }

    if (controller->backend != NULL && controller->backend->shutdown != NULL) {
        controller->backend->shutdown(controller->backend_state, controller->backend_userdata);
    }

    controller->backend = NULL;
    controller->backend_state = NULL;
    controller->backend_userdata = NULL;
    controller->store = NULL;
    controller->loop = NULL;
}
