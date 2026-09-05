#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/controller.h"

#include "mesh/ui/backend.h"
#include "mesh/ui/nav.h"
#include "mesh/utils/log.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/epoll.h>

static int mesh_ui_controller_event_callback(int fd, uint32_t events, void *userdata) {
    (void)fd;
    struct mesh_ui_controller *controller = (struct mesh_ui_controller *)userdata;
    if (controller == NULL) {
        return 0;
    }

    if ((events & EPOLLIN) == 0U) {
        return 0;
    }

    struct mesh_ui_snapshot snapshot;
    while (mesh_ui_store_consume_updates(controller->store, &snapshot)) {
        if (controller->backend != NULL && controller->backend->present != NULL) {
            controller->backend->present(controller->backend_state, &snapshot,
                                         controller->backend_userdata);
        }
    }

    return 0;
}

int mesh_ui_controller_init(struct mesh_ui_controller *controller, struct mesh_ui_store *store,
                            const struct mesh_ui_backend *backend, void *backend_userdata,
                            struct mesh_event_loop *loop) {
    if (controller == NULL || store == NULL) {
        return -EINVAL;
    }

    memset(controller, 0, sizeof *controller);
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

    if (controller->backend != NULL && controller->backend->shutdown != NULL) {
        controller->backend->shutdown(controller->backend_state, controller->backend_userdata);
    }

    controller->backend = NULL;
    controller->backend_state = NULL;
    controller->backend_userdata = NULL;
    controller->store = NULL;
    controller->loop = NULL;
}
