#pragma once

#include "mesh/config.h"
#include "mesh/event_loop.h"
#include "mesh/transport/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_app {
    struct mesh_app_config config;
    struct mesh_event_loop loop;
    struct mesh_transport_registry transport_registry;
};

int mesh_app_init(struct mesh_app *app, const struct mesh_app_config *config);
void mesh_app_shutdown(struct mesh_app *app);
int mesh_app_run(struct mesh_app *app);

#ifdef __cplusplus
}
#endif
