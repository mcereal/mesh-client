#include "mesh/app.h"

#include "mesh/log.h"
#include "mesh/transport/ble.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/backends/minui.h"
#include "mesh/ui/backends/stub.h"
#include "mesh/ui/preferences.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

static void mesh_app_minui_on_device_selected(void *userdata, const char *identifier) {
    if (userdata == NULL || identifier == NULL || identifier[0] == '\0') {
        return;
    }

    struct mesh_app *app = (struct mesh_app *)userdata;
    struct mesh_transport *ble = mesh_ble_transport();
    if (ble == NULL) {
        mesh_log_warn("ui", "BLE transport unavailable for MinUI selection");
        return;
    }

    mesh_log_info("ui", "MinUI selection requested connect to %s", identifier);
    snprintf(app->config.preferred_ble_device, sizeof app->config.preferred_ble_device, "%s", identifier);
    snprintf(app->ui_preferences.preferred_device, sizeof app->ui_preferences.preferred_device, "%s", identifier);
    app->ui_preferences_dirty = true;

    int connect_result = mesh_ble_transport_connect(ble, identifier);
    if (connect_result < 0 && connect_result != -EALREADY) {
        mesh_log_warn("ui", "Failed to connect to %s via MinUI (%d)", identifier, connect_result);
    }
}

static bool mesh_app_select_minui(struct mesh_app *app, const struct mesh_ui_backend **backend, void **userdata,
                                  bool log_on_missing) {
    if (!mesh_ui_backend_minui_is_available()) {
        if (log_on_missing) {
            mesh_log_warn("ui", "MinUI helpers not found; falling back to CLI backend");
        }
        return false;
    }

    if (backend != NULL) {
        *backend = mesh_ui_backend_minui();
    }
    if (userdata != NULL) {
        *userdata = &app->ui_minui_context;
    }
    app->ui_minui_context.loop = &app->loop;
    app->ui_minui_context.on_device_selected = mesh_app_minui_on_device_selected;
    app->ui_minui_context.callback_userdata = app;
    return true;
}

static void mesh_app_select_cli(struct mesh_app *app, const struct mesh_ui_backend **backend, void **userdata) {
    if (backend != NULL) {
        *backend = mesh_ui_backend_cli();
    }
    if (userdata != NULL) {
        *userdata = &app->ui_cli_context;
    }
}

static void mesh_app_select_stub(const struct mesh_ui_backend **backend, void **userdata) {
    if (backend != NULL) {
        *backend = mesh_ui_backend_stub();
    }
    if (userdata != NULL) {
        *userdata = NULL;
    }
}

static const struct mesh_ui_backend *mesh_app_select_backend(struct mesh_app *app, void **userdata) {
    if (userdata != NULL) {
        *userdata = NULL;
    }

    const char *requested = getenv("MESHCLIENT_UI_BACKEND");
    if (requested != NULL && requested[0] == '\0') {
        requested = NULL;
    }

    const struct mesh_ui_backend *backend = NULL;
    void *backend_userdata = NULL;

    if (requested != NULL) {
        if (strcasecmp(requested, "minui") == 0) {
            if (!mesh_app_select_minui(app, &backend, &backend_userdata, true)) {
                mesh_app_select_cli(app, &backend, &backend_userdata);
            }
        } else if (strcasecmp(requested, "cli") == 0) {
            mesh_app_select_cli(app, &backend, &backend_userdata);
        } else if (strcasecmp(requested, "stub") == 0) {
            mesh_app_select_stub(&backend, &backend_userdata);
        } else if (strcasecmp(requested, "auto") == 0) {
            if (!mesh_app_select_minui(app, &backend, &backend_userdata, false)) {
                mesh_app_select_cli(app, &backend, &backend_userdata);
            }
        } else {
            mesh_log_warn("ui", "Unknown UI backend '%s'; defaulting to CLI", requested);
            mesh_app_select_cli(app, &backend, &backend_userdata);
        }
    } else {
        const char *platform = getenv("PLATFORM");
        bool prefer_minui = (platform != NULL && strcasecmp(platform, "tg5040") == 0);
        if (!prefer_minui || !mesh_app_select_minui(app, &backend, &backend_userdata, false)) {
            if (!mesh_app_select_minui(app, &backend, &backend_userdata, false)) {
                mesh_app_select_cli(app, &backend, &backend_userdata);
            }
        }
    }

    if (backend == NULL) {
        mesh_app_select_cli(app, &backend, &backend_userdata);
    }

    if (userdata != NULL) {
        *userdata = backend_userdata;
    }

    return backend;
}

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

    bool preferences_modified = false;
    if (connected_address != NULL && connected_address[0] != '\0') {
        if (strcmp(app->ui_preferences.preferred_device, connected_address) != 0) {
            snprintf(app->ui_preferences.preferred_device, sizeof app->ui_preferences.preferred_device, "%s",
                     connected_address);
            preferences_modified = true;
        }
    }

    struct mesh_ble_handshake_status status = mesh_ble_transport_handshake_status(ble);
    const bool handshake_active =
        status.request_in_flight || status.config_complete || status.has_my_info || status.has_config ||
        (status.node_count > 0U);

    if (handshake_active) {
        struct mesh_ui_handshake_state ui_handshake;
        memset(&ui_handshake, 0, sizeof(ui_handshake));
        ui_handshake.request_in_flight = status.request_in_flight;
        ui_handshake.request_id = status.request_id;
        ui_handshake.config_complete = status.config_complete;
        ui_handshake.config_complete_id = status.config_complete_id;
        ui_handshake.has_my_info = status.has_my_info;
        ui_handshake.has_config = status.has_config;
        ui_handshake.cached = false;
        if (status.has_my_info) {
            const uint32_t my_node = status.my_info.my_node_num;
            ui_handshake.my_info.node_num = status.my_info.my_node_num;
            ui_handshake.my_info.nodedb_entries = status.my_info.nodedb_count;
            ui_handshake.my_info.reboot_count = status.my_info.reboot_count;
            for (size_t i = 0; i < status.node_count && i < MESH_BLE_MAX_NODE_SUMMARY; ++i) {
                if (status.nodes[i].node_id == my_node && status.nodes[i].short_name[0] != '\0') {
                    snprintf(ui_handshake.my_short_name, sizeof(ui_handshake.my_short_name), "%s",
                             status.nodes[i].short_name);
                    break;
                }
            }
        }

        size_t copy_count = status.node_count;
        if (copy_count > MESH_UI_MAX_HANDSHAKE_NODES) {
            copy_count = MESH_UI_MAX_HANDSHAKE_NODES;
        }
        for (size_t i = 0; i < copy_count; ++i) {
            const struct mesh_ble_node_summary *src = &status.nodes[i];
            struct mesh_ui_node_summary *dst = &ui_handshake.nodes[i];
            dst->node_id = src->node_id;
            snprintf(dst->long_name, sizeof(dst->long_name), "%s", src->long_name);
            snprintf(dst->short_name, sizeof(dst->short_name), "%s", src->short_name);
            dst->last_heard = src->last_heard;
            dst->snr = src->snr;
            dst->via_mqtt = src->via_mqtt;
            dst->has_hops_away = src->has_hops_away;
            dst->hops_away = src->hops_away;
        }
        ui_handshake.node_count = (uint32_t)copy_count;

        mesh_ui_update_flags prev_flags = app->ui_store.pending_flags;
        mesh_ui_store_set_handshake(&app->ui_store, &ui_handshake);
        if (app->ui_handshake_cache_path[0] != '\0' &&
            (app->ui_store.pending_flags & MESH_UI_UPDATE_HANDSHAKE) != 0U &&
            (prev_flags & MESH_UI_UPDATE_HANDSHAKE) == 0U) {
            app->ui_handshake_cache_dirty = true;
        }

        if (ui_handshake.primary_channel[0] != '\0' &&
            strcmp(app->ui_preferences.preferred_channel, ui_handshake.primary_channel) != 0) {
            snprintf(app->ui_preferences.preferred_channel, sizeof app->ui_preferences.preferred_channel, "%s",
                     ui_handshake.primary_channel);
            preferences_modified = true;
        }
    } else {
        mesh_ui_update_flags prev_flags = app->ui_store.pending_flags;
        mesh_ui_store_set_handshake(&app->ui_store, NULL);
        if (app->ui_handshake_cache_path[0] != '\0' &&
            (app->ui_store.pending_flags & MESH_UI_UPDATE_HANDSHAKE) != 0U &&
            (prev_flags & MESH_UI_UPDATE_HANDSHAKE) == 0U) {
            app->ui_handshake_cache_dirty = true;
        }
    }

    if (preferences_modified && app->ui_preferences_path[0] != '\0') {
        if (mesh_ui_preferences_save(&app->ui_preferences, app->ui_preferences_path) == 0) {
            app->ui_preferences_dirty = false;
        } else {
            app->ui_preferences_dirty = true;
        }
    } else if (app->ui_preferences_dirty && app->ui_preferences_path[0] != '\0') {
        if (mesh_ui_preferences_save(&app->ui_preferences, app->ui_preferences_path) == 0) {
            app->ui_preferences_dirty = false;
        }
    }

    if (app->ui_handshake_cache_dirty && app->ui_handshake_cache_path[0] != '\0') {
        int save_handshake = mesh_ui_store_save(&app->ui_store, app->ui_handshake_cache_path);
        if (save_handshake == 0) {
            app->ui_handshake_cache_dirty = false;
        } else {
            mesh_log_debug("app", "Failed to persist handshake cache: %d", save_handshake);
        }
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

    memset(&app->ui_minui_context, 0, sizeof app->ui_minui_context);
    memset(&app->ui_preferences, 0, sizeof(app->ui_preferences));
    app->ui_preferences_path[0] = '\0';
    app->ui_preferences_dirty = false;
    app->ui_handshake_cache_path[0] = '\0';
    app->ui_handshake_cache_dirty = false;

    if (mesh_ui_preferences_default_path(app->ui_preferences_path, sizeof(app->ui_preferences_path)) == 0) {
        int load_result = mesh_ui_preferences_load(&app->ui_preferences, app->ui_preferences_path);
        if (load_result == 0) {
            if (app->config.preferred_ble_device[0] == '\0' &&
                app->ui_preferences.preferred_device[0] != '\0') {
                snprintf(app->config.preferred_ble_device, sizeof app->config.preferred_ble_device, "%s",
                         app->ui_preferences.preferred_device);
            }
        }
        int handshake_written = snprintf(app->ui_handshake_cache_path, sizeof(app->ui_handshake_cache_path),
                                         "%s.handshake", app->ui_preferences_path);
        if (handshake_written < 0 || handshake_written >= (int)sizeof(app->ui_handshake_cache_path)) {
            mesh_log_warn("app", "Handshake cache path truncated; disabling cache");
            app->ui_handshake_cache_path[0] = '\0';
        }
    }

    result = mesh_ui_store_init(&app->ui_store);
    if (result < 0) {
        mesh_log_error("app", "UI store init failed: %d", result);
        mesh_event_loop_shutdown(&app->loop);
        return result;
    }

    if (app->ui_handshake_cache_path[0] != '\0') {
        int handshake_load = mesh_ui_store_load(&app->ui_store, app->ui_handshake_cache_path);
        if (handshake_load < 0 && handshake_load != -ENOENT) {
            mesh_log_debug("app", "Failed to load handshake cache: %d", handshake_load);
        }
    }

    void *backend_userdata = NULL;
    const struct mesh_ui_backend *ui_backend = mesh_app_select_backend(app, &backend_userdata);
    result = mesh_ui_controller_init(&app->ui_controller, &app->ui_store, ui_backend, backend_userdata, &app->loop);
    if (result < 0) {
        mesh_log_warn("app", "UI backend init failed (%d); falling back to stub", result);
        result = mesh_ui_controller_init(&app->ui_controller, &app->ui_store, mesh_ui_backend_stub(), NULL,
                                         &app->loop);
    }
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
    if (app->ui_handshake_cache_path[0] != '\0') {
        mesh_ui_store_save(&app->ui_store, app->ui_handshake_cache_path);
        app->ui_handshake_cache_dirty = false;
    }
    mesh_ui_store_shutdown(&app->ui_store);
    if (app->ui_preferences_dirty && app->ui_preferences_path[0] != '\0') {
        mesh_ui_preferences_save(&app->ui_preferences, app->ui_preferences_path);
        app->ui_preferences_dirty = false;
    }
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
