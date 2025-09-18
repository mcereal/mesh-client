#include "mesh/app.h"

#include "mesh/log.h"
#include "mesh/transport/ble.h"
#include "mesh/ui/backends/stub.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void mesh_app_publish_ui_state(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }

    struct mesh_transport *ble = mesh_ble_transport();
    if (ble == NULL) {
        return;
    }

    struct mesh_bluez_device_info ble_devices[MESH_UI_MAX_DEVICES];
    size_t device_count = mesh_ble_transport_get_devices(ble, ble_devices, MESH_UI_MAX_DEVICES);

    struct mesh_ui_device ui_devices[MESH_UI_MAX_DEVICES];
    memset(ui_devices, 0, sizeof(ui_devices));

    const char *connected_address = mesh_ble_transport_connected_address(ble);
    bool connected_address_seen = false;

    for (size_t i = 0; i < device_count && i < MESH_UI_MAX_DEVICES; ++i) {
        snprintf(ui_devices[i].identifier, sizeof(ui_devices[i].identifier), "%s", ble_devices[i].address);
        snprintf(ui_devices[i].name, sizeof(ui_devices[i].name), "%s", ble_devices[i].name);
        int16_t rssi = ble_devices[i].rssi;
        if (rssi < INT8_MIN) {
            rssi = INT8_MIN;
        } else if (rssi > INT8_MAX) {
            rssi = INT8_MAX;
        }
        ui_devices[i].rssi = (int8_t)rssi;
        ui_devices[i].connected = (connected_address != NULL && connected_address[0] != '\0' &&
                                   strcmp(connected_address, ble_devices[i].address) == 0);
        if (ui_devices[i].connected) {
            connected_address_seen = true;
        }
    }

    if (connected_address != NULL && connected_address[0] != '\0' && !connected_address_seen &&
        device_count < MESH_UI_MAX_DEVICES) {
        snprintf(ui_devices[device_count].identifier, sizeof(ui_devices[device_count].identifier), "%s",
                 connected_address);
        snprintf(ui_devices[device_count].name, sizeof(ui_devices[device_count].name), "%s", "Connected");
        ui_devices[device_count].rssi = 0;
        ui_devices[device_count].connected = true;
        ++device_count;
    }

    mesh_ui_store_set_discovery(&app->ui_store, ui_devices, device_count);

    struct mesh_ble_handshake_status status = mesh_ble_transport_handshake_status(ble);
    const bool handshake_active =
        status.request_in_flight || status.config_complete || status.has_my_info || status.has_config ||
        (status.node_count > 0U);

    if (handshake_active) {
        struct mesh_ui_handshake_state ui_handshake;
        memset(&ui_handshake, 0, sizeof(ui_handshake));
        ui_handshake.request_in_flight = status.request_in_flight;
        ui_handshake.config_complete = status.config_complete;
        ui_handshake.node_count = (uint32_t)status.node_count;

        if (status.has_my_info) {
            const uint32_t my_node = status.my_info.my_node_num;
            for (size_t i = 0; i < status.node_count && i < MESH_BLE_MAX_NODE_SUMMARY; ++i) {
                if (status.nodes[i].node_id == my_node && status.nodes[i].short_name[0] != '\0') {
                    snprintf(ui_handshake.my_short_name, sizeof(ui_handshake.my_short_name), "%s",
                             status.nodes[i].short_name);
                    break;
                }
            }
        }

        mesh_ui_store_set_handshake(&app->ui_store, &ui_handshake);
    } else {
        mesh_ui_store_set_handshake(&app->ui_store, NULL);
    }
}

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

    result = mesh_ui_store_init(&app->ui_store);
    if (result < 0) {
        mesh_log_error("app", "UI store init failed: %d", result);
        mesh_event_loop_shutdown(&app->loop);
        return result;
    }

    result = mesh_ui_controller_init(&app->ui_controller, &app->ui_store, mesh_ui_backend_stub(), NULL, &app->loop);
    if (result < 0) {
        mesh_log_error("app", "UI controller init failed: %d", result);
        mesh_ui_store_shutdown(&app->ui_store);
        mesh_event_loop_shutdown(&app->loop);
        return result;
    }

    mesh_transport_registry_init(&app->transport_registry);

    result = mesh_transport_registry_register(&app->transport_registry, mesh_ble_transport());
    if (result < 0) {
        mesh_log_error("app", "Failed to register BLE transport: %d", result);
        mesh_ui_controller_shutdown(&app->ui_controller);
        mesh_ui_store_shutdown(&app->ui_store);
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
    mesh_ui_controller_shutdown(&app->ui_controller);
    mesh_ui_store_shutdown(&app->ui_store);
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

    mesh_app_publish_ui_state(app);

    switch (app->config.run_mode) {
        case MESH_APP_RUN_SINGLE_POLL:
            mesh_log_debug("app", "Running single poll with timeout %d ms", app->config.idle_timeout_ms);
            mesh_app_publish_ui_state(app);
            result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
            if (result >= 0) {
                mesh_app_publish_ui_state(app);
            }
            break;
        case MESH_APP_RUN_FOREGROUND:
            mesh_log_info("app", "Starting foreground event loop (timeout %d ms)", app->config.idle_timeout_ms);
            while (true) {
                mesh_transport_registry_tick(&app->transport_registry);
                mesh_app_publish_ui_state(app);
                result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
                if (result < 0) {
                    break;
                }
                if (app->loop.stop_requested) {
                    mesh_log_info("app", "Event loop stop requested");
                    break;
                }
                mesh_app_publish_ui_state(app);
            }
            break;
        default:
            mesh_log_warn("app", "Unknown run mode %d, performing single poll", app->config.run_mode);
            mesh_transport_registry_tick(&app->transport_registry);
            mesh_app_publish_ui_state(app);
            result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
            if (result >= 0) {
                mesh_app_publish_ui_state(app);
            }
            break;
    }

    mesh_transport_registry_stop_all(&app->transport_registry);
    mesh_app_publish_ui_state(app);
    return result;
}
