#pragma once

#include "mesh/event_loop.h"
#include "mesh/ui/store.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_backend;

struct mesh_ui_controller {
    struct mesh_ui_store *store;
    const struct mesh_ui_backend *backend;
    void *backend_state;
    void *backend_userdata;
    struct mesh_event_loop *loop;
    bool registered;
};

int mesh_ui_controller_init(struct mesh_ui_controller *controller, struct mesh_ui_store *store,
                            const struct mesh_ui_backend *backend, void *backend_userdata,
                            struct mesh_event_loop *loop);
void mesh_ui_controller_shutdown(struct mesh_ui_controller *controller);

#ifdef __cplusplus
}
#endif

