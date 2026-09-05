#define _POSIX_C_SOURCE 200809L

/*
 * Transport and session state -> the UI store.
 *
 * One direction only, and copied rather than referenced: the UI renders from plain structs it
 * owns, so nothing in src/ui/ ever sees a protobuf or a live transport. mesh_app_run() calls
 * mesh_app_publish_ui_state() every loop turn, which makes this the app's hot path - hence the
 * dirty flags and the ranking cut rather than a full rebuild each time.
 */

#include "app_internal.h"

#include "mesh/core/version.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/serial.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/preferences.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Resolves a node number to something a human can read, preferring the short name the NodeDB
   gave us and falling back to the Meshtastic-style "!hex" id. */
void mesh_app_format_peer_name(const struct mesh_handshake_status *status, uint32_t node_id,
                               char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }

    if (node_id == MESH_MESSAGE_BROADCAST_ADDR) {
        snprintf(out, out_len, "all");
        return;
    }

    if (status != NULL) {
        for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
            if (status->nodes[i].node_id != node_id) {
                continue;
            }
            /* The names are already sanitised; the copy still has to respect character
               boundaries because peer_name is far shorter than long_name. */
            if (status->nodes[i].short_name[0] != '\0') {
                mesh_text_sanitise_str(status->nodes[i].short_name, out, out_len);
                return;
            }
            if (status->nodes[i].long_name[0] != '\0') {
                mesh_text_sanitise_str(status->nodes[i].long_name, out, out_len);
                return;
            }
            break;
        }
    }

    snprintf(out, out_len, "!%08x", node_id);
}

/* Position, device metrics and environment, from the session's structs into the UI's twins.
   Field by field rather than a memcpy: the two declarations are deliberately independent (the
   UI half must stay free of nanopb), so nothing but this function keeps them in step. */
static void mesh_app_copy_node_detail(const struct mesh_node_summary *src,
                                      struct mesh_ui_node_summary *dst) {
    dst->position.valid = src->position.valid;
    dst->position.latitude_i = src->position.latitude_i;
    dst->position.longitude_i = src->position.longitude_i;
    dst->position.has_altitude = src->position.has_altitude;
    dst->position.altitude = src->position.altitude;
    dst->position.time = src->position.time;
    dst->position.sats_in_view = src->position.sats_in_view;
    dst->position.precision_bits = src->position.precision_bits;

    dst->metrics.valid = src->metrics.valid;
    dst->metrics.time = src->metrics.time;
    dst->metrics.has_battery = src->metrics.has_battery;
    dst->metrics.battery_level = src->metrics.battery_level;
    dst->metrics.has_voltage = src->metrics.has_voltage;
    dst->metrics.voltage = src->metrics.voltage;
    dst->metrics.has_channel_utilization = src->metrics.has_channel_utilization;
    dst->metrics.channel_utilization = src->metrics.channel_utilization;
    dst->metrics.has_air_util_tx = src->metrics.has_air_util_tx;
    dst->metrics.air_util_tx = src->metrics.air_util_tx;
    dst->metrics.has_uptime = src->metrics.has_uptime;
    dst->metrics.uptime_seconds = src->metrics.uptime_seconds;

    dst->environment.valid = src->environment.valid;
    dst->environment.time = src->environment.time;
    dst->environment.has_temperature = src->environment.has_temperature;
    dst->environment.temperature = src->environment.temperature;
    dst->environment.has_humidity = src->environment.has_humidity;
    dst->environment.relative_humidity = src->environment.relative_humidity;
    dst->environment.has_pressure = src->environment.has_pressure;
    dst->environment.barometric_pressure = src->environment.barometric_pressure;
    dst->environment.has_iaq = src->environment.has_iaq;
    dst->environment.iaq = src->environment.iaq;
    dst->environment.has_lux = src->environment.has_lux;
    dst->environment.lux = src->environment.lux;
    dst->environment.has_voltage = src->environment.has_voltage;
    dst->environment.voltage = src->environment.voltage;
    dst->environment.has_current = src->environment.has_current;
    dst->environment.current = src->environment.current;
}

/*
 * The same copy the other way, for the roster the last run left on disk. Only the fields the
 * cache actually persists are restored - position, metrics and environment come back through
 * their own keys - so a node returns as what we knew, not as a blank with a name.
 */
static void mesh_app_restore_node(const struct mesh_ui_node_summary *src,
                                  struct mesh_node_summary *dst) {
    memset(dst, 0, sizeof *dst);
    dst->node_id = src->node_id;
    (void)mesh_str_copy(dst->long_name, sizeof dst->long_name, src->long_name);
    (void)mesh_str_copy(dst->short_name, sizeof dst->short_name, src->short_name);
    (void)mesh_str_copy(dst->user_id, sizeof dst->user_id, src->user_id);
    dst->has_user = src->has_user;
    dst->in_nodedb = src->in_nodedb;
    dst->last_heard = src->last_heard;
    dst->snr = src->snr;
    dst->via_mqtt = src->via_mqtt;
    dst->has_hops_away = src->has_hops_away;
    dst->hops_away = src->hops_away;
    dst->hw_model = src->hw_model;
    dst->role = src->role;
    dst->is_licensed = src->is_licensed;
    dst->is_unmessagable = src->is_unmessagable;
    dst->public_key_len = src->public_key_len > sizeof dst->public_key
                              ? (uint8_t)sizeof dst->public_key
                              : src->public_key_len;
    memcpy(dst->public_key, src->public_key, dst->public_key_len);
    dst->is_favorite = src->is_favorite;
    dst->is_ignored = src->is_ignored;
    dst->is_muted = src->is_muted;
    dst->channel = src->channel;

    dst->position.valid = src->position.valid;
    dst->position.latitude_i = src->position.latitude_i;
    dst->position.longitude_i = src->position.longitude_i;
    dst->position.has_altitude = src->position.has_altitude;
    dst->position.altitude = src->position.altitude;
    dst->position.time = src->position.time;
    dst->position.sats_in_view = src->position.sats_in_view;
    dst->position.precision_bits = src->position.precision_bits;

    dst->metrics.valid = src->metrics.valid;
    dst->metrics.time = src->metrics.time;
    dst->metrics.has_battery = src->metrics.has_battery;
    dst->metrics.battery_level = src->metrics.battery_level;
    dst->metrics.has_voltage = src->metrics.has_voltage;
    dst->metrics.voltage = src->metrics.voltage;
    dst->metrics.has_channel_utilization = src->metrics.has_channel_utilization;
    dst->metrics.channel_utilization = src->metrics.channel_utilization;
    dst->metrics.has_air_util_tx = src->metrics.has_air_util_tx;
    dst->metrics.air_util_tx = src->metrics.air_util_tx;
    dst->metrics.has_uptime = src->metrics.has_uptime;
    dst->metrics.uptime_seconds = src->metrics.uptime_seconds;

    dst->environment.valid = src->environment.valid;
    dst->environment.time = src->environment.time;
    dst->environment.has_temperature = src->environment.has_temperature;
    dst->environment.temperature = src->environment.temperature;
    dst->environment.has_humidity = src->environment.has_humidity;
    dst->environment.relative_humidity = src->environment.relative_humidity;
    dst->environment.has_pressure = src->environment.has_pressure;
    dst->environment.barometric_pressure = src->environment.barometric_pressure;
    dst->environment.has_iaq = src->environment.has_iaq;
    dst->environment.iaq = src->environment.iaq;
    dst->environment.has_lux = src->environment.has_lux;
    dst->environment.lux = src->environment.lux;
    dst->environment.has_voltage = src->environment.has_voltage;
    dst->environment.voltage = src->environment.voltage;
    dst->environment.has_current = src->environment.has_current;
    dst->environment.current = src->environment.current;
}

/*
 * Hands the roster the last run persisted to the session, before any radio is attached. Without
 * it the roster would outlive a reconnect but not a restart, and the client would still forget
 * a node the moment the radio's 80-entry NodeDB did. The radio's own replay lands on top of
 * this a few seconds later and corrects whatever has changed since.
 */
void mesh_app_seed_nodes_from_cache(struct mesh_app *app) {
    if (app == NULL || !app->ui_store.handshake_valid) {
        return;
    }
    const struct mesh_ui_handshake_state *cached = &app->ui_store.handshake;
    /* Whose roster this is, before the nodes themselves: a radio other than this one must clear
       it on its first MyNodeInfo rather than merge its mesh into ours. */
    if (cached->roster_owner != 0U) {
        mesh_session_set_roster_owner(&app->session, cached->roster_owner);
    } else if (cached->has_my_info) {
        mesh_session_set_roster_owner(&app->session, cached->my_info.node_num);
    }
    uint32_t seeded = 0U;
    for (uint32_t i = 0; i < cached->node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
        if (cached->nodes[i].node_id == 0U) {
            continue;
        }
        struct mesh_node_summary node;
        mesh_app_restore_node(&cached->nodes[i], &node);
        mesh_session_seed_node(&app->session, &node);
        ++seeded;
    }
    if (seeded > 0U) {
        mesh_log_info("app", "Restored %u node%s from the cached roster", seeded,
                      seeded == 1U ? "" : "s");
    }
}

/* Lower is more important; see the ranking comment in mesh_app_publish_ui_state(). */
unsigned mesh_app_node_rank(const struct mesh_node_summary *node, uint32_t my_node,
                            const struct mesh_message_log *log,
                            const struct mesh_ui_preferences *prefs) {
    if (my_node != 0U && node->node_id == my_node) {
        return 0U;
    }
    /* A pinned node outranks even someone you are mid-conversation with: pinning is the user
       saying "keep this one where I can see it", and it is also what keeps a quiet node inside
       the UI's 128-node budget when the mesh is busy. */
    if (node->is_favorite) {
        return 1U;
    }
    /*
     * A radio of our own that we are not connected to right now. is_favorite lives in the
     * connected radio's NodeDB and is resolved per receiver, so a pin only ever teaches the
     * radio it was made on: move the Brick from one of your nodes to another and the node you
     * just unplugged arrives on the new radio as an ordinary stranger, ranked by last_heard,
     * free to fall out of the 128-node budget on a busy mesh. The client remembers its own
     * hardware instead (mesh_ui_preferences_note_radio), which needs no admin write and cannot
     * disagree with what "favorite" means on the radio.
     */
    if (mesh_ui_preferences_knows_radio(prefs, node->node_id)) {
        return 2U;
    }
    if (log != NULL) {
        for (size_t i = 0; i < log->count; ++i) {
            const struct mesh_message *message = mesh_message_log_at(log, i);
            if (message == NULL) {
                continue;
            }
            if (message->from == node->node_id || message->to == node->node_id) {
                return 3U;
            }
        }
    }
    return node->via_mqtt ? 5U : 4U;
}

/* Copies the newest MESH_UI_MAX_MESSAGES entries out of the transport ring into the store,
   merged with whatever history was restored from the cache at startup. */
static void mesh_app_publish_messages(struct mesh_app *app,
                                      const struct mesh_handshake_status *status) {
    const struct mesh_message_log *log = mesh_session_messages(&app->session);
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
        target->ack_error = source->ack_error;
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

/*
 * A traceroute, from the protobuf's shape into the one the UI draws. RouteDiscovery gives the
 * *intermediate* nodes and a parallel array of link SNRs; what a reader wants is the whole
 * path with a reading against each stop it reached. So this stitches the ends on - us at the
 * front going out, the target at the front coming back - resolves every hop to a name, and
 * pairs hop i with snr[i - 1], the link that got the packet there.
 *
 * The SNR array is normally one longer than the route (one reading per link, not per node),
 * but a firmware that disagrees must not make us read off the end, so each pairing is bounds
 * checked rather than assumed.
 */
static uint8_t mesh_app_flatten_route(const struct mesh_handshake_status *status,
                                      uint32_t first_node, const uint32_t *route,
                                      uint8_t route_count, const int8_t *snr, uint8_t snr_count,
                                      uint32_t last_node, struct mesh_ui_traceroute_hop *out) {
    uint8_t count = 0U;
    /* The path is first_node, then every node that forwarded it, then the far end. */
    uint32_t path[MESH_UI_TRACEROUTE_MAX_HOPS];
    path[count++] = first_node;
    for (uint8_t i = 0; i < route_count && count < MESH_UI_TRACEROUTE_MAX_HOPS - 1U; ++i) {
        path[count++] = route[i];
    }
    path[count++] = last_node;

    for (uint8_t i = 0; i < count; ++i) {
        struct mesh_ui_traceroute_hop *hop = &out[i];
        memset(hop, 0, sizeof *hop);
        hop->node_id = path[i];
        mesh_app_format_peer_name(status, path[i], hop->name, sizeof hop->name);
        /* The first stop is the sender: nothing carried the packet *to* it. */
        if (i > 0U && (uint8_t)(i - 1U) < snr_count) {
            hop->has_snr = true;
            hop->snr_quarter_db = snr[i - 1U];
        }
    }
    return count;
}

void mesh_app_flatten_traceroute(const struct mesh_handshake_status *status,
                                 const struct mesh_traceroute *src, uint32_t my_node,
                                 struct mesh_ui_traceroute *dst) {
    memset(dst, 0, sizeof *dst);
    if (src == NULL || src->state == MESH_TRACEROUTE_IDLE) {
        return;
    }
    dst->state = src->state;
    dst->target = src->target;
    dst->completed = src->completed;
    if (src->state != MESH_TRACEROUTE_DONE) {
        return;
    }
    dst->forward_count =
        mesh_app_flatten_route(status, my_node, src->route, src->route_count, src->snr,
                               src->snr_count, src->target, dst->forward);
    /* The way back is only drawn when the firmware measured it; an empty route_back with no
       readings would otherwise render as a bare two-stop path that says nothing. */
    if (src->snr_back_count > 0U || src->back_count > 0U) {
        dst->back_count =
            mesh_app_flatten_route(status, src->target, src->route_back, src->back_count,
                                   src->snr_back, src->snr_back_count, my_node, dst->back);
    }
}

/* Mesh health, copied field by field for the same reason everything else here is: nothing
   keeps the session's declaration and the UI's in step but this function. */
static void mesh_app_flatten_radio_stats(const struct mesh_radio_stats *src,
                                         struct mesh_ui_radio_stats *dst) {
    memset(dst, 0, sizeof *dst);
    if (src == NULL || !src->valid) {
        return;
    }
    dst->valid = true;
    dst->time = src->time;
    dst->uptime_seconds = src->uptime_seconds;
    dst->channel_utilization = src->channel_utilization;
    dst->air_util_tx = src->air_util_tx;
    dst->num_packets_tx = src->num_packets_tx;
    dst->num_packets_rx = src->num_packets_rx;
    dst->num_packets_rx_bad = src->num_packets_rx_bad;
    dst->num_rx_dupe = src->num_rx_dupe;
    dst->num_tx_relay = src->num_tx_relay;
    dst->num_tx_relay_canceled = src->num_tx_relay_canceled;
    dst->num_tx_dropped = src->num_tx_dropped;
    dst->num_online_nodes = src->num_online_nodes;
    dst->num_total_nodes = src->num_total_nodes;
    dst->has_heap = src->has_heap;
    dst->heap_total_bytes = src->heap_total_bytes;
    dst->heap_free_bytes = src->heap_free_bytes;
    dst->has_noise_floor = src->has_noise_floor;
    dst->noise_floor = src->noise_floor;
}

/* Flattens the transport's protobuf-typed view into the UI's plain struct. */
/* The About section's data: this client rather than the radio. The updater's state is copied
   across as a byte and a line of text so store.h stays free of the updater, the same way the
   radio's settings are copied free of nanopb. */
static void mesh_app_flatten_client_info(const struct mesh_app *app,
                                         struct mesh_ui_client_info *dst) {
    memset(dst, 0, sizeof *dst);
    snprintf(dst->version, sizeof dst->version, "%s", mesh_version_string());
    if (app == NULL) {
        return;
    }
    if (app->ui_controller.backend != NULL && app->ui_controller.backend->name != NULL) {
        snprintf(dst->backend, sizeof dst->backend, "%s", app->ui_controller.backend->name);
    }
    /* The preferences file's directory: where a user looking for canned.txt or the caches
       should go, which on the Brick is inside the pak's userdata and not obvious. */
    if (app->ui_preferences_path[0] != '\0') {
        /* A path longer than the display field is clipped rather than refused: it is shown
           for orientation, not used to open anything. */
        mesh_str_copy(dst->data_dir, sizeof dst->data_dir, app->ui_preferences_path);
        char *slash = strrchr(dst->data_dir, '/');
        if (slash != NULL && slash != dst->data_dir) {
            *slash = '\0';
        }
    }

    const struct mesh_updater *updater = &app->updater;
    dst->update_state = (uint8_t)updater->state;
    dst->update_supported = mesh_updater_available(updater);
    dst->update_busy =
        updater->state == MESH_UPDATE_CHECKING || updater->state == MESH_UPDATE_DOWNLOADING;
    dst->update_can_install = mesh_updater_can_install(updater);
    dst->update_is_release = mesh_version_is_release();
    dst->update_allow_dev = updater->allow_dev;
    dst->update_allow_dev_from_env = updater->allow_dev_from_env;
    snprintf(dst->update_message, sizeof dst->update_message, "%s", updater->message);
    snprintf(dst->update_latest, sizeof dst->update_latest, "%s", updater->latest);
    snprintf(dst->update_channel, sizeof dst->update_channel, "%s",
             mesh_update_channel_name(updater->channel));
}

static void mesh_app_flatten_settings(const struct mesh_radio_settings *src,
                                      struct mesh_ui_settings *dst) {
    memset(dst, 0, sizeof *dst);
    if (src == NULL) {
        return;
    }
    dst->loaded = mesh_radio_settings_loaded(src);
    dst->admin_ok = src->admin_replies > 0U;
    dst->admin_busy = mesh_radio_settings_busy(src) || src->queue_len > 0U;
    dst->write_pending = mesh_radio_settings_write_pending(src);
    dst->admin_replies = src->admin_replies;

    if (src->has_owner) {
        dst->has_owner = true;
        snprintf(dst->long_name, sizeof dst->long_name, "%s", src->owner.long_name);
        snprintf(dst->short_name, sizeof dst->short_name, "%s", src->owner.short_name);
        dst->is_licensed = src->owner.is_licensed;
        dst->is_unmessagable = src->owner.has_is_unmessagable && src->owner.is_unmessagable;
    }
    if (src->has_device) {
        dst->has_device = true;
        dst->role = (uint8_t)src->device.role;
        dst->rebroadcast_mode = (uint8_t)src->device.rebroadcast_mode;
        snprintf(dst->tzdef, sizeof dst->tzdef, "%s", src->device.tzdef);
        dst->led_heartbeat_disabled = src->device.led_heartbeat_disabled;
        dst->double_tap_as_button_press = src->device.double_tap_as_button_press;
        dst->node_info_broadcast_secs = src->device.node_info_broadcast_secs;
    }
    if (src->has_display) {
        dst->has_display = true;
        dst->screen_on_secs = src->display.screen_on_secs;
        dst->carousel_secs = src->display.auto_screen_carousel_secs;
        dst->compass_orientation = (uint8_t)src->display.compass_orientation;
        dst->use_12h_clock = src->display.use_12h_clock;
        dst->units = (uint8_t)src->display.units;
        dst->flip_screen = src->display.flip_screen;
    }
    if (src->has_lora) {
        dst->has_lora = true;
        dst->use_preset = src->lora.use_preset;
        dst->modem_preset = (uint8_t)src->lora.modem_preset;
        dst->region = (uint8_t)src->lora.region;
        dst->bandwidth = src->lora.bandwidth;
        dst->spread_factor = src->lora.spread_factor;
        dst->coding_rate = src->lora.coding_rate;
        dst->hop_limit = (uint8_t)src->lora.hop_limit;
        dst->tx_enabled = src->lora.tx_enabled;
        dst->tx_power = (int8_t)src->lora.tx_power;
        dst->ignore_mqtt = src->lora.ignore_mqtt;
        dst->config_ok_to_mqtt = src->lora.config_ok_to_mqtt;
    }
    if (src->has_bluetooth) {
        dst->has_bluetooth = true;
        dst->bluetooth_enabled = src->bluetooth.enabled;
        dst->pairing_mode = (uint8_t)src->bluetooth.mode;
        dst->fixed_pin = src->bluetooth.fixed_pin;
    }
    if (src->has_security) {
        dst->has_security = true;
        size_t key_len = src->security.public_key.size;
        if (key_len > sizeof dst->public_key) {
            key_len = sizeof dst->public_key;
        }
        memcpy(dst->public_key, src->security.public_key.bytes, key_len);
        dst->public_key_len = (uint8_t)key_len;
        dst->has_private_key = src->security.private_key.size > 0U;
        size_t private_len = src->security.private_key.size;
        if (private_len > sizeof dst->private_key) {
            private_len = sizeof dst->private_key;
        }
        memcpy(dst->private_key, src->security.private_key.bytes, private_len);
        dst->private_key_len = (uint8_t)private_len;
        dst->admin_key_count = (uint8_t)src->security.admin_key_count;
        for (unsigned i = 0; i < 3U && i < src->security.admin_key_count; ++i) {
            size_t len = src->security.admin_key[i].size;
            if (len > sizeof dst->admin_keys[i]) {
                len = sizeof dst->admin_keys[i];
            }
            memcpy(dst->admin_keys[i], src->security.admin_key[i].bytes, len);
            dst->admin_key_lens[i] = (uint8_t)len;
        }
        dst->is_managed = src->security.is_managed;
        dst->serial_enabled = src->security.serial_enabled;
        dst->debug_log_api_enabled = src->security.debug_log_api_enabled;
        dst->admin_channel_enabled = src->security.admin_channel_enabled;
        dst->packet_signature_policy = (uint8_t)src->security.packet_signature_policy;
    }
    if (src->has_position) {
        dst->has_position = true;
        dst->gps_mode = (uint8_t)src->position.gps_mode;
        dst->position_broadcast_secs = src->position.position_broadcast_secs;
        dst->position_broadcast_smart_enabled = src->position.position_broadcast_smart_enabled;
        dst->fixed_position = src->position.fixed_position;
        dst->gps_update_interval = src->position.gps_update_interval;
        dst->smart_minimum_distance = src->position.broadcast_smart_minimum_distance;
        dst->smart_minimum_interval_secs = src->position.broadcast_smart_minimum_interval_secs;
    }
    if (src->has_power) {
        dst->has_power = true;
        dst->is_power_saving = src->power.is_power_saving;
        dst->ls_secs = src->power.ls_secs;
        dst->min_wake_secs = src->power.min_wake_secs;
        dst->on_battery_shutdown_after_secs = src->power.on_battery_shutdown_after_secs;
        dst->wait_bluetooth_secs = src->power.wait_bluetooth_secs;
    }
    if (src->has_mqtt) {
        dst->has_mqtt = true;
        dst->mqtt_enabled = src->mqtt.enabled;
        snprintf(dst->mqtt_address, sizeof dst->mqtt_address, "%s", src->mqtt.address);
        snprintf(dst->mqtt_username, sizeof dst->mqtt_username, "%s", src->mqtt.username);
        snprintf(dst->mqtt_password, sizeof dst->mqtt_password, "%s", src->mqtt.password);
        snprintf(dst->mqtt_root, sizeof dst->mqtt_root, "%s", src->mqtt.root);
        dst->mqtt_encryption_enabled = src->mqtt.encryption_enabled;
        dst->mqtt_tls_enabled = src->mqtt.tls_enabled;
        dst->mqtt_map_reporting_enabled = src->mqtt.map_reporting_enabled;
        dst->mqtt_proxy_to_client_enabled = src->mqtt.proxy_to_client_enabled;
        dst->mqtt_map_publish_interval_secs = src->mqtt.map_report_settings.publish_interval_secs;
        dst->mqtt_map_position_precision = src->mqtt.map_report_settings.position_precision;
        dst->mqtt_map_should_report_location = src->mqtt.map_report_settings.should_report_location;
    }
    if (src->has_store_forward) {
        dst->has_store_forward = true;
        dst->store_forward_enabled = src->store_forward.enabled;
        dst->store_forward_heartbeat = src->store_forward.heartbeat;
        dst->store_forward_is_server = src->store_forward.is_server;
        dst->store_forward_records = src->store_forward.records;
        dst->store_forward_history_return_max = src->store_forward.history_return_max;
        dst->store_forward_history_return_window = src->store_forward.history_return_window;
    }
    if (src->has_telemetry) {
        dst->has_telemetry = true;
        dst->device_update_interval = src->telemetry.device_update_interval;
        dst->device_telemetry_enabled = src->telemetry.device_telemetry_enabled;
        dst->environment_measurement_enabled = src->telemetry.environment_measurement_enabled;
        dst->environment_update_interval = src->telemetry.environment_update_interval;
        dst->environment_screen_enabled = src->telemetry.environment_screen_enabled;
        dst->environment_display_fahrenheit = src->telemetry.environment_display_fahrenheit;
        dst->air_quality_enabled = src->telemetry.air_quality_enabled;
        dst->air_quality_interval = src->telemetry.air_quality_interval;
        dst->air_quality_screen_enabled = src->telemetry.air_quality_screen_enabled;
        dst->power_measurement_enabled = src->telemetry.power_measurement_enabled;
        dst->power_update_interval = src->telemetry.power_update_interval;
        dst->power_screen_enabled = src->telemetry.power_screen_enabled;
        dst->health_measurement_enabled = src->telemetry.health_measurement_enabled;
        dst->health_update_interval = src->telemetry.health_update_interval;
        dst->health_screen_enabled = src->telemetry.health_screen_enabled;
    }
    if (src->has_neighbor_info) {
        dst->has_neighbor_info = true;
        dst->neighbor_info_enabled = src->neighbor_info.enabled;
        dst->neighbor_info_interval = src->neighbor_info.update_interval;
        dst->neighbor_info_over_lora = src->neighbor_info.transmit_over_lora;
    }
    if (src->has_range_test) {
        dst->has_range_test = true;
        dst->range_test_enabled = src->range_test.enabled;
        dst->range_test_sender = src->range_test.sender;
        dst->range_test_save = src->range_test.save;
        dst->range_test_clear_on_reboot = src->range_test.clear_on_reboot;
    }
    if (src->has_paxcounter) {
        dst->has_paxcounter = true;
        dst->paxcounter_enabled = src->paxcounter.enabled;
        dst->paxcounter_interval = src->paxcounter.paxcounter_update_interval;
        dst->paxcounter_wifi_threshold = src->paxcounter.wifi_threshold;
        dst->paxcounter_ble_threshold = src->paxcounter.ble_threshold;
    }
    if (src->has_tak) {
        dst->has_tak = true;
        dst->tak_team = (uint8_t)src->tak.team;
        dst->tak_role = (uint8_t)src->tak.role;
    }
    if (src->has_ambient_lighting) {
        dst->has_ambient_lighting = true;
        dst->ambient_led_state = src->ambient_lighting.led_state;
        dst->ambient_current = src->ambient_lighting.current;
        dst->ambient_red = src->ambient_lighting.red;
        dst->ambient_green = src->ambient_lighting.green;
        dst->ambient_blue = src->ambient_lighting.blue;
    }
    if (src->has_status_message) {
        dst->has_status_message = true;
        mesh_str_copy(dst->status_message, sizeof dst->status_message,
                      src->status_message.node_status);
    }
    for (size_t i = 0; i < MESH_RADIO_SETTINGS_MAX_CHANNELS && i < MESH_UI_MAX_CHANNELS; ++i) {
        if (!src->has_channel[i]) {
            continue;
        }
        const meshtastic_Channel *channel = &src->channels[i];
        struct mesh_ui_channel_detail *detail = &dst->channels[i];
        dst->has_channels = true;
        detail->present = true;
        detail->index = (uint8_t)i;
        detail->role = (uint8_t)channel->role;
        if (channel->has_settings) {
            snprintf(detail->name, sizeof detail->name, "%s", channel->settings.name);
            size_t psk_len = channel->settings.psk.size;
            if (psk_len > sizeof detail->psk) {
                psk_len = sizeof detail->psk;
            }
            memcpy(detail->psk, channel->settings.psk.bytes, psk_len);
            detail->psk_len = (uint8_t)psk_len;
            detail->uplink_enabled = channel->settings.uplink_enabled;
            detail->downlink_enabled = channel->settings.downlink_enabled;
            detail->position_precision = channel->settings.has_module_settings
                                             ? channel->settings.module_settings.position_precision
                                             : 0U;
        }
    }
    if (src->has_metadata) {
        dst->has_metadata = true;
        snprintf(dst->firmware_version, sizeof dst->firmware_version, "%s",
                 src->metadata.firmware_version);
        dst->hw_model = (uint32_t)src->metadata.hw_model;
        dst->has_wifi = src->metadata.hasWifi;
        dst->has_bluetooth_radio = src->metadata.hasBluetooth;
        dst->has_ethernet = src->metadata.hasEthernet;
        dst->has_pkc = src->metadata.hasPKC;
        dst->can_shutdown = src->metadata.canShutdown;
    }
}

/*
 * Watching a message the user just sent, so its delivery result reaches them. Only a DM with
 * want_ack has one to wait for; a broadcast is fire-and-forget and is never watched.
 */
void mesh_app_watch_sent(struct mesh_app *app, uint32_t packet_id, const char *peer) {
    if (app == NULL || packet_id == 0U) {
        return;
    }
    const size_t capacity = sizeof app->ui_sent_watch / sizeof app->ui_sent_watch[0];
    if (app->ui_sent_watch_count >= capacity) {
        /* Drop the oldest: a result nobody has seen in eight messages is not news any more. */
        memmove(&app->ui_sent_watch[0], &app->ui_sent_watch[1],
                (capacity - 1U) * sizeof app->ui_sent_watch[0]);
        app->ui_sent_watch_count = capacity - 1U;
    }
    struct mesh_app_sent_watch *slot = &app->ui_sent_watch[app->ui_sent_watch_count++];
    slot->packet_id = packet_id;
    snprintf(slot->peer, sizeof slot->peer, "%s", peer != NULL ? peer : "");
}

/* Announces the delivery result of anything being watched, once. Failures only: a delivered
   message already shows "ok" on its row, and a toast per message would be noise. */
static void mesh_app_report_delivery(struct mesh_app *app) {
    if (app == NULL || app->ui_sent_watch_count == 0U) {
        return;
    }
    const struct mesh_message_log *log = mesh_session_messages(&app->session);
    if (log == NULL) {
        return;
    }

    size_t kept = 0U;
    for (size_t i = 0; i < app->ui_sent_watch_count; ++i) {
        struct mesh_app_sent_watch *watch = &app->ui_sent_watch[i];
        const struct mesh_message *message = NULL;
        for (size_t j = 0; j < log->count; ++j) {
            const struct mesh_message *entry = mesh_message_log_at(log, j);
            if (entry != NULL && entry->packet_id == watch->packet_id) {
                message = entry;
                break;
            }
        }
        if (message == NULL) {
            continue; /* evicted from the ring; nothing left to report */
        }
        if (message->ack == MESH_MESSAGE_ACK_PENDING) {
            app->ui_sent_watch[kept++] = *watch;
            continue;
        }
        if (message->ack == MESH_MESSAGE_ACK_FAILED) {
            char toast[MESH_UI_NAV_TOAST_MAX];
            snprintf(toast, sizeof toast, "Not delivered to %.16s: %s", watch->peer,
                     mesh_message_ack_error_to_string(message->ack_error));
            mesh_ui_store_set_toast(&app->ui_store, mesh_time_monotonic_ms(), toast);
        }
    }
    app->ui_sent_watch_count = kept;
}

void mesh_app_publish_ui_state(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }

    mesh_ui_store_tick(&app->ui_store, mesh_time_monotonic_ms());
    mesh_app_report_delivery(app);

    struct mesh_transport *ble = mesh_ble_transport();
    if (ble == NULL) {
        mesh_ui_store_set_transport_status(&app->ui_store, "unavailable");
        return;
    }

    const struct mesh_transport *active = mesh_app_active_transport();
    const char *transport_status =
        (active != NULL && active->ops != NULL && active->ops->status != NULL)
            ? active->ops->status(active)
            : NULL;
    mesh_ui_store_set_transport_status(&app->ui_store,
                                       transport_status != NULL ? transport_status : "unknown");

    struct mesh_ui_device ui_devices[MESH_UI_MAX_DEVICES];
    memset(ui_devices, 0, sizeof(ui_devices));
    size_t device_count = 0U;

    const char *connected_address = mesh_app_connected_identifier();
    bool connected_address_seen = false;

    /* Announce a dropped link once; auto-connect brings it back and the footer tracks it. */
    const bool link_connected = (connected_address != NULL && connected_address[0] != '\0');
    if (app->ui_link_was_connected && !link_connected &&
        app->config.run_mode == MESH_APP_RUN_FOREGROUND) {
        mesh_ui_store_set_toast(&app->ui_store, mesh_time_monotonic_ms(),
                                "Radio link lost; reconnecting");
    }
    app->ui_link_was_connected = link_connected;

    if (link_connected) {
        app->ui_report_link_error = false;
    }

    /* USB ports first: a plugged-in node needs no pairing and no range, so it is the one you
       almost always want, and putting it at the top makes it the default cursor row. */
    struct mesh_serial_device_info serial_devices[MESH_SERIAL_MAX_DEVICES];
    const size_t serial_count = mesh_serial_transport_get_devices(
        mesh_serial_transport(), serial_devices, MESH_SERIAL_MAX_DEVICES);
    for (size_t i = 0; i < serial_count && device_count < MESH_UI_MAX_DEVICES; ++i) {
        struct mesh_ui_device *slot = &ui_devices[device_count];
        /* Before the bind there is no tty, so the sysfs id is all we can address it by. */
        const char *identifier =
            serial_devices[i].path[0] != '\0' ? serial_devices[i].path : serial_devices[i].id;
        snprintf(slot->identifier, sizeof slot->identifier, "%s", identifier);
        snprintf(slot->name, sizeof slot->name, "%s", serial_devices[i].name);
        slot->kind = (uint8_t)MESH_UI_DEVICE_SERIAL;
        slot->rssi = 0;
        slot->paired = true; /* a cable has nothing to bond */
        slot->connected = (connected_address != NULL && connected_address[0] != '\0' &&
                           strcmp(connected_address, identifier) == 0);
        if (slot->connected) {
            connected_address_seen = true;
        }
        ++device_count;
    }

    struct mesh_bluez_device_info ble_devices[MESH_UI_MAX_DEVICES];
    const size_t ble_count = mesh_ble_transport_get_devices(ble, ble_devices, MESH_UI_MAX_DEVICES);
    /* Which row the link is working on. Not the same as connected: a BLE connect is several
       seconds of pairing and service discovery before it is a connection. */
    const char *pending_address = mesh_ble_transport_pending_address(ble);
    for (size_t i = 0; i < ble_count && device_count < MESH_UI_MAX_DEVICES; ++i) {
        struct mesh_ui_device *slot = &ui_devices[device_count];
        snprintf(slot->identifier, sizeof slot->identifier, "%s", ble_devices[i].address);
        snprintf(slot->name, sizeof slot->name, "%s", ble_devices[i].name);
        slot->kind = (uint8_t)MESH_UI_DEVICE_BLE;
        int16_t rssi = ble_devices[i].rssi;
        if (rssi < INT8_MIN) {
            rssi = INT8_MIN;
        } else if (rssi > INT8_MAX) {
            rssi = INT8_MAX;
        }
        slot->rssi = (int8_t)rssi;
        slot->paired = ble_devices[i].paired;
        slot->busy =
            (pending_address != NULL && strcmp(pending_address, ble_devices[i].address) == 0);
        slot->connected = (connected_address != NULL && connected_address[0] != '\0' &&
                           strcmp(connected_address, ble_devices[i].address) == 0);
        if (slot->connected) {
            connected_address_seen = true;
        }
        ++device_count;
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

    /*
     * The BlueZ pairing agent's question, if it has one. It arrives in the middle of a connect
     * and BlueZ holds the bond open until it is answered, so the prompt is raised from here
     * rather than by a key press - and taken down again the moment the request is gone,
     * however it ended.
     */
    {
        struct mesh_ble_pairing_request request;
        if (mesh_ble_transport_pairing_request(ble, &request)) {
            /* The advertised name beats a MAC on a 40-column prompt. */
            char label[MESH_UI_NAV_TARGET_NAME_MAX];
            snprintf(label, sizeof label, "%s", request.label);
            for (size_t i = 0; i < device_count; ++i) {
                if (ui_devices[i].kind == (uint8_t)MESH_UI_DEVICE_BLE &&
                    ui_devices[i].name[0] != '\0' &&
                    strcmp(ui_devices[i].identifier, request.address) == 0) {
                    mesh_str_copy(label, sizeof label, ui_devices[i].name);
                    break;
                }
            }
            mesh_ui_store_open_passkey_prompt(&app->ui_store, label, request.passkey,
                                              request.kind ==
                                                  (uint8_t)MESH_BLUEZ_AGENT_REQUEST_CONFIRM);
        } else {
            mesh_ui_store_close_passkey_prompt(&app->ui_store);
        }
    }

    bool preferences_modified = false;
    if (connected_address != NULL && connected_address[0] != '\0') {
        const uint8_t connected_kind = (active == mesh_serial_transport())
                                           ? (uint8_t)MESH_UI_DEVICE_SERIAL
                                           : (uint8_t)MESH_UI_DEVICE_BLE;
        if (strcmp(app->ui_preferences.preferred_device, connected_address) != 0 ||
            app->ui_preferences.preferred_device_kind != connected_kind) {
            snprintf(app->ui_preferences.preferred_device,
                     sizeof app->ui_preferences.preferred_device, "%s", connected_address);
            app->ui_preferences.preferred_device_kind = connected_kind;
            preferences_modified = true;
        }
    }

    struct mesh_handshake_status status = *mesh_session_handshake(&app->session);
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
        /* The roster outlives the connection, so a node list on screen is not proof of a live
           sync: what makes it live is something from this connection having arrived. */
        ui_handshake.roster_owner = mesh_session_roster_owner(&app->session);
        ui_handshake.cached = !status.config_complete && !status.has_my_info &&
                              !status.request_in_flight && !status.has_config;
        if (status.has_my_info) {
            const uint32_t my_node = status.my_info.my_node_num;
            /* Remember this radio as one of ours. Pins live in the radio's own NodeDB, so
               without this the node you connect to today is a stranger on the node you
               connect to tomorrow; see mesh_app_node_rank(). */
            if (mesh_ui_preferences_note_radio(&app->ui_preferences, my_node)) {
                preferences_modified = true;
            }
            ui_handshake.my_info.node_num = status.my_info.my_node_num;
            ui_handshake.my_info.nodedb_entries = status.my_info.nodedb_count;
            ui_handshake.my_info.reboot_count = status.my_info.reboot_count;
            for (size_t i = 0; i < status.node_count && i < MESH_SESSION_MAX_NODES; ++i) {
                if (status.nodes[i].node_id == my_node && status.nodes[i].short_name[0] != '\0') {
                    snprintf(ui_handshake.my_short_name, sizeof(ui_handshake.my_short_name), "%s",
                             status.nodes[i].short_name);
                    break;
                }
            }
        }

        /* The UI carries fewer nodes than a real mesh has. Rank them so the ones that matter
           survive the cut: ourselves, then pinned nodes, then our other radios, then anyone we
           have exchanged messages with, then nodes heard directly over RF by last_heard, then
           MQTT-fed nodes by last_heard. On a mesh with an MQTT uplink dozens of far-away nodes
           are "heard" every minute and would otherwise push the radio you are actually talking
           to off the list. Insertion sort: MESH_SESSION_MAX_NODES is small and this runs once
           per publish. */
        const struct mesh_message_log *message_log = mesh_session_messages(&app->session);
        size_t order[MESH_SESSION_MAX_NODES];
        unsigned rank[MESH_SESSION_MAX_NODES];
        size_t total =
            status.node_count > MESH_SESSION_MAX_NODES ? MESH_SESSION_MAX_NODES : status.node_count;
        const uint32_t my_node = status.has_my_info ? status.my_info.my_node_num : 0U;
        for (size_t i = 0; i < total; ++i) {
            const struct mesh_node_summary *node = &status.nodes[i];
            rank[i] = mesh_app_node_rank(node, my_node, message_log, &app->ui_preferences);
            size_t j = i;
            while (j > 0U) {
                const size_t prev_index = order[j - 1U];
                const struct mesh_node_summary *prev = &status.nodes[prev_index];
                if (rank[prev_index] < rank[i] ||
                    (rank[prev_index] == rank[i] && prev->last_heard >= node->last_heard)) {
                    break;
                }
                order[j] = prev_index;
                --j;
            }
            order[j] = i;
        }

        size_t copy_count = total;
        if (copy_count > MESH_UI_MAX_HANDSHAKE_NODES) {
            copy_count = MESH_UI_MAX_HANDSHAKE_NODES;
        }
        for (size_t i = 0; i < copy_count; ++i) {
            const struct mesh_node_summary *src = &status.nodes[order[i]];
            struct mesh_ui_node_summary *dst = &ui_handshake.nodes[i];
            dst->node_id = src->node_id;
            snprintf(dst->long_name, sizeof(dst->long_name), "%s", src->long_name);
            snprintf(dst->short_name, sizeof(dst->short_name), "%s", src->short_name);
            dst->last_heard = src->last_heard;
            dst->snr = src->snr;
            dst->via_mqtt = src->via_mqtt;
            dst->has_hops_away = src->has_hops_away;
            dst->hops_away = src->hops_away;
            snprintf(dst->user_id, sizeof(dst->user_id), "%s", src->user_id);
            dst->has_user = src->has_user;
            dst->in_nodedb = src->in_nodedb;
            dst->hw_model = src->hw_model;
            dst->role = src->role;
            dst->is_licensed = src->is_licensed;
            dst->is_unmessagable = src->is_unmessagable;
            dst->public_key_len = src->public_key_len > sizeof(dst->public_key)
                                      ? (uint8_t)sizeof(dst->public_key)
                                      : src->public_key_len;
            memcpy(dst->public_key, src->public_key, dst->public_key_len);
            dst->is_favorite = src->is_favorite;
            dst->is_ignored = src->is_ignored;
            dst->is_muted = src->is_muted;
            dst->channel = src->channel;
            mesh_app_copy_node_detail(src, dst);
        }
        ui_handshake.node_count = (uint32_t)copy_count;

        size_t channel_count = status.channel_count;
        if (channel_count > MESH_UI_MAX_CHANNELS) {
            channel_count = MESH_UI_MAX_CHANNELS;
        }
        for (size_t i = 0; i < channel_count; ++i) {
            ui_handshake.channels[i].index = status.channels[i].index;
            ui_handshake.channels[i].role = status.channels[i].role;
            ui_handshake.channels[i].psk_len = status.channels[i].psk_len;
            ui_handshake.channels[i].uplink_enabled = status.channels[i].uplink_enabled;
            ui_handshake.channels[i].downlink_enabled = status.channels[i].downlink_enabled;
            ui_handshake.channels[i].position_precision = status.channels[i].position_precision;
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

    mesh_app_publish_messages(app, &status);

    const struct mesh_radio_settings *radio_settings = mesh_session_settings(&app->session);
    struct mesh_ui_settings ui_settings;
    mesh_app_flatten_settings(radio_settings, &ui_settings);
    /* flatten_settings() zeroes the struct, so the client's own facts go in after it. */
    mesh_app_flatten_client_info(app, &ui_settings.client);
    /* Where the radio says it is, which is not part of PositionConfig: it comes from our own
       node's record, and it is what the Position section's coordinate rows start from. */
    if (status.has_my_info) {
        const struct mesh_node_summary *self = NULL;
        for (size_t i = 0; i < status.node_count && i < MESH_SESSION_MAX_NODES; ++i) {
            if (status.nodes[i].node_id == status.my_info.my_node_num) {
                self = &status.nodes[i];
                break;
            }
        }
        if (self != NULL && self->position.valid) {
            ui_settings.has_own_position = true;
            ui_settings.own_latitude_i = self->position.latitude_i;
            ui_settings.own_longitude_i = self->position.longitude_i;
            ui_settings.has_own_altitude = self->position.has_altitude;
            ui_settings.own_altitude = self->position.altitude;
        }
    }
    mesh_app_flatten_radio_stats(mesh_session_radio_stats(&app->session), &ui_settings.stats);
    mesh_ui_store_set_settings(&app->ui_store, &ui_settings);
    mesh_app_track_settings_save(app, radio_settings, link_connected);

    struct mesh_ui_traceroute ui_traceroute;
    mesh_app_flatten_traceroute(&status, mesh_session_traceroute(&app->session),
                                status.has_my_info ? status.my_info.my_node_num : 0U,
                                &ui_traceroute);
    mesh_ui_store_set_traceroute(&app->ui_store, &ui_traceroute);

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

    /* Marking a conversation read touches nothing else, so watch the stamp for it. */
    if (app->ui_store.read_state.stamp != app->ui_read_state_stamp) {
        app->ui_read_state_stamp = app->ui_store.read_state.stamp;
        if (app->ui_handshake_cache_path[0] != '\0') {
            app->ui_handshake_cache_dirty = true;
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
