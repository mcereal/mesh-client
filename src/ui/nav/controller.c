#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/controller.h"

#include "inkcell/ui/focus.h"
#include "inkcell/ui/latency.h"
#include "inkwell/runtime/loop.h"

#include "mesh/ui/focus.h"
#include "mesh/ui/nav.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* The store, as the scheduler sees it: a descriptor, a drain and a nudge. */
static bool mesh_ui_controller_drain(void *userdata, void *snapshot) {
    return mesh_ui_store_consume_updates((struct mesh_ui_store *)userdata,
                                         (struct mesh_ui_snapshot *)snapshot);
}

/* A timer frame re-presents the last snapshot, and nothing in it changed. */
static void mesh_ui_controller_unchanged(void *userdata, void *snapshot) {
    (void)userdata;
    ((struct mesh_ui_snapshot *)snapshot)->update_flags = MESH_UI_UPDATE_NONE;
}

static void mesh_ui_controller_refresh(void *userdata) {
    mesh_ui_store_request_refresh((struct mesh_ui_store *)userdata);
}

int mesh_ui_controller_init(struct mesh_ui_controller *controller, struct mesh_ui_store *store,
                            const struct inkcell_backend *backend, void *backend_userdata,
                            struct inkwell_loop *loop) {
    if (controller == NULL || store == NULL) {
        return -EINVAL;
    }

    memset(controller, 0, sizeof *controller);
    controller->store = store;
    controller->loop = loop;

    const struct inkstand_frame_config frames = {
        .loop = loop,
        .backend = backend,
        .backend_userdata = backend_userdata,
        .snapshot = &controller->snapshot,
        .wake_fd = mesh_ui_store_event_fd(store),
        .drain = mesh_ui_controller_drain,
        .unchanged = mesh_ui_controller_unchanged,
        .refresh = mesh_ui_controller_refresh,
        .source_userdata = store,
        .interval_ms = MESH_UI_FRAME_INTERVAL_MS,
    };
    const int result = inkstand_frame_scheduler_init(&controller->frames, &frames);
    if (result < 0) {
        controller->store = NULL;
        controller->loop = NULL;
    }
    return result;
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
    uint32_t rows = 0U;
    if (inkstand_frame_scheduler_page_rows(&controller->frames, &rows)) {
        mesh_ui_store_set_page_rows(controller->store, rows);
    }
    const struct inkcell_focus_map *map = NULL;
    if (inkstand_frame_scheduler_focus_map(&controller->frames, &map)) {
        mesh_ui_store_set_focus_map(controller->store, map);
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

void mesh_ui_controller_handle_text(struct mesh_ui_controller *controller, const char *text) {
    if (controller == NULL || controller->store == NULL) {
        return;
    }
    inkcell_latency_press_handled(mesh_ui_store_insert_text(controller->store, text));
}

static void mesh_ui_controller_dispatch_command(struct mesh_ui_controller *controller,
                                                enum mesh_ui_command_id command,
                                                enum mesh_ui_command_direction direction,
                                                bool dismiss_context) {
    if (controller == NULL || controller->store == NULL ||
        !inkstand_frame_scheduler_presented(&controller->frames) ||
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

void mesh_ui_controller_handle_shortcut(struct mesh_ui_controller *controller, char letter) {
    enum mesh_ui_command_id command = MESH_UI_COMMAND_NONE;
    switch (letter) {
    case 'n':
        command = MESH_UI_COMMAND_NEW;
        break;
    case 'r':
        command = MESH_UI_COMMAND_REFRESH;
        break;
    case 's':
        command = MESH_UI_COMMAND_SAVE;
        break;
    default:
        inkcell_latency_press_handled(false);
        return;
    }
    mesh_ui_controller_handle_command(controller, command, MESH_UI_COMMAND_DIRECTION_NONE);
}

void mesh_ui_controller_handle_action_key(struct mesh_ui_controller *controller,
                                          enum inkcell_key key) {
    if (controller == NULL || !inkstand_frame_scheduler_presented(&controller->frames) ||
        key == INKCELL_KEY_NONE) {
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
    if (inkstand_frame_scheduler_presented(&controller->frames) &&
        controller->snapshot.nav.context_open && target > (uint32_t)MESH_UI_FOCUS_MENU &&
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
    return controller != NULL && inkstand_frame_scheduler_has_backend(&controller->frames);
}

void mesh_ui_controller_shutdown(struct mesh_ui_controller *controller) {
    if (controller == NULL) {
        return;
    }
    inkstand_frame_scheduler_shutdown(&controller->frames);
    controller->store = NULL;
    controller->loop = NULL;
}

void mesh_ui_controller_request_frame(struct mesh_ui_controller *controller) {
    if (controller != NULL) {
        inkstand_frame_scheduler_request_frame(&controller->frames);
    }
}

bool mesh_ui_controller_settled(const struct mesh_ui_controller *controller) {
    return controller == NULL || inkstand_frame_scheduler_settled(&controller->frames);
}

bool mesh_ui_controller_frame(const struct mesh_ui_controller *controller,
                              struct inkcell_surface *out) {
    return controller != NULL && inkstand_frame_scheduler_frame(&controller->frames, out);
}
