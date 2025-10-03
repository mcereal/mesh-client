#pragma once

#include "mesh/config.h"
#include "mesh/event_loop.h"
#include "mesh/transport/transport.h"
#include "mesh/ui/controller.h"
#include "mesh/ui/store.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/backends/minui.h"
#include "mesh/ui/backends/fb.h"
#include "mesh/ui/preferences.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_app {
    struct mesh_app_config config;
    struct mesh_event_loop loop;
    struct mesh_transport_registry transport_registry;
    struct mesh_ui_store ui_store;
    struct mesh_ui_controller ui_controller;
    struct mesh_ui_backend_cli_context ui_cli_context;
    struct mesh_ui_backend_minui_context ui_minui_context;
    struct mesh_ui_backend_fb_context ui_fb_context;
    struct mesh_ui_preferences ui_preferences;
    char ui_preferences_path[256];
    char ui_handshake_cache_path[256];
    bool ui_preferences_dirty;
    bool ui_handshake_cache_dirty;
};

int mesh_app_init(struct mesh_app *app, const struct mesh_app_config *config);
void mesh_app_shutdown(struct mesh_app *app);
int mesh_app_run(struct mesh_app *app);

#ifdef __cplusplus
}
#endif
