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

#include "mesh/i18n/strings.h"

#include "mesh/core/version.h"
#include "mesh/geo/coords.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/serial.h"
#include "mesh/transport/tcp.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/preferences.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* A fixed two-second batching window, not a sliding debounce: a busy radio must still
   reach disk. A failed save stays dirty and is retried in the next window. */
void mesh_app_flush_ui_cache(struct mesh_app *app) {
    if (app->ui_handshake_cache_dirty && app->ui_handshake_cache_path[0] != '\0') {
        const int result = mesh_ui_store_save(&app->ui_store, app->ui_handshake_cache_path);
        if (result == 0) {
            app->ui_handshake_cache_dirty = false;
        } else {
            mesh_log_debug("app", "Failed to persist handshake cache: %d", result);
        }
    }
}

void mesh_app_close_ui_cache_timer(struct mesh_app *app) {
    if (app->ui_cache_timer_armed) {
        mesh_event_loop_remove_fd(&app->loop, app->ui_cache_timer_fd);
        close(app->ui_cache_timer_fd);
        app->ui_cache_timer_armed = false;
        app->ui_cache_timer_fd = -1;
    }
}

static void mesh_app_schedule_ui_cache(struct mesh_app *app);

static int mesh_app_ui_cache_timer(int fd, uint32_t events, void *userdata) {
    (void)events;
    uint64_t count;
    if (read(fd, &count, sizeof count) != sizeof count) {
        return 0;
    }
    struct mesh_app *app = userdata;
    mesh_app_close_ui_cache_timer(app);
    mesh_app_flush_ui_cache(app);
    mesh_app_schedule_ui_cache(app);
    return 0;
}

static void mesh_app_schedule_ui_cache(struct mesh_app *app) {
    if (!app->ui_handshake_cache_dirty || app->ui_handshake_cache_path[0] == '\0' ||
        app->ui_cache_timer_armed) {
        return;
    }
    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd >= 0) {
        int result = mesh_event_loop_add_fd(&app->loop, fd, EPOLLIN, mesh_app_ui_cache_timer, app);
        const struct itimerspec spec = {.it_value = {.tv_sec = 2}};
        if (result == 0) {
            if (timerfd_settime(fd, 0, &spec, NULL) == 0) {
                app->ui_cache_timer_fd = fd;
                app->ui_cache_timer_armed = true;
                return;
            }
            mesh_event_loop_remove_fd(&app->loop, fd);
        }
        close(fd);
    }
    /* Persistence still works when an event source cannot be allocated. */
    mesh_app_flush_ui_cache(app);
}

/* Compare source state before formatting, ranking and merging it. Exact comparisons avoid
   missed updates from mutation paths that do not yet expose revision counters. Padding can
   cause an extra rebuild, but cannot hide a changed field. Allocation failure keeps the
   uncached path working. Dynamic client status is deliberately rebuilt every publish. */
struct mesh_app_publish_cache {
    bool valid;
    const struct mesh_i18n_locale *locale;
    uint32_t roster_owner;
    /* Not part of `handshake`: the session's send path is not a field of the handshake status,
       so a drop that changed nothing else in that struct would not republish without this. */
    bool link_up;
    struct mesh_handshake_status handshake;
    struct mesh_message_log messages;
    struct mesh_waypoint_book waypoints;
    struct mesh_ui_message_list restored_messages;
    struct mesh_ui_preferences preferences;
    struct mesh_radio_settings settings;
    struct mesh_ui_settings flat_settings;
};

uint8_t mesh_app_primary_channel(const struct mesh_handshake_status *status) {
    if (status == NULL) {
        return 0U;
    }
    for (size_t i = 0; i < status->channel_count && i < MESH_SESSION_MAX_CHANNELS; ++i) {
        if (status->channels[i].role == meshtastic_Channel_Role_PRIMARY) {
            return status->channels[i].index;
        }
    }
    return 0U;
}

/* Resolves a node number to something a human can read, preferring the short name the NodeDB
   gave us and falling back to the Meshtastic-style "!hex" id. */
void mesh_app_format_peer_name(const struct mesh_handshake_status *status, uint32_t node_id,
                               char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }

    if (node_id == MESH_MESSAGE_BROADCAST_ADDR) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_PEER_EVERYONE));
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

/* Position, device metrics, environment and the four sensor groups, from the session's structs
   into the UI's twins.
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
    dst->position.received = src->position.received;
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

    dst->power.valid = src->power.valid;
    dst->power.time = src->power.time;
    for (size_t ch = 0; ch < sizeof dst->power.channel / sizeof dst->power.channel[0]; ++ch) {
        dst->power.channel[ch].has_voltage = src->power.channel[ch].has_voltage;
        dst->power.channel[ch].voltage = src->power.channel[ch].voltage;
        dst->power.channel[ch].has_current = src->power.channel[ch].has_current;
        dst->power.channel[ch].current = src->power.channel[ch].current;
    }

    dst->air_quality.valid = src->air_quality.valid;
    dst->air_quality.time = src->air_quality.time;
    dst->air_quality.has_pm10 = src->air_quality.has_pm10;
    dst->air_quality.pm10_standard = src->air_quality.pm10_standard;
    dst->air_quality.has_pm25 = src->air_quality.has_pm25;
    dst->air_quality.pm25_standard = src->air_quality.pm25_standard;
    dst->air_quality.has_pm100 = src->air_quality.has_pm100;
    dst->air_quality.pm100_standard = src->air_quality.pm100_standard;
    dst->air_quality.has_co2 = src->air_quality.has_co2;
    dst->air_quality.co2 = src->air_quality.co2;
    dst->air_quality.has_voc_index = src->air_quality.has_voc_index;
    dst->air_quality.voc_index = src->air_quality.voc_index;
    dst->air_quality.has_nox_index = src->air_quality.has_nox_index;
    dst->air_quality.nox_index = src->air_quality.nox_index;

    dst->health.valid = src->health.valid;
    dst->health.time = src->health.time;
    dst->health.has_heart_bpm = src->health.has_heart_bpm;
    dst->health.heart_bpm = src->health.heart_bpm;
    dst->health.has_spo2 = src->health.has_spo2;
    dst->health.spo2 = src->health.spo2;
    dst->health.has_temperature = src->health.has_temperature;
    dst->health.temperature = src->health.temperature;

    dst->host.valid = src->host.valid;
    dst->host.time = src->host.time;
    dst->host.has_uptime = src->host.has_uptime;
    dst->host.uptime_seconds = src->host.uptime_seconds;
    dst->host.has_freemem = src->host.has_freemem;
    dst->host.freemem_kib = src->host.freemem_kib;
    dst->host.has_diskfree = src->host.has_diskfree;
    dst->host.diskfree_mib = src->host.diskfree_mib;
    dst->host.has_load = src->host.has_load;
    dst->host.load1 = src->host.load1;
    dst->host.load5 = src->host.load5;
    dst->host.load15 = src->host.load15;

    dst->neighbors.valid = src->neighbors.valid;
    dst->neighbors.time = src->neighbors.time;
    dst->neighbors.broadcast_interval_secs = src->neighbors.broadcast_interval_secs;
    dst->neighbors.count = src->neighbors.count > MESH_UI_MAX_NEIGHBORS
                               ? (uint8_t)MESH_UI_MAX_NEIGHBORS
                               : src->neighbors.count;
    for (uint8_t n = 0; n < dst->neighbors.count; ++n) {
        dst->neighbors.entries[n].node_id = src->neighbors.entries[n].node_id;
        dst->neighbors.entries[n].snr = src->neighbors.entries[n].snr;
    }
}

/*
 * The same copy the other way, for the roster the last run left on disk. Only the fields the
 * cache actually persists are restored - position, metrics, environment and the four sensor
 * groups come back through their own keys - so a node returns as what we knew, not as a blank
 * with a name.
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
    dst->has_rssi = src->has_rssi;
    dst->rx_rssi = src->rx_rssi;
    dst->rssi_time = src->rssi_time;
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
    dst->position.received = src->position.received;
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

    dst->power.valid = src->power.valid;
    dst->power.time = src->power.time;
    for (size_t ch = 0; ch < sizeof dst->power.channel / sizeof dst->power.channel[0]; ++ch) {
        dst->power.channel[ch].has_voltage = src->power.channel[ch].has_voltage;
        dst->power.channel[ch].voltage = src->power.channel[ch].voltage;
        dst->power.channel[ch].has_current = src->power.channel[ch].has_current;
        dst->power.channel[ch].current = src->power.channel[ch].current;
    }

    dst->air_quality.valid = src->air_quality.valid;
    dst->air_quality.time = src->air_quality.time;
    dst->air_quality.has_pm10 = src->air_quality.has_pm10;
    dst->air_quality.pm10_standard = src->air_quality.pm10_standard;
    dst->air_quality.has_pm25 = src->air_quality.has_pm25;
    dst->air_quality.pm25_standard = src->air_quality.pm25_standard;
    dst->air_quality.has_pm100 = src->air_quality.has_pm100;
    dst->air_quality.pm100_standard = src->air_quality.pm100_standard;
    dst->air_quality.has_co2 = src->air_quality.has_co2;
    dst->air_quality.co2 = src->air_quality.co2;
    dst->air_quality.has_voc_index = src->air_quality.has_voc_index;
    dst->air_quality.voc_index = src->air_quality.voc_index;
    dst->air_quality.has_nox_index = src->air_quality.has_nox_index;
    dst->air_quality.nox_index = src->air_quality.nox_index;

    dst->health.valid = src->health.valid;
    dst->health.time = src->health.time;
    dst->health.has_heart_bpm = src->health.has_heart_bpm;
    dst->health.heart_bpm = src->health.heart_bpm;
    dst->health.has_spo2 = src->health.has_spo2;
    dst->health.spo2 = src->health.spo2;
    dst->health.has_temperature = src->health.has_temperature;
    dst->health.temperature = src->health.temperature;

    dst->host.valid = src->host.valid;
    dst->host.time = src->host.time;
    dst->host.has_uptime = src->host.has_uptime;
    dst->host.uptime_seconds = src->host.uptime_seconds;
    dst->host.has_freemem = src->host.has_freemem;
    dst->host.freemem_kib = src->host.freemem_kib;
    dst->host.has_diskfree = src->host.has_diskfree;
    dst->host.diskfree_mib = src->host.diskfree_mib;
    dst->host.has_load = src->host.has_load;
    dst->host.load1 = src->host.load1;
    dst->host.load5 = src->host.load5;
    dst->host.load15 = src->host.load15;

    dst->neighbors.valid = src->neighbors.valid;
    dst->neighbors.time = src->neighbors.time;
    dst->neighbors.broadcast_interval_secs = src->neighbors.broadcast_interval_secs;
    dst->neighbors.count = src->neighbors.count > MESH_NODE_MAX_NEIGHBORS
                               ? (uint8_t)MESH_NODE_MAX_NEIGHBORS
                               : src->neighbors.count;
    for (uint8_t n = 0; n < dst->neighbors.count; ++n) {
        dst->neighbors.entries[n].node_id = src->neighbors.entries[n].node_id;
        dst->neighbors.entries[n].snr = src->neighbors.entries[n].snr;
    }
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
    /* What the roster already knew before any radio was attached. Without this baseline the
       first sync of every launch would read a cache full of orphans as news and say so. */
    app->ui_nodes_off_radio_seen = mesh_session_forgettable_nodes(&app->session, true);
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
/*
 * Copies the session's waypoint book into the store, resolving each sharer's name and whether
 * this client may withdraw the place.
 *
 * Both derived fields need our own node number, which is why they are settled here rather than
 * in a screen: `editable` decides whether the delete row offers to withdraw a place from the
 * mesh or only to stop showing it, and a screen that worked that out itself would be a second
 * opinion about it beside mesh_session_forget_waypoint()'s.
 */
static void mesh_app_publish_waypoints(struct mesh_app *app,
                                       const struct mesh_handshake_status *status) {
    const struct mesh_waypoint_book *book = mesh_session_waypoints(&app->session);
    if (book == NULL) {
        return;
    }
    const uint32_t me = status->has_my_info ? status->my_info.my_node_num : 0U;

    struct mesh_ui_waypoint_list list;
    memset(&list, 0, sizeof list);
    list.dropped = book->dropped;
    for (size_t i = 0; i < book->count && list.count < MESH_UI_MAX_WAYPOINTS; ++i) {
        const struct mesh_waypoint *source = mesh_waypoint_book_at(book, i);
        if (source == NULL) {
            continue;
        }
        struct mesh_ui_waypoint *target = &list.entries[list.count];
        target->id = source->id;
        target->latitude_i = source->latitude_i;
        target->longitude_i = source->longitude_i;
        target->has_coords = source->has_coords;
        target->expire = source->expire;
        target->locked_to = source->locked_to;
        target->icon = source->icon;
        target->from = source->from;
        target->heard = source->heard;
        target->channel = source->channel;
        target->ours = source->ours || (me != 0U && source->from == me);
        target->editable = (source->locked_to == 0U) || (me != 0U && source->locked_to == me);
        mesh_str_copy(target->name, sizeof target->name, source->name);
        mesh_str_copy(target->description, sizeof target->description, source->description);
        if (source->from != 0U) {
            mesh_app_format_peer_name(status, source->from, target->from_name,
                                      sizeof target->from_name);
        }
        list.count++;
    }

    mesh_ui_store_set_waypoints(&app->ui_store, &list);
}

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
        target->kind = source->kind;
        target->ack = source->ack;
        target->ack_error = source->ack_error;
        target->broadcast = (source->to == MESH_MESSAGE_BROADCAST_ADDR);
        target->pki_encrypted = source->pki_encrypted;
        target->reply_id = source->reply_id;
        target->is_reaction = source->is_reaction;
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

/* The radio's own announcements, copied by hand for the reason above. Both are always copied,
   valid or not: `seq` 0 and `valid` false are what the renderers read as "nothing yet", and
   zeroing them here is what clears the last radio's words when a new one connects. */
static void mesh_app_flatten_radio_notice(const struct mesh_client_notification *src,
                                          struct mesh_ui_radio_notice *dst) {
    memset(dst, 0, sizeof *dst);
    if (src == NULL || src->seq == 0U) {
        return;
    }
    dst->seq = src->seq;
    dst->time = src->time;
    dst->received = src->received;
    dst->level = src->level;
    mesh_str_copy(dst->text, sizeof dst->text, src->text);
}

static void mesh_app_flatten_queue_status(const struct mesh_queue_status *src,
                                          struct mesh_ui_queue_status *dst) {
    memset(dst, 0, sizeof *dst);
    if (src == NULL || !src->valid) {
        return;
    }
    dst->valid = true;
    dst->res = src->res;
    dst->free = src->free;
    dst->maxlen = src->maxlen;
}

/* The router's name comes out of the roster, so this one takes the handshake as well - a
   heartbeat is often the only packet a router sends, and the roster is what turns the node
   number it arrived from into something a row can say. */
static void mesh_app_flatten_store_forward(const struct mesh_handshake_status *status,
                                           const struct mesh_store_forward *src,
                                           struct mesh_ui_store_forward *dst) {
    memset(dst, 0, sizeof *dst);
    if (src == NULL) {
        return;
    }
    dst->state = src->state;
    dst->router = src->router;
    if (src->router != 0U) {
        mesh_app_format_peer_name(status, src->router, dst->router_name, sizeof dst->router_name);
    }
    dst->router_secondary = src->router_secondary;
    dst->expected = src->expected;
    dst->received = src->received;
    dst->stored = src->stored;
    dst->seq = src->seq;
    dst->has_stats = src->has_stats;
    dst->messages_saved = src->messages_saved;
    dst->messages_max = src->messages_max;
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

    /* The theme every backend draws this frame with. Published like any other fact about the
       client, so the switch needs no path of its own down to the renderer. */
    const struct mesh_ui_theme *theme = app->ui_theme;
    snprintf(dst->theme, sizeof dst->theme, "%s", theme != NULL ? theme->id : "");
    snprintf(dst->theme_name, sizeof dst->theme_name, "%s", theme != NULL ? theme->name : "");
    /* Straight off the locale rather than out of the catalog: `name` is a required field of
       every locale and is always that language's own name for itself, whereas a catalog entry
       is optional by design - a partial translation that had not got to it yet would fall back
       to English and make About report the wrong language. */
    dst->language_from_env = mesh_i18n_is_overridden();
    snprintf(dst->language_name, sizeof dst->language_name, "%s", mesh_i18n_locale()->name);
    dst->theme_from_env = app->ui_theme_from_env;

    const struct mesh_updater *updater = &app->updater;
    dst->update_state = (uint8_t)updater->state;
    dst->update_supported = mesh_updater_available(updater);
    dst->update_busy =
        updater->state == MESH_UPDATE_CHECKING || updater->state == MESH_UPDATE_DOWNLOADING;
    dst->update_can_install = mesh_updater_can_install(updater);
    dst->update_is_release = mesh_version_is_release();
    dst->update_allow_dev = updater->allow_dev;
    dst->update_allow_dev_from_env = updater->allow_dev_from_env;
    uint32_t progress = 0U;
    dst->update_progress_known = mesh_updater_progress(updater, &progress);
    dst->update_progress = (uint16_t)progress;
    snprintf(dst->update_message, sizeof dst->update_message, "%s", updater->message);
    snprintf(dst->update_latest, sizeof dst->update_latest, "%s", updater->latest);
    snprintf(dst->update_channel, sizeof dst->update_channel, "%s",
             mesh_update_channel_name(updater->channel));
}

/*
 * Which bus the radio is on, as the firmware module's own idea of a path.
 *
 * Derived here rather than recorded when a link comes up: a serial link is over the same USB
 * port a UF2 write would use and a BLE link is where an OTA would happen, so "could this be
 * done from where we are standing" is one comparison - and a flag set at connect time is a
 * second opinion about a fact the transport registry already holds.
 */
static enum mesh_firmware_path mesh_app_firmware_bus(void) {
    if (mesh_app_connected_identifier() == NULL) {
        return MESH_FIRMWARE_PATH_NONE;
    }
    return mesh_app_active_transport() == mesh_serial_transport() ? MESH_FIRMWARE_PATH_USB
                                                                  : MESH_FIRMWARE_PATH_BLE;
}

/*
 * The radio's firmware situation, flattened onto the settings snapshot - the same trick
 * flatten_client_info() plays for the client's own updater, and for the same reason: store.h
 * has no business seeing a module that forks child processes.
 *
 * Not part of the cached half above, because none of it comes out of mesh_radio_settings: a
 * check finishing changes these rows while the radio's own configuration has not moved.
 */
static void mesh_app_flatten_firmware(struct mesh_app *app, struct mesh_ui_settings *dst) {
    struct mesh_firmware *const firmware = &app->firmware;
    mesh_firmware_set_bus(firmware, mesh_app_firmware_bus());

    /*
     * A check whose answer is about a radio that is no longer the one on the other end.
     *
     * What makes an answer still this radio's is mesh_firmware_answers_for(), which owns the
     * test because it owns the answer. Asked here rather than hooked onto the swap in
     * session.c, so every route to another radio is covered by the one call.
     *
     * Gated on knowing a model, because a link that has merely *dropped* clears the metadata
     * and the answer is still worth reading with the radio back in a pocket.
     */
    const struct mesh_radio_settings *const radio = mesh_session_settings(&app->session);
    const bool known = radio != NULL && radio->has_metadata;
    const uint32_t model = known ? (uint32_t)radio->metadata.hw_model : 0U;
    if (model != 0U &&
        !mesh_firmware_answers_for(firmware, model, radio->metadata.firmware_version)) {
        mesh_firmware_forget(firmware);
    }

    dst->fw_supported = mesh_firmware_available(firmware);
    dst->fw_busy = mesh_firmware_busy(firmware);
    mesh_str_copy(dst->fw_channel, sizeof dst->fw_channel,
                  mesh_firmware_channel_name(firmware->channel));
    dst->fw_state = (uint8_t)firmware->state;
    mesh_str_copy(dst->fw_message, sizeof dst->fw_message, firmware->message);
    mesh_str_copy(dst->fw_latest, sizeof dst->fw_latest, firmware->release.version);

    const struct mesh_firmware_board *const board = mesh_firmware_board(firmware);
    mesh_str_copy(dst->fw_board, sizeof dst->fw_board, board != NULL ? board->name : "");

    /*
     * The wrong-bus refusal is the one the module cannot phrase on its own: which bus to go and
     * use is a property of the board, and the board is here. Everything else is one line the
     * module already knows.
     */
    if (firmware->blocker == MESH_FIRMWARE_BLOCKER_WRONG_BUS && board != NULL) {
        mesh_str_copy(dst->fw_blocker_reason, sizeof dst->fw_blocker_reason,
                      mesh_str(board->path == MESH_FIRMWARE_PATH_USB
                                   ? MESH_STR_FW_BLOCK_CONNECT_USB
                                   : MESH_STR_FW_BLOCK_CONNECT_BLE));
    } else {
        mesh_str_copy(dst->fw_blocker_reason, sizeof dst->fw_blocker_reason,
                      mesh_firmware_blocker_reason(firmware->blocker));
    }
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
    if (src->has_detection_sensor) {
        dst->has_detection_sensor = true;
        dst->detection_enabled = src->detection_sensor.enabled;
        dst->detection_minimum_broadcast_secs = src->detection_sensor.minimum_broadcast_secs;
        dst->detection_state_broadcast_secs = src->detection_sensor.state_broadcast_secs;
        dst->detection_send_bell = src->detection_sensor.send_bell;
        mesh_str_copy(dst->detection_name, sizeof dst->detection_name, src->detection_sensor.name);
        dst->detection_monitor_pin = src->detection_sensor.monitor_pin;
        dst->detection_trigger_type = (uint8_t)src->detection_sensor.detection_trigger_type;
        dst->detection_use_pullup = src->detection_sensor.use_pullup;
    }
    if (src->has_external_notification) {
        const meshtastic_ModuleConfig_ExternalNotificationConfig *ext = &src->external_notification;
        dst->has_external_notification = true;
        dst->extnotif_enabled = ext->enabled;
        dst->extnotif_output_ms = ext->output_ms;
        dst->extnotif_nag_timeout = ext->nag_timeout;
        dst->extnotif_active = ext->active;
        dst->extnotif_use_pwm = ext->use_pwm;
        dst->extnotif_use_i2s_as_buzzer = ext->use_i2s_as_buzzer;
        dst->extnotif_output = (uint8_t)ext->output;
        dst->extnotif_output_vibra = ext->output_vibra;
        dst->extnotif_output_buzzer = ext->output_buzzer;
        dst->extnotif_alert_message = ext->alert_message;
        dst->extnotif_alert_message_vibra = ext->alert_message_vibra;
        dst->extnotif_alert_message_buzzer = ext->alert_message_buzzer;
        dst->extnotif_alert_bell = ext->alert_bell;
        dst->extnotif_alert_bell_vibra = ext->alert_bell_vibra;
        dst->extnotif_alert_bell_buzzer = ext->alert_bell_buzzer;
    }
    if (src->has_traffic_management) {
        const meshtastic_ModuleConfig_TrafficManagementConfig *tm = &src->traffic_management;
        dst->has_traffic_management = true;
        dst->traffic_position_min_interval_secs = tm->position_min_interval_secs;
        dst->traffic_nodeinfo_max_hops = tm->nodeinfo_direct_response_max_hops;
        dst->traffic_rate_limit_window_secs = tm->rate_limit_window_secs;
        dst->traffic_rate_limit_max_packets = tm->rate_limit_max_packets;
        dst->traffic_unknown_packet_threshold = tm->unknown_packet_threshold;
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
    if (src->has_ui_config) {
        dst->has_ui_config = true;
        dst->ui_theme = (uint8_t)src->ui_config.theme;
        dst->ui_language = (uint32_t)src->ui_config.language;
        dst->ui_brightness = src->ui_config.screen_brightness;
        dst->ui_screen_timeout = src->ui_config.screen_timeout;
        dst->ui_alert_enabled = src->ui_config.alert_enabled;
        dst->ui_banner_enabled = src->ui_config.banner_enabled;
        dst->ui_ring_tone_id = src->ui_config.ring_tone_id;
        dst->ui_compass_mode = (uint8_t)src->ui_config.compass_mode;
        dst->ui_gps_format = (uint8_t)src->ui_config.gps_format;
        dst->ui_clockface_analog = src->ui_config.is_clockface_analog;
        dst->ui_screen_lock = src->ui_config.screen_lock;
        dst->ui_settings_lock = src->ui_config.settings_lock;
    }
    if (src->has_canned_messages) {
        dst->has_canned_messages = true;
        mesh_str_copy(dst->canned_messages, sizeof dst->canned_messages, src->canned_messages);
    }
    if (src->has_ringtone) {
        dst->has_ringtone = true;
        mesh_str_copy(dst->ringtone, sizeof dst->ringtone, src->ringtone);
    }
    if (src->has_connection_status) {
        const meshtastic_DeviceConnectionStatus *conn = &src->connection_status;
        struct mesh_ui_connection_status *out = &dst->connection;
        out->valid = true;
        if (conn->has_wifi) {
            out->has_wifi = true;
            out->wifi_connected = conn->wifi.status.is_connected;
            snprintf(out->wifi_ssid, sizeof out->wifi_ssid, "%s", conn->wifi.ssid);
            out->wifi_rssi = conn->wifi.rssi;
            out->wifi_ip = conn->wifi.status.ip_address;
            out->wifi_mqtt = conn->wifi.status.is_mqtt_connected;
            out->wifi_syslog = conn->wifi.status.is_syslog_connected;
        }
        if (conn->has_ethernet) {
            out->has_ethernet = true;
            out->ethernet_connected = conn->ethernet.status.is_connected;
            out->ethernet_ip = conn->ethernet.status.ip_address;
            out->ethernet_mqtt = conn->ethernet.status.is_mqtt_connected;
            out->ethernet_syslog = conn->ethernet.status.is_syslog_connected;
        }
        if (conn->has_bluetooth) {
            out->has_bluetooth = true;
            out->bluetooth_connected = conn->bluetooth.is_connected;
            out->bluetooth_pin = conn->bluetooth.pin;
            out->bluetooth_rssi = conn->bluetooth.rssi;
        }
        if (conn->has_serial) {
            out->has_serial = true;
            out->serial_connected = conn->serial.is_connected;
            out->serial_baud = conn->serial.baud;
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
            mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_NOT_DELIVERED, watch->peer,
                            mesh_message_ack_error_to_string(message->ack_error));
            mesh_ui_store_set_toast(&app->ui_store, mesh_time_monotonic_ms(), toast);
        }
    }
    app->ui_sent_watch_count = kept;
}

/*
 * Announces what the radio has said about itself since the last publish, once each.
 *
 * Both counters run forwards for the life of a connection and are reset to 0 with the
 * handshake, so "seq differs from what we last saw" is the test rather than "seq is greater":
 * a reconnect restarts the count at 1, and a greater-than test would swallow the first
 * notification of every connection after a talkative one.
 */
static void mesh_app_report_radio_notices(struct mesh_app *app) {
    const struct mesh_client_notification *notice = mesh_session_notification(&app->session);
    if (notice != NULL && notice->seq != app->ui_notice_seq_seen) {
        app->ui_notice_seq_seen = notice->seq;
        if (notice->seq != 0U && notice->text[0] != '\0' &&
            app->config.run_mode == MESH_APP_RUN_FOREGROUND) {
            /* The radio's words verbatim: it is describing a decision the firmware took, and
               nothing this side of the link knows how to say it better. */
            mesh_ui_store_set_toast(&app->ui_store, mesh_time_monotonic_ms(), notice->text);
        }
    }

    const uint32_t reboots = app->session.reboot_notices;
    if (reboots != app->ui_reboot_notices_seen) {
        app->ui_reboot_notices_seen = reboots;
        /* The session has already re-run the config sync by the time this is read; the toast
           exists so a screen that empties and refills looks like an event rather than a
           glitch. */
        if (reboots != 0U && app->config.run_mode == MESH_APP_RUN_FOREGROUND) {
            mesh_ui_store_set_toast(&app->ui_store, mesh_time_monotonic_ms(),
                                    mesh_str(MESH_STR_TOAST_RADIO_RESTARTED));
        }
    }
}

/*
 * Announces the newest unseen critical alert, once.
 *
 * The Messages tab may not be the one on screen, and an ALERT_APP message is by definition the
 * one thing the firmware expects a client to interrupt for. Only the newest is announced: three
 * alerts arriving together are one situation, and three toasts in a row would show the user the
 * last one anyway.
 *
 * Only inbound, and only alerts. A detection is a sensor announcing itself, which belongs in
 * the conversation and not in front of whatever the user is doing.
 */
static void mesh_app_report_alerts(struct mesh_app *app) {
    const struct mesh_message_log *log = mesh_session_messages(&app->session);
    if (log == NULL || log->count == 0U) {
        return;
    }

    const struct mesh_message *newest = NULL;
    for (size_t i = log->count; i > 0U; --i) {
        const struct mesh_message *entry = mesh_message_log_at(log, i - 1U);
        if (entry != NULL && entry->kind == MESH_MESSAGE_KIND_ALERT &&
            entry->direction == MESH_MESSAGE_INBOUND) {
            newest = entry;
            break;
        }
    }
    if (newest == NULL || newest->packet_id == app->ui_alert_announced_id) {
        return;
    }
    app->ui_alert_announced_id = newest->packet_id;
    if (app->config.run_mode != MESH_APP_RUN_FOREGROUND) {
        return;
    }

    /* The sender's name and then the alert itself, truncated by the toast rather than by us -
       the first words of an alert are the ones that say what it is. */
    char peer[MESH_UI_NAV_TARGET_NAME_MAX];
    mesh_app_format_peer_name(mesh_session_handshake(&app->session), newest->from, peer,
                              sizeof peer);
    char toast[MESH_UI_NAV_TOAST_MAX];
    /* Built in two steps rather than one snprintf: the alert body is up to 233 bytes against a
       64-byte toast, and letting one format truncate it is both a compiler warning and a
       formatting decision made by accident. The name is bounded first so the text keeps
       whatever room is left, because the first words of an alert are the ones that say what
       it is. */
    const int prefix = mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_ALERT_FROM, peer);
    if (prefix > 0 && (size_t)prefix < sizeof toast) {
        (void)mesh_str_copy(toast + prefix, sizeof toast - (size_t)prefix, newest->text);
    }
    mesh_ui_store_set_toast(&app->ui_store, mesh_time_monotonic_ms(), toast);
}

/*
 * Announces the newest unseen direct message, once.
 *
 * mesh_app_report_alerts()'s shape and its reasoning, with three conditions of its own, because
 * an ordinary message is worth less interruption than a critical alert and has to earn it:
 *
 *   - it is muted, and then it is not announced at all. That is the whole of what the press on
 *     the conversation list buys, and the predicate is the store's so that the tab badge, this
 *     notice and the row's own bell cannot disagree about who is muted.
 *   - the user is already looking at it. A notice sliding over the transcript to report the
 *     bubble that has just appeared on it is the client talking to itself; the all-traffic view
 *     counts as looking at it, because every conversation is on that screen.
 *   - it is the first pass. See ui_message_announce_primed.
 *
 * Every unseen one is announced rather than only the newest, which is where this parts company
 * with the alerts above and why the snackbar grew a queue. Three alerts arriving together are
 * one situation and the last of them describes it; three messages from three people are three
 * things somebody said to you, and showing the last is losing two. They post rather than set,
 * so they wait for whatever is on the snackbar instead of overwriting it - see
 * mesh_ui_nav_post_toast(). The queue's own depth is what bounds a burst.
 *
 * A log whose last announcement has since been evicted falls back to the newest alone: the
 * alternative is replaying however much of the ring is unaccounted for, which after a long
 * absence is the whole of it.
 */
static void mesh_app_report_direct_messages(struct mesh_app *app) {
    /*
     * The first pass adopts whatever the cache brought with it and says none of it - see
     * ui_message_announce_primed.
     *
     * Taken before the empty-log guard below, and that is the whole reason it is up here: a
     * client that starts with no cache at all would otherwise return without priming, and spend
     * the priming on the first message that genuinely arrived.
     */
    const bool announce = app->ui_message_announce_primed;
    app->ui_message_announce_primed = true;

    const struct mesh_message_log *log = mesh_session_messages(&app->session);
    if (log == NULL || log->count == 0U) {
        return;
    }

    /* Start after the last one announced. */
    size_t next = 0U;
    bool lost = false;
    if (app->ui_message_announced_id != 0U) {
        bool found = false;
        for (size_t i = 0; i < log->count; ++i) {
            const struct mesh_message *entry = mesh_message_log_at(log, i);
            if (entry != NULL && entry->packet_id == app->ui_message_announced_id) {
                next = i + 1U;
                found = true;
                break;
            }
        }
        /*
         * The message we last announced is not in the log any more, so where we had got to is
         * unknowable - and the honest answer to that is to say nothing and take our place again
         * from the newest, exactly as a launch does.
         *
         * The reachable way in is a *delete* rather than the ring: the log holds 64 and this
         * runs on every publish, so an eviction would need 64 messages between two turns of the
         * loop. Deleting the conversation the cursor was pointing into is one press, and
         * treating the deletion as "everything since is new" announced whatever inbound message
         * happened to be last - somebody else's, and one the user had already been told about,
         * arriving as a notice a second after they pressed delete.
         */
        if (!found) {
            lost = true;
            next = 0U;
        }
    }

    const struct mesh_ui_nav *nav = &app->ui_store.nav;
    const bool foreground = (app->config.run_mode == MESH_APP_RUN_FOREGROUND);

    for (size_t i = next; i < log->count; ++i) {
        const struct mesh_message *entry = mesh_message_log_at(log, i);
        if (entry == NULL) {
            continue;
        }
        /* A reaction has no bubble and no words to put in a notice - the whole of what it says
           is which message it is about - so it is skipped here exactly as the transcript and
           the unread count skip it. */
        if (entry->direction != MESH_MESSAGE_INBOUND || entry->is_reaction) {
            continue;
        }
        if (entry->to == MESH_MESSAGE_BROADCAST_ADDR) {
            continue;
        }
        /* An alert already announced itself, in its own words and its own sentence. */
        if (entry->kind != (uint8_t)MESH_MESSAGE_KIND_TEXT) {
            continue;
        }
        /* Nothing to remember it by, so it could never be marked announced. */
        if (entry->packet_id == 0U) {
            continue;
        }

        app->ui_message_announced_id = entry->packet_id;
        if (!announce || lost || !foreground) {
            continue;
        }
        if (mesh_ui_store_conversation_muted(&app->ui_store, (uint8_t)MESH_UI_CONVERSATION_DIRECT,
                                             entry->from, 0U)) {
            continue;
        }
        /* Already on screen: a notice reporting the bubble the reader is looking at is the
           client talking to itself. All traffic counts, because every conversation is there. */
        if (nav->thread_open && (nav->inbox || nav->target_node == entry->from)) {
            continue;
        }

        /* The sender's name, then what they said - the alert's two steps and for its reason: the
           body is up to 233 bytes against a 64-byte notice, and letting one format truncate the
           pair is a formatting decision made by accident. */
        char peer[MESH_UI_NAV_TARGET_NAME_MAX];
        mesh_app_format_peer_name(mesh_session_handshake(&app->session), entry->from, peer,
                                  sizeof peer);
        char toast[MESH_UI_NAV_TOAST_MAX];
        const int prefix = mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_MESSAGE_FROM, peer);
        if (prefix > 0 && (size_t)prefix < sizeof toast) {
            (void)mesh_str_copy(toast + prefix, sizeof toast - (size_t)prefix, entry->text);
        }
        mesh_ui_store_post_toast(&app->ui_store, mesh_time_monotonic_ms(), toast);
    }
}

/* Nodes that have to appear on one sync before the divergence is worth interrupting for. */
#define MESH_APP_OFF_RADIO_HINT_MIN 8U

/*
 * Announces the roster and the radio's NodeDB parting company, once per sync.
 *
 * The roster outliving the radio's database is the point of having one - the radio holds 80
 * entries and evicts, so ours is often the only copy - but it makes one moment look broken:
 * after a NodeDB reset the Status screen says two nodes and the Nodes tab says eighty-one,
 * with nothing on either screen saying why. The Nodes tab now marks those rows, and this says
 * it once, in front of whatever the user is looking at, with the row that clears them named.
 *
 * A rise since the last completed sync is the trigger, not the count: a roster that has held
 * the same orphans for a week is not news, and the threshold below keeps an ordinary connect -
 * where the radio has evicted a node or two since we last looked - from saying anything at all.
 *
 * It counts what the row it names would drop rather than every off-radio node, so a toast that
 * sends the user to Settings never sends them to a row that has nothing to do: a roster whose
 * orphans are all pinned says nothing here.
 */
static void mesh_app_report_off_radio_nodes(struct mesh_app *app) {
    const struct mesh_handshake_status *status = mesh_session_handshake(&app->session);
    if (status == NULL || !status->config_complete ||
        status->config_complete_id == app->ui_nodes_off_radio_sync_id) {
        return;
    }
    const uint32_t before = app->ui_nodes_off_radio_seen;
    const uint32_t now = mesh_session_forgettable_nodes(&app->session, true);
    app->ui_nodes_off_radio_sync_id = status->config_complete_id;
    app->ui_nodes_off_radio_seen = now;
    if (now <= before || now - before < MESH_APP_OFF_RADIO_HINT_MIN ||
        app->config.run_mode != MESH_APP_RUN_FOREGROUND) {
        return;
    }
    char toast[MESH_UI_NAV_TOAST_MAX];
    mesh_str_format(toast, sizeof toast, MESH_STR_TOAST_NODES_OFF_RADIO, now);
    mesh_ui_store_set_toast(&app->ui_store, mesh_time_monotonic_ms(), toast);
    mesh_log_info("app", "Roster and NodeDB diverged: %u off radio, was %u", now, before);
}

void mesh_app_publish_ui_state(struct mesh_app *app) {
    if (app == NULL) {
        return;
    }

    mesh_ui_store_tick(&app->ui_store, mesh_time_monotonic_ms());
    mesh_app_report_delivery(app);
    mesh_app_report_radio_notices(app);
    mesh_app_report_alerts(app);
    mesh_app_report_direct_messages(app);
    mesh_app_report_off_radio_nodes(app);

    struct mesh_transport *ble = mesh_ble_transport();
    if (ble == NULL) {
        mesh_ui_store_set_transport_status(&app->ui_store,
                                           mesh_str(MESH_STR_TRANSPORT_UNAVAILABLE));
        return;
    }

    const struct mesh_transport *active = mesh_app_active_transport();
    const char *transport_status =
        (active != NULL && active->ops != NULL && active->ops->status != NULL)
            ? active->ops->status(active)
            : NULL;
    mesh_ui_store_set_transport_status(&app->ui_store, transport_status != NULL
                                                           ? transport_status
                                                           : mesh_str(MESH_STR_TRANSPORT_UNKNOWN));

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
                                mesh_str(MESH_STR_TOAST_LINK_LOST));
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
        slot->in_range = true; /* a port that is enumerated is plugged in */
        slot->paired = true;   /* a cable has nothing to bond */
        slot->bootloader = !mesh_serial_device_is_radio(&serial_devices[i]);
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
        /* Both sources are fixed-size arrays BlueZ filled in, not strings we can prove are
           terminated - a full 64-byte name with no NUL would send `%s` reading on into the next
           device in the array. mesh_str_copy stops at the destination's size either way, which
           is what it was written for. */
        mesh_str_copy(slot->identifier, sizeof slot->identifier, ble_devices[i].address);
        mesh_str_copy(slot->name, sizeof slot->name, ble_devices[i].name);
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
        /* A radio answering us is better evidence of range than any advertisement, and the
           scan is held down for the whole of a link - so the node we are on says so even
           after BlueZ has dropped the RSSI it was last heard at. A connect that is merely in
           flight is not evidence of anything: pressing A on a row for a radio that is at home
           would otherwise turn it "in range" with a 0 dBm reading for the whole of the
           connect timeout, and count it on the Status card while it did. */
        slot->in_range = ble_devices[i].in_range || slot->connected;
        if (slot->connected) {
            connected_address_seen = true;
        }
        ++device_count;
    }

    if (connected_address != NULL && connected_address[0] != '\0' && !connected_address_seen &&
        device_count < MESH_UI_MAX_DEVICES) {
        struct mesh_ui_device *slot = &ui_devices[device_count];
        /*
         * The link that is up but in nobody's list.
         *
         * For BLE and USB this is a race - a connect that beat its own discovery - and the row
         * is replaced by the real one the moment the scan catches up. For a network link it is
         * the steady state: nothing enumerates a host, so this is the only row a TCP link will
         * ever have, and what it says about itself is what the whole client says about itself
         * in the line under the keycaps.
         *
         * Which is why the kind is stated rather than left to the memset. BLE is 0, so an unset
         * `kind` is a Bluetooth radio - and a renderer asks it three separate questions. The
         * network link drew a Bluetooth disc, reported `0dBm` (the reading this file refuses
         * everywhere else, because an absent RSSI read as a number is the *strongest* signal on
         * the screen) and offered Y to forget a bond that was never made.
         */
        const bool over_network = (active == mesh_tcp_transport());

        snprintf(slot->identifier, sizeof slot->identifier, "%s", connected_address);
        slot->kind = over_network ? (uint8_t)MESH_UI_DEVICE_TCP : (uint8_t)MESH_UI_DEVICE_BLE;
        /*
         * A placeholder name is for something that is going to arrive. A radio's advertisement
         * does; a host's does not, so on a network link "Connected" would have stood
         * permanently where the address goes, and the address is the only thing about this row
         * the reader did not already know. Left empty, every label falls back to `identifier`.
         */
        if (!over_network) {
            snprintf(slot->name, sizeof slot->name, "%s",
                     mesh_str(MESH_STR_DEVICES_CONNECTED_NAME));
        }
        slot->rssi = 0;
        /* The scan is held down while a link is up, so the node we are talking to has no
           fresh RSSI - but a radio answering us is the strongest evidence of range there is.
           A host answers from anywhere, so it is evidence of no distance at all. */
        slot->in_range = !over_network;
        slot->connected = true;
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
    /*
     * A network link is deliberately not remembered here.
     *
     * This history is what auto-connect ranks a *scan* with - which of the radios in the list is
     * most recently yours - and a network host is not in any scan: it is found in configuration,
     * which already remembers it. Recorded, it would be filed under the only other kind there is,
     * so an IP address would sit in the BLE history as a preferred device no advertisement can
     * ever match, pushing a real radio out of eight slots to do it.
     */
    if (connected_address != NULL && connected_address[0] != '\0' &&
        active != mesh_tcp_transport()) {
        const uint8_t connected_kind = (active == mesh_serial_transport())
                                           ? (uint8_t)MESH_UI_DEVICE_SERIAL
                                           : (uint8_t)MESH_UI_DEVICE_BLE;
        /* The helper raises ui_preferences_dirty when the file needs rewriting, which the
           flush at the end of this function already acts on. */
        mesh_app_note_connected_device(app, connected_address, connected_kind);
    }

    if (app->publish_cache == NULL) {
        app->publish_cache = calloc(1U, sizeof *app->publish_cache);
    }
    struct mesh_app_publish_cache *cache = app->publish_cache;
    const struct mesh_handshake_status *source_status = mesh_session_handshake(&app->session);
    const struct mesh_message_log *source_messages = mesh_session_messages(&app->session);
    const bool handshake_changed =
        cache == NULL || !cache->valid ||
        memcmp(&cache->handshake, source_status, sizeof *source_status) != 0;
    const bool messages_changed =
        cache == NULL || !cache->valid ||
        memcmp(&cache->messages, source_messages, sizeof *source_messages) != 0;
    const bool roster_changed =
        handshake_changed || messages_changed ||
        cache->roster_owner != mesh_session_roster_owner(&app->session) ||
        cache->link_up != mesh_session_attached(&app->session) ||
        memcmp(&cache->preferences, &app->ui_preferences, sizeof app->ui_preferences) != 0;
    const struct mesh_waypoint_book *source_waypoints = mesh_session_waypoints(&app->session);
    const bool waypoints_changed =
        cache == NULL || !cache->valid ||
        memcmp(&cache->waypoints, source_waypoints, sizeof *source_waypoints) != 0;
    const bool message_view_changed = handshake_changed || messages_changed ||
                                      cache->locale != mesh_i18n_locale() ||
                                      memcmp(&cache->restored_messages, &app->ui_messages_cached,
                                             sizeof app->ui_messages_cached) != 0;
    const struct mesh_handshake_status *status = source_status;
    const bool handshake_active = status->request_in_flight || status->config_complete ||
                                  status->has_my_info || status->has_config ||
                                  (status->node_count > 0U);

    if (roster_changed && handshake_active) {
        struct mesh_ui_handshake_state ui_handshake;
        memset(&ui_handshake, 0, sizeof(ui_handshake));
        ui_handshake.request_in_flight = status->request_in_flight;
        ui_handshake.request_id = status->request_id;
        ui_handshake.config_complete = status->config_complete;
        ui_handshake.config_complete_id = status->config_complete_id;
        ui_handshake.has_my_info = status->has_my_info;
        ui_handshake.has_config = status->has_config;
        ui_handshake.sync_nodes = (uint32_t)mesh_session_synced_nodes(&app->session);
        ui_handshake.link_up = mesh_session_attached(&app->session);
        /* The roster outlives the connection, so a node list on screen is not proof of a live
           sync: what makes it live is something from this connection having arrived. */
        ui_handshake.roster_owner = mesh_session_roster_owner(&app->session);
        /* What each forget row would drop, counted over the whole session roster rather than
           the 128 that fit below - the roster holds twice that - and through the same
           predicate the forget itself uses, so a row's number is what the press removes. The
           Nodes tab's "off radio" total is a different question and is counted from the rows
           it draws. */
        ui_handshake.nodes_forgettable_off_radio =
            mesh_session_forgettable_nodes(&app->session, true);
        ui_handshake.nodes_forgettable_all = mesh_session_forgettable_nodes(&app->session, false);
        ui_handshake.cached = !status->config_complete && !status->has_my_info &&
                              !status->request_in_flight && !status->has_config;
        if (status->has_my_info) {
            const uint32_t my_node = status->my_info.my_node_num;
            /* Remember this radio as one of ours. Pins live in the radio's own NodeDB, so
               without this the node you connect to today is a stranger on the node you
               connect to tomorrow; see mesh_app_node_rank(). */
            if (mesh_ui_preferences_note_radio(&app->ui_preferences, my_node)) {
                preferences_modified = true;
            }
            ui_handshake.my_info.node_num = status->my_info.my_node_num;
            ui_handshake.my_info.nodedb_entries = status->my_info.nodedb_count;
            ui_handshake.my_info.reboot_count = status->my_info.reboot_count;
            for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
                if (status->nodes[i].node_id == my_node && status->nodes[i].short_name[0] != '\0') {
                    snprintf(ui_handshake.my_short_name, sizeof(ui_handshake.my_short_name), "%s",
                             status->nodes[i].short_name);
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
        size_t total = status->node_count > MESH_SESSION_MAX_NODES ? MESH_SESSION_MAX_NODES
                                                                   : status->node_count;
        const uint32_t my_node = status->has_my_info ? status->my_info.my_node_num : 0U;
        for (size_t i = 0; i < total; ++i) {
            const struct mesh_node_summary *node = &status->nodes[i];
            rank[i] = mesh_app_node_rank(node, my_node, message_log, &app->ui_preferences);
            size_t j = i;
            while (j > 0U) {
                const size_t prev_index = order[j - 1U];
                const struct mesh_node_summary *prev = &status->nodes[prev_index];
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
            const struct mesh_node_summary *src = &status->nodes[order[i]];
            struct mesh_ui_node_summary *dst = &ui_handshake.nodes[i];
            dst->node_id = src->node_id;
            snprintf(dst->long_name, sizeof(dst->long_name), "%s", src->long_name);
            snprintf(dst->short_name, sizeof(dst->short_name), "%s", src->short_name);
            dst->last_heard = src->last_heard;
            dst->snr = src->snr;
            dst->has_rssi = src->has_rssi;
            dst->rx_rssi = src->rx_rssi;
            dst->rssi_time = src->rssi_time;
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
        /* `total`, not copy_count: what the roster knows, against what survived the ranking. */
        ui_handshake.nodes_known = (uint32_t)total;

        /*
         * The map's roster, from the whole of `total` rather than from the 128 above.
         *
         * The ranking cut is a decision about a *list*: which nodes are worth a row on a screen
         * a reader scrolls. A map has no rows, and a node's rank has nothing to do with whether
         * its marker is on the panel - so a mesh whose 200th-ranked node is the one parked at
         * the far end of the valley was drawing everything except the marker that answered the
         * question. Same order, because the order is the drawing order and ties are settled by
         * it; no cut, because struct mesh_ui_map_node is small enough not to need one.
         *
         * Positioned nodes only, and the bounds test rather than `valid` alone: the session
         * already refuses an out-of-range fix, and asking again here costs nothing and keeps a
         * hand-built roster from putting a marker off the edge of Earth.
         */
        size_t map_count = 0U;
        for (size_t i = 0; i < total && map_count < MESH_UI_MAX_MAP_NODES; ++i) {
            const struct mesh_node_summary *src = &status->nodes[order[i]];
            if (!src->position.valid ||
                !mesh_geo_coords_valid(src->position.latitude_i, src->position.longitude_i)) {
                continue;
            }
            struct mesh_ui_map_node *dst = &ui_handshake.map_nodes[map_count++];
            dst->node_id = src->node_id;
            dst->latitude_i = src->position.latitude_i;
            dst->longitude_i = src->position.longitude_i;
            dst->received = src->position.received;
            dst->precision_bits = src->position.precision_bits;
            dst->in_nodedb = src->in_nodedb;
            /* Both arrays are cut from `order`, so a row exists exactly when this node's place
               in the ranking is inside the cut - no search, and no second answer to disagree
               with the one mesh_ui_node_detail_find() gives. */
            dst->has_row = (i < copy_count);
            /* The short name, falling back to the long one - the rule is stated on the field.
               mesh_str_copy rather than snprintf because the long name is longer than a label
               and cutting it is the expected case, not an overflow to be warned about. */
            mesh_str_copy(dst->label, sizeof(dst->label),
                          src->short_name[0] != '\0' ? src->short_name : src->long_name);
        }
        ui_handshake.map_node_count = (uint32_t)map_count;

        size_t channel_count = status->channel_count;
        if (channel_count > MESH_UI_MAX_CHANNELS) {
            channel_count = MESH_UI_MAX_CHANNELS;
        }
        for (size_t i = 0; i < channel_count; ++i) {
            ui_handshake.channels[i].index = status->channels[i].index;
            ui_handshake.channels[i].role = status->channels[i].role;
            ui_handshake.channels[i].psk_len = status->channels[i].psk_len;
            ui_handshake.channels[i].uplink_enabled = status->channels[i].uplink_enabled;
            ui_handshake.channels[i].downlink_enabled = status->channels[i].downlink_enabled;
            ui_handshake.channels[i].position_precision = status->channels[i].position_precision;
            snprintf(ui_handshake.channels[i].name, sizeof(ui_handshake.channels[i].name), "%s",
                     status->channels[i].name);
            if (status->channels[i].role == 1U /* PRIMARY */) {
                snprintf(ui_handshake.primary_channel, sizeof(ui_handshake.primary_channel), "%s",
                         status->channels[i].name);
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
    } else if (roster_changed) {
        mesh_ui_update_flags prev_flags = app->ui_store.pending_flags;
        mesh_ui_store_set_handshake(&app->ui_store, NULL);
        if (app->ui_handshake_cache_path[0] != '\0' &&
            (app->ui_store.pending_flags & MESH_UI_UPDATE_HANDSHAKE) != 0U &&
            (prev_flags & MESH_UI_UPDATE_HANDSHAKE) == 0U) {
            app->ui_handshake_cache_dirty = true;
        }
    }

    if (message_view_changed) {
        mesh_app_publish_messages(app, status);
    }
    /* The names on a waypoint row come out of the roster, so a NodeInfo arriving changes what
       this publishes even when the book itself has not moved - which is why the handshake is
       part of the test and not just the book. */
    if (waypoints_changed || handshake_changed) {
        mesh_app_publish_waypoints(app, status);
    }

    const struct mesh_radio_settings *radio_settings = mesh_session_settings(&app->session);
    struct mesh_ui_settings ui_settings;
    if (cache == NULL || !cache->valid ||
        memcmp(&cache->settings, radio_settings, sizeof *radio_settings) != 0) {
        mesh_app_flatten_settings(radio_settings, &ui_settings);
        if (cache != NULL) {
            cache->settings = *radio_settings;
            cache->flat_settings = ui_settings;
        }
    } else {
        ui_settings = cache->flat_settings;
    }
    /* flatten_settings() zeroes the struct, so the client's own facts go in after it. */
    mesh_app_flatten_client_info(app, &ui_settings.client);
    mesh_app_flatten_firmware(app, &ui_settings);
    /* Where the radio says it is, which is not part of PositionConfig: it comes from our own
       node's record, and it is what the Position section's coordinate rows start from. */
    if (status->has_my_info) {
        const struct mesh_node_summary *self = NULL;
        for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
            if (status->nodes[i].node_id == status->my_info.my_node_num) {
                self = &status->nodes[i];
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
    mesh_app_flatten_radio_notice(mesh_session_notification(&app->session), &ui_settings.notice);
    mesh_app_flatten_queue_status(mesh_session_queue_status(&app->session), &ui_settings.queue);
    mesh_app_flatten_store_forward(status, mesh_session_store_forward(&app->session),
                                   &ui_settings.store_forward);
    ui_settings.reboot_notices = app->session.reboot_notices;
    mesh_ui_store_set_settings(&app->ui_store, &ui_settings);
    mesh_app_track_settings_save(app, radio_settings, link_connected);

    struct mesh_ui_traceroute ui_traceroute;
    mesh_app_flatten_traceroute(status, mesh_session_traceroute(&app->session),
                                status->has_my_info ? status->my_info.my_node_num : 0U,
                                &ui_traceroute);
    mesh_ui_store_set_traceroute(&app->ui_store, &ui_traceroute);

    if (cache != NULL) {
        if (handshake_changed) {
            cache->handshake = *source_status;
        }
        if (messages_changed) {
            cache->messages = *source_messages;
        }
        if (waypoints_changed) {
            cache->waypoints = *source_waypoints;
        }
        if (message_view_changed) {
            cache->restored_messages = app->ui_messages_cached;
        }
        cache->preferences = app->ui_preferences;
        cache->roster_owner = mesh_session_roster_owner(&app->session);
        cache->link_up = mesh_session_attached(&app->session);
        cache->locale = mesh_i18n_locale();
        cache->valid = true;
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

    /* Marking a conversation read touches nothing else, so watch the stamp for it. */
    if (app->ui_store.read_state.stamp != app->ui_read_state_stamp) {
        app->ui_read_state_stamp = app->ui_store.read_state.stamp;
        if (app->ui_handshake_cache_path[0] != '\0') {
            app->ui_handshake_cache_dirty = true;
        }
    }

    mesh_app_schedule_ui_cache(app);
}
