#include "mesh/app.h"

#include "mesh/log.h"
#include "mesh/transport/ble.h"

#include <errno.h>
#include <string.h>

int mesh_app_init(struct mesh_app *app, const struct mesh_app_config *config) {
    if (app == NULL) {
        return -EINVAL;
    }

    if (config != NULL) {
        app->config = *config;
    } else {
        app->config = mesh_app_config_default();
        mesh_app_config_apply_env_overrides(&app->config);
    }

    int result = mesh_event_loop_init(&app->loop);
    if (result < 0) {
        mesh_log_error("app", "Event loop init failed: %d", result);
        return result;
    }

    mesh_transport_registry_init(&app->transport_registry);

    result = mesh_transport_registry_register(&app->transport_registry, mesh_ble_transport());
    if (result < 0) {
        mesh_log_error("app", "Failed to register BLE transport: %d", result);
        mesh_event_loop_shutdown(&app->loop);
        return result;
    }

    return 0;
}

void mesh_app_shutdown(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }

    mesh_transport_registry_stop_all(&app->transport_registry);
    mesh_event_loop_shutdown(&app->loop);
}

int mesh_app_run(struct mesh_app *app) {
    if (app == NULL) {
        return -EINVAL;
    }

    int result = mesh_transport_registry_start_all(&app->transport_registry, &app->config, &app->loop);
    if (result < 0) {
        return result;
    }

    switch (app->config.run_mode) {
        case MESH_APP_RUN_SINGLE_POLL:
            mesh_log_debug("app", "Running single poll with timeout %d ms", app->config.idle_timeout_ms);
            result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
            break;
        case MESH_APP_RUN_FOREGROUND:
            mesh_log_info("app", "Starting foreground event loop (timeout %d ms)", app->config.idle_timeout_ms);
            while (true) {
                mesh_transport_registry_tick(&app->transport_registry);
                result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
                if (result < 0) {
                    break;
                }
                if (app->loop.stop_requested) {
                    mesh_log_info("app", "Event loop stop requested");
                    break;
                }
            }
            break;
        default:
            mesh_log_warn("app", "Unknown run mode %d, performing single poll", app->config.run_mode);
            mesh_transport_registry_tick(&app->transport_registry);
            result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
            break;
    }

    mesh_transport_registry_stop_all(&app->transport_registry);
    return result;
}
