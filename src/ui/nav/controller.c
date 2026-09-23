#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/controller.h"

#include "inkcell/ui/backend.h"
#include "inkcell/ui/focus.h"
#include "inkcell/ui/latency.h"
#include "inkwell/base/log.h"
#include "inkwell/runtime/loop.h"
#include "inkwell/runtime/timer.h"

#include "mesh/ui/focus.h"
#include "mesh/ui/nav.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

/*
 * Asks for one more frame in MESH_UI_FRAME_INTERVAL_MS, or stops asking.
 *
 * A zero delay disarms, so "still moving" and "settled" are the same call with a different
 * answer from the backend - there is no separate stop path to forget to take.
 */
static void mesh_ui_controller_schedule_frame(struct mesh_ui_controller *controller, bool moving) {
    if (controller->frame_timer_fd < 0) {
        return;
    }

    const int armed = inkwell_timer_arm_once(controller->frame_timer_fd,
                                             moving ? (uint32_t)MESH_UI_FRAME_INTERVAL_MS : 0U);
    if (armed < 0) {
        inkwell_log_warn("ui", "arming the frame timer failed: %s", strerror(-armed));
    }
    controller->frame_armed = moving && armed >= 0;
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

    if ((events & INKWELL_LOOP_IN) == 0U) {
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
    if (controller == NULL || (events & INKWELL_LOOP_IN) == 0U) {
        return 0;
    }

    controller->frame_armed = false;
    const int64_t expired = inkwell_timer_read(fd);
    if (expired < 0) {
        inkwell_log_warn("ui", "frame timer read failed: %s", strerror((int)-expired));
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
                                                 struct inkwell_loop *loop) {
    controller->frame_timer_fd = -1;
    if (loop == NULL || controller->backend == NULL || controller->backend->animating == NULL) {
        return;
    }

    const int fd = inkwell_timer_open();
    if (fd < 0) {
        inkwell_log_warn("ui", "creating the frame timer failed: %s", strerror(-fd));
        return;
    }
    if (inkwell_loop_add_fd(loop, fd, INKWELL_LOOP_IN, mesh_ui_controller_frame_callback,
                            controller) < 0) {
        inkwell_log_warn("ui", "Failed to watch the frame timer");
        close(fd);
        return;
    }
    controller->frame_timer_fd = fd;
}

int mesh_ui_controller_init(struct mesh_ui_controller *controller, struct mesh_ui_store *store,
                            const struct inkcell_backend *backend, void *backend_userdata,
                            struct inkwell_loop *loop) {
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
            inkwell_log_error("ui", "Backend init failed (%s): %d",
                              backend->name != NULL ? backend->name : "unknown", result);
            controller->backend = NULL;
            controller->backend_state = NULL;
            controller->backend_userdata = NULL;
        }
    }

    const int event_fd = mesh_ui_store_event_fd(store);
    if (loop != NULL && event_fd >= 0) {
        int add_result = inkwell_loop_add_fd(loop, event_fd, INKWELL_LOOP_IN,
                                             mesh_ui_controller_event_callback, controller);
        if (add_result < 0) {
            inkwell_log_error("ui", "Failed to register UI store fd: %d", add_result);
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

/* The window the press is judged against is the one on the panel now, and so are the boxes it
   is resolved against: both are facts about the frame the reader was looking at when they
   pressed, and both are read back rather than guessed at. */
static void mesh_ui_controller_read_frame(struct mesh_ui_controller *controller) {
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
}

static void mesh_ui_controller_dispatch_key(struct mesh_ui_controller *controller,
                                            enum inkcell_key key, bool dismiss_context) {
    if (controller == NULL || controller->store == NULL || key == INKCELL_KEY_NONE) {
        return;
    }
    const bool dismissed = dismiss_context && mesh_ui_store_dismiss_context(controller->store);
    mesh_ui_controller_read_frame(controller);
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
    inkcell_latency_press_handled(repaints || dismissed);
    if (action.type != MESH_UI_ACTION_NONE && controller->on_action != NULL) {
        controller->on_action(controller->action_userdata, &action);
    }
}

void mesh_ui_controller_handle_key(struct mesh_ui_controller *controller, enum inkcell_key key) {
    mesh_ui_controller_dispatch_key(controller, key, false);
}

static void mesh_ui_controller_dispatch_command(struct mesh_ui_controller *controller,
                                                enum mesh_ui_command_id command,
                                                enum mesh_ui_command_direction direction,
                                                bool dismiss_context) {
    if (controller == NULL || controller->store == NULL || !controller->snapshot_valid ||
        command == MESH_UI_COMMAND_NONE) {
        return;
    }
    /*
     * Commands name the frame the user saw. Once the store has unpublished changes, its cursor
     * or row may no longer name the same subject; this is the command equivalent of the stale
     * click guard in mesh_ui_store_handle_click(). The next loop turn presents a fresh command
     * set, so dropping it is safer than running a verb on a row that was never on screen.
     */
    if (controller->store->pending_flags != MESH_UI_UPDATE_NONE) {
        inkcell_latency_press_handled(false);
        return;
    }

    struct mesh_ui_command_set offered;
    mesh_ui_commands_for(&controller->snapshot, &offered);
    const struct mesh_ui_command *binding = mesh_ui_commands_find(&offered, command);
    if (binding == NULL) {
        inkcell_latency_press_handled(false);
        return;
    }

    /*
     * QUIT deliberately has no logical key: input hosts stop the loop before delivering one.
     * A semantic menu still needs to invoke the offered command, at the same ownership layer.
     */
    if (binding->button == INKCELL_BUTTON_QUIT) {
        if (controller->loop != NULL) {
            inkwell_loop_request_stop(controller->loop);
        }
        return;
    }

    enum inkcell_key keys[2];
    const size_t count = inkcell_button_keys(binding->button, keys);
    enum inkcell_key key = INKCELL_KEY_NONE;
    if (count == 1U) {
        key = keys[0];
    } else if (count == 2U && direction == MESH_UI_COMMAND_PREVIOUS) {
        key = keys[0];
    } else if (count == 2U && direction == MESH_UI_COMMAND_NEXT) {
        key = keys[1];
    }
    if (key == INKCELL_KEY_NONE) {
        inkcell_latency_press_handled(false);
        return;
    }
    mesh_ui_controller_dispatch_key(controller, key, dismiss_context);
}

void mesh_ui_controller_handle_command(struct mesh_ui_controller *controller,
                                       enum mesh_ui_command_id command,
                                       enum mesh_ui_command_direction direction) {
    mesh_ui_controller_dispatch_command(controller, command, direction, false);
}

void mesh_ui_controller_handle_action_key(struct mesh_ui_controller *controller,
                                          enum inkcell_key key) {
    if (controller == NULL || !controller->snapshot_valid || key == INKCELL_KEY_NONE) {
        return;
    }

    struct mesh_ui_command_set offered;
    mesh_ui_commands_for(&controller->snapshot, &offered);
    for (size_t i = 0U; i < offered.count; ++i) {
        enum inkcell_key keys[2];
        const size_t count = inkcell_button_keys(offered.items[i].button, keys);
        if (count == 1U && keys[0] == key) {
            mesh_ui_controller_handle_command(controller, offered.items[i].id,
                                              MESH_UI_COMMAND_DIRECTION_NONE);
            return;
        }
        if (count == 2U && keys[0] == key) {
            mesh_ui_controller_handle_command(controller, offered.items[i].id,
                                              MESH_UI_COMMAND_PREVIOUS);
            return;
        }
        if (count == 2U && keys[1] == key) {
            mesh_ui_controller_handle_command(controller, offered.items[i].id,
                                              MESH_UI_COMMAND_NEXT);
            return;
        }
    }
    inkcell_latency_press_handled(false);
}

void mesh_ui_controller_handle_click(struct mesh_ui_controller *controller, uint32_t target) {
    if (controller == NULL || controller->store == NULL || target == INKCELL_FOCUS_NONE) {
        return;
    }
    if (controller->snapshot_valid && controller->snapshot.nav.context_open &&
        target > (uint32_t)MESH_UI_FOCUS_MENU &&
        target < (uint32_t)MESH_UI_FOCUS_MENU + (uint32_t)MESH_UI_COMMAND_COUNT) {
        const enum mesh_ui_command_id command =
            (enum mesh_ui_command_id)(target - (uint32_t)MESH_UI_FOCUS_MENU);
        struct mesh_ui_command_set offered;
        mesh_ui_commands_for(&controller->snapshot, &offered);
        const struct mesh_ui_command *const binding = mesh_ui_commands_find(&offered, command);
        if (mesh_ui_command_context_order(binding) >= 0) {
            mesh_ui_controller_dispatch_command(controller, command, MESH_UI_COMMAND_DIRECTION_NONE,
                                                true);
            return;
        }
    }
    mesh_ui_controller_read_frame(controller);
    struct mesh_ui_action action;
    /* Confirmed here for the key's reason: only the store knows whether the click did anything. */
    inkcell_latency_press_handled(mesh_ui_store_handle_click(controller->store, target, &action));
    if (action.type != MESH_UI_ACTION_NONE && controller->on_action != NULL) {
        controller->on_action(controller->action_userdata, &action);
    }
}

void mesh_ui_controller_handle_context(struct mesh_ui_controller *controller, uint32_t target,
                                       int x, int y) {
    if (controller == NULL || controller->store == NULL) {
        return;
    }
    mesh_ui_controller_read_frame(controller);
    inkcell_latency_press_handled(mesh_ui_store_handle_context(controller->store, target, x, y));
}

bool mesh_ui_controller_has_backend(const struct mesh_ui_controller *controller) {
    return controller != NULL && controller->backend != NULL;
}

void mesh_ui_controller_shutdown(struct mesh_ui_controller *controller) {
    if (controller == NULL) {
        return;
    }

    if (controller->loop != NULL && controller->registered) {
        const int event_fd = mesh_ui_store_event_fd(controller->store);
        inkwell_loop_remove_fd(controller->loop, event_fd);
        controller->registered = false;
    }

    if (controller->frame_timer_fd >= 0) {
        if (controller->loop != NULL) {
            inkwell_loop_remove_fd(controller->loop, controller->frame_timer_fd);
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

/*
 * A frame already on its way is the frame asked for, so it is left where it is. Re-arming is a
 * one-shot timer's deadline moved back, and a window resizing faster than the frame interval
 * would move it back every time - no frame until the resizing stopped.
 */
void mesh_ui_controller_request_frame(struct mesh_ui_controller *controller) {
    if (controller != NULL && !controller->frame_armed) {
        mesh_ui_controller_schedule_frame(controller, true);
    }
}

bool mesh_ui_controller_settled(const struct mesh_ui_controller *controller) {
    return controller == NULL || !controller->frame_armed;
}

bool mesh_ui_controller_frame(const struct mesh_ui_controller *controller,
                              struct inkcell_surface *out) {
    return controller != NULL && controller->backend != NULL &&
           controller->backend->frame != NULL &&
           controller->backend->frame(controller->backend_state, controller->backend_userdata, out);
}
