#define _POSIX_C_SOURCE 200809L

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
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

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
    snprintf(app->config.preferred_ble_device, sizeof app->config.preferred_ble_device, "%s",
             identifier);
    snprintf(app->ui_preferences.preferred_device, sizeof app->ui_preferences.preferred_device,
             "%s", identifier);
    app->ui_preferences_dirty = true;

    int connect_result = mesh_ble_transport_connect(ble, identifier);
    if (connect_result < 0 && connect_result != -EALREADY) {
        mesh_log_warn("ui", "Failed to connect to %s via MinUI (%d)", identifier, connect_result);
    }
}

/* Button presses arrive here from the evdev reader and go straight into the UI store's
   navigation model; the repaint happens on the store's eventfd in the same loop turn. */
static void mesh_app_on_ui_key(void *userdata, enum mesh_ui_key key) {
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app == NULL) {
        return;
    }
    mesh_ui_controller_handle_key(&app->ui_controller, key);
}

static uint64_t mesh_app_now_ms(void);

/* What the navigation model cannot do by itself: talk to the radio. */
static void mesh_app_on_ui_action(void *userdata, const struct mesh_ui_action *action) {
    struct mesh_app *app = (struct mesh_app *)userdata;
    if (app == NULL || action == NULL) {
        return;
    }

    struct mesh_transport *ble = mesh_ble_transport();
    char toast[MESH_UI_NAV_TOAST_MAX];
    const uint64_t now = mesh_app_now_ms();

    switch (action->type) {
    case MESH_UI_ACTION_CONNECT: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, "BLE transport unavailable");
            return;
        }
        mesh_log_info("ui", "Connect to %s requested from the device", action->identifier);
        /* Same bookkeeping as a MinUI pick: this becomes the node auto-connect goes back to. */
        snprintf(app->config.preferred_ble_device, sizeof app->config.preferred_ble_device, "%s",
                 action->identifier);
        snprintf(app->ui_preferences.preferred_device, sizeof app->ui_preferences.preferred_device,
                 "%s", action->identifier);
        app->ui_preferences_dirty = true;

        /* A user pick beats whatever auto-connect is doing or has done. */
        if (mesh_ble_transport_connected_address(ble) != NULL ||
            mesh_ble_transport_is_connecting(ble)) {
            mesh_ble_transport_disconnect(ble);
        }
        const int result = mesh_ble_transport_connect(ble, action->identifier);
        if (result == 0 || result == -EALREADY || result == -EINPROGRESS) {
            snprintf(toast, sizeof toast, "Connecting to %.40s", action->identifier);
        } else {
            snprintf(toast, sizeof toast, "Connect failed (%d)", result);
            mesh_log_warn("ui", "Connect to %s failed: %d", action->identifier, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_SEND_TEXT: {
        if (ble == NULL) {
            mesh_ui_store_set_toast(&app->ui_store, now, "BLE transport unavailable");
            return;
        }
        const bool broadcast = (action->dest == MESH_MESSAGE_BROADCAST_ADDR);
        uint32_t packet_id = 0U;
        const int result = mesh_ble_transport_send_text(ble, action->dest, action->channel,
                                                        action->text, !broadcast, &packet_id);
        if (result == 0) {
            snprintf(toast, sizeof toast, "Sent to %s", app->ui_store.nav.target_name);
            mesh_log_info("ui", "Sent \"%s\" to %s (packet %u)", action->text,
                          app->ui_store.nav.target_name, packet_id);
        } else if (result == -ENOTCONN) {
            snprintf(toast, sizeof toast, "%s", "Not connected to a node");
        } else {
            snprintf(toast, sizeof toast, "Send failed (%d)", result);
            mesh_log_warn("ui", "Send to %s failed: %d", app->ui_store.nav.target_name, result);
        }
        mesh_ui_store_set_toast(&app->ui_store, now, toast);
        return;
    }
    case MESH_UI_ACTION_NONE:
    default:
        return;
    }
}

static bool mesh_app_select_minui(struct mesh_app *app, const struct mesh_ui_backend **backend,
                                  void **userdata, bool log_on_missing) {
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

static void mesh_app_select_cli(struct mesh_app *app, const struct mesh_ui_backend **backend,
                                void **userdata) {
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

static bool mesh_app_select_fb(struct mesh_app *app, const struct mesh_ui_backend **backend,
                               void **userdata) {
    if (!mesh_ui_backend_fb_is_available()) {
        return false;
    }

    if (backend != NULL) {
        *backend = mesh_ui_backend_fb();
    }
    if (userdata != NULL) {
        app->ui_fb_context.loop = &app->loop;
        *userdata = &app->ui_fb_context;
    }
    return true;
}

static const struct mesh_ui_backend *mesh_app_select_backend(struct mesh_app *app,
                                                             void **userdata) {
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
            if (!mesh_app_select_minui(app, &backend, &backend_userdata, true) &&
                !mesh_app_select_fb(app, &backend, &backend_userdata)) {
                mesh_app_select_cli(app, &backend, &backend_userdata);
            }
        } else if (strcasecmp(requested, "fb") == 0) {
            if (!mesh_app_select_fb(app, &backend, &backend_userdata)) {
                mesh_app_select_cli(app, &backend, &backend_userdata);
            }
        } else if (strcasecmp(requested, "cli") == 0) {
            mesh_app_select_cli(app, &backend, &backend_userdata);
        } else if (strcasecmp(requested, "stub") == 0) {
            mesh_app_select_stub(&backend, &backend_userdata);
        } else if (strcasecmp(requested, "auto") == 0) {
            if (!mesh_app_select_minui(app, &backend, &backend_userdata, false) &&
                !mesh_app_select_fb(app, &backend, &backend_userdata)) {
                mesh_app_select_cli(app, &backend, &backend_userdata);
            }
        } else {
            mesh_log_warn("ui", "Unknown UI backend '%s'; defaulting to CLI", requested);
            mesh_app_select_cli(app, &backend, &backend_userdata);
        }
    } else {
        const char *platform = getenv("PLATFORM");
        bool prefer_minui = (platform != NULL && strcasecmp(platform, "tg5040") == 0);

        if (prefer_minui) {
            if (!mesh_app_select_minui(app, &backend, &backend_userdata, false) &&
                !mesh_app_select_fb(app, &backend, &backend_userdata)) {
                mesh_app_select_cli(app, &backend, &backend_userdata);
            }
        } else {
            if (!mesh_app_select_fb(app, &backend, &backend_userdata)) {
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

/* Resolves a node number to something a human can read, preferring the short name the NodeDB
   gave us and falling back to the Meshtastic-style "!hex" id. */
static void mesh_app_format_peer_name(const struct mesh_ble_handshake_status *status,
                                      uint32_t node_id, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }

    if (node_id == MESH_MESSAGE_BROADCAST_ADDR) {
        snprintf(out, out_len, "all");
        return;
    }

    if (status != NULL) {
        for (size_t i = 0; i < status->node_count && i < MESH_BLE_MAX_NODE_SUMMARY; ++i) {
            if (status->nodes[i].node_id != node_id) {
                continue;
            }
            if (status->nodes[i].short_name[0] != '\0') {
                snprintf(out, out_len, "%s", status->nodes[i].short_name);
                return;
            }
            if (status->nodes[i].long_name[0] != '\0') {
                snprintf(out, out_len, "%s", status->nodes[i].long_name);
                return;
            }
            break;
        }
    }

    snprintf(out, out_len, "!%08x", node_id);
}

/* Copies the newest MESH_UI_MAX_MESSAGES entries out of the transport ring into the store,
   merged with whatever history was restored from the cache at startup. */
static void mesh_app_publish_messages(struct mesh_app *app, struct mesh_transport *ble,
                                      const struct mesh_ble_handshake_status *status) {
    const struct mesh_message_log *log = mesh_ble_transport_messages(ble);
    if (log == NULL) {
        return;
    }

    struct mesh_ui_message_list live;
    memset(&live, 0, sizeof(live));
    live.dropped = log->dropped;

    /* The ring holds more than the UI carries; take the newest tail of it. */
    size_t first = (log->count > MESH_UI_MAX_MESSAGES) ? log->count - MESH_UI_MAX_MESSAGES : 0U;
    for (size_t i = first; i < log->count; ++i) {
        const struct mesh_message *source = mesh_message_log_at(log, i);
        if (source == NULL) {
            continue;
        }

        struct mesh_ui_message *target = &live.entries[live.count];
        const bool outbound = (source->direction == MESH_MESSAGE_OUTBOUND);
        target->packet_id = source->packet_id;
        target->peer = outbound ? source->to : source->from;
        target->rx_time = source->rx_time;
        target->channel = source->channel;
        target->direction = source->direction;
        target->ack = source->ack;
        target->broadcast = (source->to == MESH_MESSAGE_BROADCAST_ADDR);
        mesh_app_format_peer_name(status, target->peer, target->peer_name,
                                  sizeof(target->peer_name));
        snprintf(target->text, sizeof(target->text), "%s", source->text);
        live.count++;
    }

    struct mesh_ui_message_list list;
    mesh_ui_message_list_merge(&app->ui_messages_cached, &live, &list);

    mesh_ui_update_flags prev_flags = app->ui_store.pending_flags;
    mesh_ui_store_set_messages(&app->ui_store, &list);
    if (app->ui_handshake_cache_path[0] != '\0' &&
        (app->ui_store.pending_flags & MESH_UI_UPDATE_MESSAGES) != 0U &&
        (prev_flags & MESH_UI_UPDATE_MESSAGES) == 0U) {
        app->ui_handshake_cache_dirty = true;
    }
}

static void mesh_app_publish_ui_state(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }

    mesh_ui_store_tick(&app->ui_store, mesh_app_now_ms());

    struct mesh_transport *ble = mesh_ble_transport();
    if (ble == NULL) {
        mesh_ui_store_set_transport_status(&app->ui_store, "unavailable");
        return;
    }

    const char *transport_status =
        (ble->ops != NULL && ble->ops->status != NULL) ? ble->ops->status(ble) : NULL;
    mesh_ui_store_set_transport_status(&app->ui_store,
                                       transport_status != NULL ? transport_status : "unknown");

    struct mesh_bluez_device_info ble_devices[MESH_UI_MAX_DEVICES];
    size_t device_count = mesh_ble_transport_get_devices(ble, ble_devices, MESH_UI_MAX_DEVICES);

    struct mesh_ui_device ui_devices[MESH_UI_MAX_DEVICES];
    memset(ui_devices, 0, sizeof(ui_devices));

    const char *connected_address = mesh_ble_transport_connected_address(ble);
    bool connected_address_seen = false;

    for (size_t i = 0; i < device_count && i < MESH_UI_MAX_DEVICES; ++i) {
        snprintf(ui_devices[i].identifier, sizeof(ui_devices[i].identifier), "%s",
                 ble_devices[i].address);
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
        snprintf(ui_devices[device_count].identifier, sizeof(ui_devices[device_count].identifier),
                 "%s", connected_address);
        snprintf(ui_devices[device_count].name, sizeof(ui_devices[device_count].name), "%s",
                 "Connected");
        ui_devices[device_count].rssi = 0;
        ui_devices[device_count].connected = true;
        ++device_count;
    }

    mesh_ui_store_set_discovery(&app->ui_store, ui_devices, device_count);

    bool preferences_modified = false;
    if (connected_address != NULL && connected_address[0] != '\0') {
        if (strcmp(app->ui_preferences.preferred_device, connected_address) != 0) {
            snprintf(app->ui_preferences.preferred_device,
                     sizeof app->ui_preferences.preferred_device, "%s", connected_address);
            preferences_modified = true;
        }
    }

    struct mesh_ble_handshake_status status = mesh_ble_transport_handshake_status(ble);
    const bool handshake_active = status.request_in_flight || status.config_complete ||
                                  status.has_my_info || status.has_config ||
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

        size_t channel_count = status.channel_count;
        if (channel_count > MESH_UI_MAX_CHANNELS) {
            channel_count = MESH_UI_MAX_CHANNELS;
        }
        for (size_t i = 0; i < channel_count; ++i) {
            ui_handshake.channels[i].index = status.channels[i].index;
            ui_handshake.channels[i].role = status.channels[i].role;
            snprintf(ui_handshake.channels[i].name, sizeof(ui_handshake.channels[i].name), "%s",
                     status.channels[i].name);
            if (status.channels[i].role == 1U /* PRIMARY */) {
                snprintf(ui_handshake.primary_channel, sizeof(ui_handshake.primary_channel), "%s",
                         status.channels[i].name);
            }
        }
        ui_handshake.channel_count = (uint32_t)channel_count;

        mesh_ui_update_flags prev_flags = app->ui_store.pending_flags;
        mesh_ui_store_set_handshake(&app->ui_store, &ui_handshake);
        if (app->ui_handshake_cache_path[0] != '\0' &&
            (app->ui_store.pending_flags & MESH_UI_UPDATE_HANDSHAKE) != 0U &&
            (prev_flags & MESH_UI_UPDATE_HANDSHAKE) == 0U) {
            app->ui_handshake_cache_dirty = true;
        }

        if (ui_handshake.primary_channel[0] != '\0' &&
            strcmp(app->ui_preferences.preferred_channel, ui_handshake.primary_channel) != 0) {
            snprintf(app->ui_preferences.preferred_channel,
                     sizeof app->ui_preferences.preferred_channel, "%s",
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

    mesh_app_publish_messages(app, ble, &status);

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

#define MESH_APP_AUTOCONNECT_RETRY_MS 2000U
#define MESH_APP_AUTOCONNECT_MAX_BACKOFF_MS 60000U
/* How long a saved preferred node gets to show up in discovery before another node is used. */
#define MESH_APP_AUTOCONNECT_PREFERRED_GRACE_MS 30000U

static uint64_t mesh_app_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/* "0", "off", "false" and "no" disable; anything else (including unset) leaves the default. */
static bool mesh_app_env_disabled(const char *name) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    return strcasecmp(value, "0") == 0 || strcasecmp(value, "off") == 0 ||
           strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0;
}

void mesh_app_autoconnect(struct mesh_app *app) {
    if (app == NULL || app->autoconnect_disabled ||
        app->config.run_mode != MESH_APP_RUN_FOREGROUND) {
        return;
    }

    struct mesh_transport *ble = mesh_ble_transport();
    if (ble == NULL || mesh_ble_transport_connected_address(ble) != NULL ||
        mesh_ble_transport_is_connecting(ble)) {
        return;
    }

    uint64_t now = mesh_app_now_ms();
    if (app->autoconnect_started_ms == 0U) {
        app->autoconnect_started_ms = now;
    }
    if (now < app->autoconnect_retry_at_ms) {
        return;
    }

    struct mesh_bluez_device_info devices[MESH_UI_MAX_DEVICES];
    size_t device_count = mesh_ble_transport_get_devices(ble, devices, MESH_UI_MAX_DEVICES);
    if (device_count == 0U) {
        return; /* nothing in range yet; discovery keeps running */
    }

    const struct mesh_bluez_device_info *target = NULL;
    const char *preferred = app->config.preferred_ble_device;
    if (preferred[0] != '\0') {
        for (size_t i = 0; i < device_count; ++i) {
            if (strcasecmp(devices[i].address, preferred) == 0 ||
                strcasecmp(devices[i].name, preferred) == 0) {
                target = &devices[i];
                break;
            }
        }
        if (target == NULL &&
            now - app->autoconnect_started_ms < MESH_APP_AUTOCONNECT_PREFERRED_GRACE_MS) {
            if (!app->autoconnect_waiting_logged) {
                mesh_log_info("app", "Preferred device '%s' not in range yet; waiting", preferred);
                app->autoconnect_waiting_logged = true;
            }
            app->autoconnect_retry_at_ms = now + 1000U;
            return;
        }
    }

    if (target == NULL) {
        size_t best = 0U;
        for (size_t i = 1; i < device_count; ++i) {
            if (devices[i].rssi > devices[best].rssi) {
                best = i;
            }
        }
        target = &devices[best];
        mesh_log_info("app", "%s; using strongest node %s (%s, %d dBm)",
                      preferred[0] != '\0' ? "Preferred device not in range"
                                           : "No preferred device saved",
                      target->name, target->address, (int)target->rssi);
    }

    int result = mesh_ble_transport_connect(ble, target->address);
    if (result == 0 || result == -EALREADY || result == -EINPROGRESS) {
        if (result == 0) {
            mesh_log_info("app", "Auto-connecting to %s (%s)", target->name, target->address);
        }
        app->autoconnect_failures = 0U;
        app->autoconnect_retry_at_ms = now + MESH_APP_AUTOCONNECT_RETRY_MS;
        return;
    }
    if (result == -EAGAIN) {
        app->autoconnect_retry_at_ms = now + 1000U; /* transport not READY yet */
        return;
    }

    if (app->autoconnect_failures < 8U) {
        app->autoconnect_failures++;
    }
    uint64_t delay = (uint64_t)MESH_APP_AUTOCONNECT_RETRY_MS << (app->autoconnect_failures - 1U);
    if (delay > MESH_APP_AUTOCONNECT_MAX_BACKOFF_MS) {
        delay = MESH_APP_AUTOCONNECT_MAX_BACKOFF_MS;
    }
    app->autoconnect_retry_at_ms = now + delay;
    mesh_log_warn("app", "Auto-connect to %s failed (%d); retrying in %llu ms", target->address,
                  result, (unsigned long long)delay);
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
    memset(&app->ui_fb_context, 0, sizeof app->ui_fb_context);
    memset(&app->ui_input, 0, sizeof app->ui_input);
    memset(&app->signals, 0, sizeof app->signals);
    app->signals.fd = -1;
    memset(&app->ui_preferences, 0, sizeof(app->ui_preferences));
    app->ui_preferences_path[0] = '\0';
    app->ui_preferences_dirty = false;
    app->ui_handshake_cache_path[0] = '\0';
    app->autoconnect_started_ms = 0U;
    app->autoconnect_retry_at_ms = 0U;
    app->autoconnect_failures = 0U;
    app->autoconnect_waiting_logged = false;
    app->autoconnect_disabled = mesh_app_env_disabled("MESHCLIENT_AUTOCONNECT");
    if (app->autoconnect_disabled) {
        mesh_log_info("app", "Auto-connect disabled by MESHCLIENT_AUTOCONNECT");
    }
    app->ui_handshake_cache_dirty = false;

    if (mesh_ui_preferences_default_path(app->ui_preferences_path,
                                         sizeof(app->ui_preferences_path)) == 0) {
        int load_result = mesh_ui_preferences_load(&app->ui_preferences, app->ui_preferences_path);
        if (load_result == 0) {
            if (app->config.preferred_ble_device[0] == '\0' &&
                app->ui_preferences.preferred_device[0] != '\0') {
                snprintf(app->config.preferred_ble_device, sizeof app->config.preferred_ble_device,
                         "%s", app->ui_preferences.preferred_device);
            }
        }
        int handshake_written =
            snprintf(app->ui_handshake_cache_path, sizeof(app->ui_handshake_cache_path),
                     "%s.handshake", app->ui_preferences_path);
        if (handshake_written < 0 ||
            handshake_written >= (int)sizeof(app->ui_handshake_cache_path)) {
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

    /* Keep the restored conversation aside: every publish merges it back in, so an empty
       transport log at startup never overwrites it. */
    app->ui_messages_cached = app->ui_store.messages;

    void *backend_userdata = NULL;
    const struct mesh_ui_backend *ui_backend = mesh_app_select_backend(app, &backend_userdata);
    result = mesh_ui_controller_init(&app->ui_controller, &app->ui_store, ui_backend,
                                     backend_userdata, &app->loop);
    if (result < 0) {
        mesh_log_warn("app", "UI backend init failed (%d); falling back to stub", result);
        result = mesh_ui_controller_init(&app->ui_controller, &app->ui_store,
                                         mesh_ui_backend_stub(), NULL, &app->loop);
    }
    if (result < 0) {
        mesh_log_error("app", "UI controller init failed: %d", result);
        mesh_ui_store_shutdown(&app->ui_store);
        mesh_event_loop_shutdown(&app->loop);
        return result;
    }
    mesh_ui_controller_set_action_handler(&app->ui_controller, mesh_app_on_ui_action, app);

    /* Optional canned.txt next to the preferences file replaces the built-in quick replies. */
    if (app->ui_preferences_path[0] != '\0') {
        char canned_path[sizeof app->ui_preferences_path + 16U];
        snprintf(canned_path, sizeof canned_path, "%s", app->ui_preferences_path);
        char *slash = strrchr(canned_path, '/');
        if (slash != NULL) {
            const size_t room = sizeof canned_path - (size_t)(slash + 1 - canned_path);
            snprintf(slash + 1, room, "%s", "canned.txt");
            const int loaded = mesh_ui_canned_load(canned_path);
            if (loaded > 0) {
                mesh_log_info("app", "Loaded %d canned replies from %s", loaded, canned_path);
            } else if (loaded != -ENOENT) {
                mesh_log_warn("app", "Ignoring %s: %d", canned_path, loaded);
            }
        }
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
    mesh_ui_input_shutdown(&app->ui_input);
    mesh_signals_shutdown(&app->signals);
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

    int result =
        mesh_transport_registry_start_all(&app->transport_registry, &app->config, &app->loop);
    if (result < 0) {
        return result;
    }

    /* Only the interactive run takes these over: --status and --list-devices stay plain CLI
       tools that Ctrl-C kills outright. Neither is fatal if it fails - a client that cannot
       read buttons is still better than no client. */
    mesh_signals_init(&app->signals, &app->loop);
    mesh_ui_input_init(&app->ui_input, &app->loop);
    mesh_ui_input_set_handler(&app->ui_input, mesh_app_on_ui_key, app);

    mesh_app_publish_ui_state(app);

    switch (app->config.run_mode) {
    case MESH_APP_RUN_SINGLE_POLL:
        mesh_log_debug("app", "Running single poll with timeout %d ms",
                       app->config.idle_timeout_ms);
        mesh_app_publish_ui_state(app);
        result = mesh_event_loop_run(&app->loop, app->config.idle_timeout_ms);
        if (result >= 0) {
            mesh_app_publish_ui_state(app);
        }
        break;
    case MESH_APP_RUN_FOREGROUND:
        mesh_log_info("app", "Starting foreground event loop (timeout %d ms)",
                      app->config.idle_timeout_ms);
        /* Paint the first frame before any transport work: the store already has a refresh
           queued, and a zero timeout drains what is ready without waiting for more. */
        mesh_event_loop_run(&app->loop, 0);
        while (true) {
            mesh_transport_registry_tick(&app->transport_registry);
            mesh_app_autoconnect(app);
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

    mesh_ui_input_shutdown(&app->ui_input);
    mesh_signals_shutdown(&app->signals);
    return result;
}
