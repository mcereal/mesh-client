#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/controller.h"

#include "inkcell/ui/backend.h"
#include "inkcell/ui/latency.h"
#include "inkcell/utils/log.h"

#include "mesh/ui/nav.h"

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
        inkcell_log_warn("ui", "frame timerfd_settime failed: %s", strerror(errno));
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
        inkcell_log_warn("ui", "frame timer read failed: %s", strerror(errno));
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
        inkcell_log_warn("ui", "frame timerfd_create failed: %s", strerror(errno));
        return;
    }
    if (mesh_event_loop_add_fd(loop, fd, EPOLLIN, mesh_ui_controller_frame_callback, controller) <
        0) {
        inkcell_log_warn("ui", "Failed to watch the frame timer");
        close(fd);
        return;
    }
    controller->frame_timer_fd = fd;
}

int mesh_ui_controller_init(struct mesh_ui_controller *controller, struct mesh_ui_store *store,
                            const struct inkcell_backend *backend, void *backend_userdata,
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
            inkcell_log_error("ui", "Backend init failed (%s): %d",
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
            inkcell_log_error("ui", "Failed to register UI store fd: %d", add_result);
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

void mesh_ui_controller_handle_key(struct mesh_ui_controller *controller, enum inkcell_key key) {
    if (controller == NULL || controller->store == NULL || key == INKCELL_KEY_NONE) {
        return;
    }

    /* The window the press is judged against is the one on the panel now, and so are the boxes
       it is resolved against: both are facts about the frame the reader was looking at when
       they pressed, and both are read back rather than guessed at. */
    if (controller->backend != NULL && controller->backend->page_rows != NULL) {
        mesh_ui_store_set_page_rows(controller->store,
                                    controller->backend->page_rows(controller->backend_state,
                                                                   controller->backend_userdata));
    }
    if (controller->backend != NULL && controller->backend->focus_map != NULL) {
        mesh_ui_store_set_focus_map(controller->store,
                                    controller->backend->focus_map(controller->backend_state,
                                                                   controller->backend_userdata));
    }
    struct mesh_ui_action action;
    const bool repaints = mesh_ui_store_handle_key(controller->store, key, &action);
    /*
     * The latency probe's press is confirmed here rather than where the button was read,
     * because only the store knows whether the press changed anything. One that did not - Down
     * at the end of a list, a button a screen does not use - publishes no snapshot and draws no
     * frame, so the next frame to arrive belongs to something else: an animation settling, or
     * the map's fill loop asking for another turn. Charged to the press, that frame would be a
     * latency nobody ever waited.
     */
    inkcell_latency_press_handled(repaints);
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
