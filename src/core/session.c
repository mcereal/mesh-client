#define _POSIX_C_SOURCE 200809L

#include "mesh/session.h"

#include "mesh/log.h"
#include "mesh/text.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/channel.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"
#include "meshtastic/telemetry.pb.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t mesh_session_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void mesh_session_reset_handshake(struct mesh_session *session) {
    memset(&session->handshake, 0, sizeof session->handshake);
    memset(&session->stats, 0, sizeof session->stats);
    memset(&session->traceroute, 0, sizeof session->traceroute);
    session->node_cache_warned = false;
    mesh_radio_settings_reset(&session->settings);
    session->admin_probe_queued = false;
}

void mesh_session_init(struct mesh_session *session) {
    if (session == NULL) {
        return;
    }
    memset(session, 0, sizeof *session);
    mesh_message_log_reset(&session->messages);
    /*
     * want_config_id is a nonce: the node echoes it back in config_complete_id. A per-process seed
     * keeps a stale completion left in the node's FIFO by a previous session from ending ours
     * early.
     */
    session->next_config_request_id = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16);
    if (session->next_config_request_id == 0U) {
        session->next_config_request_id = 1U;
    }
    session->next_packet_id = 0U; /* seeded lazily on the first send */
    mesh_session_reset_handshake(session);
}

void mesh_session_attach(struct mesh_session *session, mesh_session_send_fn send, void *ctx) {
    if (session == NULL) {
        return;
    }
    session->send = send;
    session->send_ctx = ctx;
}

void mesh_session_detach(struct mesh_session *session) {
    if (session == NULL) {
        return;
    }
    session->send = NULL;
    session->send_ctx = NULL;
    mesh_session_reset_handshake(session);
}

bool mesh_session_attached(const struct mesh_session *session) {
    return session != NULL && session->send != NULL;
}

static int mesh_session_send_raw(struct mesh_session *session, const uint8_t *packet, size_t len,
                                 uint32_t packet_id) {
    if (session == NULL || packet == NULL || len == 0U) {
        return -EINVAL;
    }
    if (session->send == NULL) {
        return -ENOTCONN;
    }
    if (len > MESH_SESSION_MAX_PACKET) {
        mesh_log_warn("session", "ToRadio packet of %zu bytes exceeds %u byte limit", len,
                      (unsigned)MESH_SESSION_MAX_PACKET);
        return -EMSGSIZE;
    }
    return session->send(session->send_ctx, packet, len, packet_id);
}

int mesh_session_begin_handshake(struct mesh_session *session) {
    if (session == NULL) {
        return -EINVAL;
    }
    if (session->send == NULL) {
        return -ENOTCONN;
    }

    uint32_t request_id = session->next_config_request_id++;
    if (session->next_config_request_id == 0U) {
        session->next_config_request_id = 1U;
    }
    if (request_id == 0U) {
        request_id = session->next_config_request_id++;
        if (session->next_config_request_id == 0U) {
            session->next_config_request_id = 1U;
        }
    }

    mesh_session_reset_handshake(session);
    session->handshake.request_in_flight = true;
    session->handshake.request_id = request_id;

    meshtastic_ToRadio request = meshtastic_ToRadio_init_default;
    request.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    request.want_config_id = request_id;

    uint8_t payload[64];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &request)) {
        mesh_log_error("session", "Failed to encode want_config: %s", PB_GET_ERROR(&stream));
        session->handshake.request_in_flight = false;
        return -EIO;
    }

    int result = mesh_session_send_raw(session, payload, stream.bytes_written, 0U);
    if (result < 0) {
        mesh_log_error("session", "Failed to send want_config request: %d", result);
        session->handshake.request_in_flight = false;
        return result;
    }

    mesh_log_info("session", "Requested config sync (request_id=%u)", request_id);
    return 0;
}

static bool mesh_session_node_known(const struct mesh_session *session, uint32_t node_id) {
    const struct mesh_handshake_status *handshake = &session->handshake;
    for (size_t i = 0; i < handshake->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        if (handshake->nodes[i].node_id == node_id) {
            return true;
        }
    }
    return false;
}

/* The node's entry in the cache, adding it if it is new. NULL only when the cache is full. */
static struct mesh_node_summary *mesh_session_node_slot(struct mesh_session *session,
                                                        uint32_t node_id) {
    struct mesh_handshake_status *handshake = &session->handshake;
    if (handshake->node_count > MESH_SESSION_MAX_NODES) {
        handshake->node_count = MESH_SESSION_MAX_NODES;
    }

    for (size_t i = 0; i < handshake->node_count; ++i) {
        if (handshake->nodes[i].node_id == node_id) {
            return &handshake->nodes[i];
        }
    }

    if (handshake->node_count >= MESH_SESSION_MAX_NODES) {
        if (!session->node_cache_warned) {
            mesh_log_warn("session", "Node cache full (%u); further nodes dropped for this sync",
                          (unsigned)MESH_SESSION_MAX_NODES);
            session->node_cache_warned = true;
        }
        return NULL;
    }

    struct mesh_node_summary *summary = &handshake->nodes[handshake->node_count++];
    memset(summary, 0, sizeof *summary);
    summary->node_id = node_id;
    return summary;
}

/*
 * The identity half of a node record. Reached from two directions: NodeInfo during the NodeDB
 * sync, and a NODEINFO_APP packet when a node introduces itself over the air afterwards - the
 * firmware only replays the database once per connection, so without the packet path a node
 * that joins mid-session stays a bare id forever.
 */
static void mesh_session_apply_user(struct mesh_node_summary *summary,
                                    const meshtastic_User *user) {
    /* Names are chosen by whoever owns that node, so they are untrusted radio input just like
       message bodies: sanitise them here rather than at each of the several places that draw
       or serialise them. A plain snprintf would also happily cut a multi-byte character in
       half at the field boundary, which matters because Meshtastic short names are routinely a
       single four-byte emoji. */
    mesh_text_sanitise_str(user->long_name, summary->long_name, sizeof summary->long_name);
    mesh_text_sanitise_str(user->short_name, summary->short_name, sizeof summary->short_name);
    mesh_text_sanitise_str(user->id, summary->user_id, sizeof summary->user_id);
    summary->hw_model = (uint32_t)user->hw_model;
    summary->role = (uint32_t)user->role;
    summary->is_licensed = user->is_licensed;
    summary->is_unmessagable = user->has_is_unmessagable && user->is_unmessagable;

    summary->public_key_len = 0U;
    if (user->public_key.size > 0U && user->public_key.size <= sizeof summary->public_key) {
        memcpy(summary->public_key, user->public_key.bytes, user->public_key.size);
        summary->public_key_len = (uint8_t)user->public_key.size;
    }
}

static void mesh_session_apply_position(struct mesh_node_summary *summary,
                                        const meshtastic_Position *position) {
    /* A Position with neither coordinate is a time-only or precision-only broadcast; keeping
       the last real fix beats replacing it with 0,0 in the Gulf of Guinea. */
    if (!position->has_latitude_i || !position->has_longitude_i) {
        return;
    }
    summary->position.valid = true;
    summary->position.latitude_i = position->latitude_i;
    summary->position.longitude_i = position->longitude_i;
    summary->position.has_altitude = position->has_altitude;
    summary->position.altitude = position->altitude;
    summary->position.time = position->time;
    summary->position.sats_in_view =
        (uint8_t)(position->sats_in_view > 255U ? 255U : position->sats_in_view);
    summary->position.precision_bits =
        (uint8_t)(position->precision_bits > 255U ? 255U : position->precision_bits);
}

static void mesh_session_apply_device_metrics(struct mesh_node_summary *summary,
                                              const meshtastic_DeviceMetrics *metrics,
                                              uint32_t heard) {
    summary->metrics.valid = true;
    summary->metrics.time = heard;
    summary->metrics.has_battery = metrics->has_battery_level;
    summary->metrics.battery_level =
        (uint8_t)(metrics->battery_level > 255U ? 255U : metrics->battery_level);
    summary->metrics.has_voltage = metrics->has_voltage;
    summary->metrics.voltage = metrics->voltage;
    summary->metrics.has_channel_utilization = metrics->has_channel_utilization;
    summary->metrics.channel_utilization = metrics->channel_utilization;
    summary->metrics.has_air_util_tx = metrics->has_air_util_tx;
    summary->metrics.air_util_tx = metrics->air_util_tx;
    summary->metrics.has_uptime = metrics->has_uptime_seconds;
    summary->metrics.uptime_seconds = metrics->uptime_seconds;
}

static void mesh_session_apply_environment(struct mesh_node_summary *summary,
                                           const meshtastic_EnvironmentMetrics *env,
                                           uint32_t heard) {
    summary->environment.valid = true;
    summary->environment.time = heard;
    summary->environment.has_temperature = env->has_temperature;
    summary->environment.temperature = env->temperature;
    summary->environment.has_humidity = env->has_relative_humidity;
    summary->environment.relative_humidity = env->relative_humidity;
    summary->environment.has_pressure = env->has_barometric_pressure;
    summary->environment.barometric_pressure = env->barometric_pressure;
    summary->environment.has_iaq = env->has_iaq;
    summary->environment.iaq = (uint16_t)env->iaq;
    summary->environment.has_lux = env->has_lux;
    summary->environment.lux = env->lux;
    summary->environment.has_voltage = env->has_voltage;
    summary->environment.voltage = env->voltage;
    summary->environment.has_current = env->has_current;
    summary->environment.current = env->current;
}

/*
 * LocalStats is the radio describing itself, so it lands on the session rather than on a node
 * record. Two fields get a flag rather than being trusted at face value: heap_total_bytes of
 * zero means the firmware did not fill it in (no radio has no heap), and a noise floor of
 * exactly 0 dBm is not a reading any LoRa front end produces.
 */
static void mesh_session_apply_local_stats(struct mesh_session *session,
                                           const meshtastic_LocalStats *stats, uint32_t stamp) {
    struct mesh_radio_stats *out = &session->stats;
    out->valid = true;
    out->time = stamp;
    out->uptime_seconds = stats->uptime_seconds;
    out->channel_utilization = stats->channel_utilization;
    out->air_util_tx = stats->air_util_tx;
    out->num_packets_tx = stats->num_packets_tx;
    out->num_packets_rx = stats->num_packets_rx;
    out->num_packets_rx_bad = stats->num_packets_rx_bad;
    out->num_rx_dupe = stats->num_rx_dupe;
    out->num_tx_relay = stats->num_tx_relay;
    out->num_tx_relay_canceled = stats->num_tx_relay_canceled;
    out->num_tx_dropped = stats->num_tx_dropped;
    out->num_online_nodes = stats->num_online_nodes;
    out->num_total_nodes = stats->num_total_nodes;
    out->has_heap = stats->heap_total_bytes > 0U;
    out->heap_total_bytes = stats->heap_total_bytes;
    out->heap_free_bytes = stats->heap_free_bytes;
    out->has_noise_floor = stats->noise_floor != 0;
    out->noise_floor = stats->noise_floor;
}

static void mesh_session_store_node_summary(struct mesh_session *session,
                                            const meshtastic_NodeInfo *info) {
    struct mesh_node_summary *summary = mesh_session_node_slot(session, info->num);
    if (summary == NULL) {
        return;
    }

    /*
     * A resync replaces what the radio knows, but the radio's NodeDB does not carry environment
     * telemetry, and it may send a NodeInfo with no position or metrics for a node we have
     * already heard both from. Overwriting only what this NodeInfo actually carries keeps the
     * detail screen from emptying itself every time the database is replayed.
     */
    summary->last_heard =
        info->last_heard > summary->last_heard ? info->last_heard : summary->last_heard;
    summary->snr = info->snr;
    summary->via_mqtt = info->via_mqtt;
    summary->has_hops_away = info->has_hops_away;
    summary->hops_away = info->hops_away;
    summary->is_favorite = info->is_favorite;
    summary->is_ignored = info->is_ignored;
    summary->is_muted = info->is_muted;
    summary->channel = (uint8_t)info->channel;

    if (info->has_user) {
        mesh_session_apply_user(summary, &info->user);
    }
    if (info->has_position) {
        mesh_session_apply_position(summary, &info->position);
    }
    if (info->has_device_metrics) {
        mesh_session_apply_device_metrics(summary, &info->device_metrics, info->last_heard);
    }

    mesh_log_debug("session", "Cached node %u (%s) last_heard=%u%s", summary->node_id,
                   summary->short_name[0] != '\0' ? summary->short_name : summary->long_name,
                   summary->last_heard, summary->via_mqtt ? " via_mqtt" : "");
}

/*
 * Every packet a node sends us is proof it is alive now. The NodeDB sync only tells us what the
 * radio knew at connect time, and a mesh of 130 nodes re-sorts constantly, so without this the
 * node you are actually talking to sinks down (or off) the UI's list while it is chatting with
 * you. A node the sync never delivered (cache full, or joined later) is added with just its id;
 * the name follows when the radio sends its NodeInfo.
 */
static void mesh_session_touch_node_from_packet(struct mesh_session *session,
                                                const meshtastic_MeshPacket *packet) {
    struct mesh_handshake_status *handshake = &session->handshake;
    if (packet->from == 0U || packet->from == MESH_MESSAGE_BROADCAST_ADDR ||
        (handshake->has_my_info && packet->from == handshake->my_info.my_node_num)) {
        return;
    }

    uint32_t heard = packet->has_rx_time ? packet->rx_time : 0U;
    if (heard == 0U) {
        /* No radio timestamp: use ours if it looks like a real clock (not 1970). */
        const time_t now = time(NULL);
        if (now > 1600000000) {
            heard = (uint32_t)now;
        }
    }

    const bool known = mesh_session_node_known(session, packet->from);
    struct mesh_node_summary *summary = mesh_session_node_slot(session, packet->from);
    if (summary == NULL) {
        return;
    }
    if (!known) {
        mesh_log_info("session", "Node 0x%08x heard before its NodeInfo; added to the cache",
                      packet->from);
    }

    if (heard > summary->last_heard) {
        summary->last_heard = heard;
    }
    if (packet->rx_snr != 0.0f) {
        summary->snr = packet->rx_snr;
    }
    if (packet->hop_start != 0U && packet->hop_start >= packet->hop_limit) {
        summary->has_hops_away = true;
        summary->hops_away = (uint8_t)(packet->hop_start - packet->hop_limit);
    }
    summary->via_mqtt = packet->via_mqtt;
}

/*
 * The other half of a node's record, off the air rather than out of the NodeDB. The firmware
 * replays its database exactly once per connection, so everything that happens afterwards - a
 * node joining, a battery draining, a tracker moving - only reaches us as one of these three
 * app payloads. Without this the detail screen would show whatever was true at connect time
 * and then quietly rot for the rest of the session.
 *
 * Unlike the last_heard touch above this runs for our own node too: our node broadcasts its
 * own position and telemetry like any other, and it is the one node whose battery the user can
 * do something about.
 */
static void mesh_session_apply_packet_details(struct mesh_session *session,
                                              const meshtastic_MeshPacket *packet) {
    if (packet->from == 0U || packet->from == MESH_MESSAGE_BROADCAST_ADDR ||
        packet->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        return;
    }

    const meshtastic_Data *data = &packet->decoded;
    if (data->portnum != meshtastic_PortNum_NODEINFO_APP &&
        data->portnum != meshtastic_PortNum_POSITION_APP &&
        data->portnum != meshtastic_PortNum_TELEMETRY_APP) {
        return;
    }

    uint32_t heard = packet->has_rx_time ? packet->rx_time : 0U;
    if (heard == 0U) {
        const time_t now = time(NULL);
        if (now > 1600000000) {
            heard = (uint32_t)now;
        }
    }

    /* LocalStats below is about the radio rather than about a node, so a full node cache
       must not cost us the one telemetry that has nowhere else to go. */
    struct mesh_node_summary *summary = mesh_session_node_slot(session, packet->from);

    pb_istream_t stream = pb_istream_from_buffer(data->payload.bytes, data->payload.size);
    switch (data->portnum) {
    case meshtastic_PortNum_NODEINFO_APP: {
        meshtastic_User user = meshtastic_User_init_default;
        if (!pb_decode(&stream, meshtastic_User_fields, &user)) {
            mesh_log_debug("session", "Bad NODEINFO_APP from 0x%08x: %s", packet->from,
                           PB_GET_ERROR(&stream));
            return;
        }
        if (summary == NULL) {
            return;
        }
        mesh_session_apply_user(summary, &user);
        mesh_log_debug("session", "Node 0x%08x introduced itself as %s", packet->from,
                       summary->short_name[0] != '\0' ? summary->short_name : summary->long_name);
        break;
    }
    case meshtastic_PortNum_POSITION_APP: {
        meshtastic_Position position = meshtastic_Position_init_default;
        if (!pb_decode(&stream, meshtastic_Position_fields, &position)) {
            mesh_log_debug("session", "Bad POSITION_APP from 0x%08x: %s", packet->from,
                           PB_GET_ERROR(&stream));
            return;
        }
        if (summary == NULL) {
            return;
        }
        mesh_session_apply_position(summary, &position);
        break;
    }
    case meshtastic_PortNum_TELEMETRY_APP: {
        meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_default;
        if (!pb_decode(&stream, meshtastic_Telemetry_fields, &telemetry)) {
            mesh_log_debug("session", "Bad TELEMETRY_APP from 0x%08x: %s", packet->from,
                           PB_GET_ERROR(&stream));
            return;
        }
        const uint32_t stamp = telemetry.time != 0U ? telemetry.time : heard;
        /* LocalStats never crosses the mesh: the firmware sends it to the attached client
           only, from its own node number. Anything else claiming to be ours is not. */
        if (telemetry.which_variant == meshtastic_Telemetry_local_stats_tag) {
            if (session->handshake.has_my_info &&
                packet->from == session->handshake.my_info.my_node_num) {
                mesh_session_apply_local_stats(session, &telemetry.variant.local_stats, heard);
            }
            break;
        }
        if (summary == NULL) {
            return;
        }
        if (telemetry.which_variant == meshtastic_Telemetry_device_metrics_tag) {
            mesh_session_apply_device_metrics(summary, &telemetry.variant.device_metrics, stamp);
        } else if (telemetry.which_variant == meshtastic_Telemetry_environment_metrics_tag) {
            mesh_session_apply_environment(summary, &telemetry.variant.environment_metrics, stamp);
        }
        break;
    }
    default:
        break;
    }
}

/*
 * A TRACEROUTE_APP packet answering the request we sent. The firmware replies from the target
 * with `Data.request_id` set to our packet id, which is what tells our trace from somebody
 * else's crossing the same radio - a node relaying a trace between two other nodes sees the
 * same portnum.
 *
 * Returns true when the packet was ours, so the caller can keep it out of everything else.
 */
static bool mesh_session_handle_traceroute(struct mesh_session *session,
                                           const meshtastic_MeshPacket *packet) {
    const meshtastic_Data *data = &packet->decoded;
    if (data->portnum != meshtastic_PortNum_TRACEROUTE_APP) {
        return false;
    }
    struct mesh_traceroute *trace = &session->traceroute;
    if (trace->state != MESH_TRACEROUTE_PENDING || data->request_id == 0U ||
        data->request_id != trace->packet_id) {
        /* Not an answer to ours: a trace passing through, or one we have already given up on.
           Claimed anyway - a RouteDiscovery is not a message and has no business in the log. */
        mesh_log_debug("session", "Ignoring TRACEROUTE_APP from 0x%08x (request %u)", packet->from,
                       data->request_id);
        return true;
    }

    meshtastic_RouteDiscovery route = meshtastic_RouteDiscovery_init_default;
    pb_istream_t stream = pb_istream_from_buffer(data->payload.bytes, data->payload.size);
    if (!pb_decode(&stream, meshtastic_RouteDiscovery_fields, &route)) {
        mesh_log_warn("session", "Bad TRACEROUTE_APP reply: %s", PB_GET_ERROR(&stream));
        trace->state = MESH_TRACEROUTE_TIMEOUT;
        return true;
    }

    trace->route_count =
        (uint8_t)(route.route_count > MESH_TRACEROUTE_MAX_HOPS ? MESH_TRACEROUTE_MAX_HOPS
                                                               : route.route_count);
    for (uint8_t i = 0; i < trace->route_count; ++i) {
        trace->route[i] = route.route[i];
    }
    trace->snr_count = (uint8_t)(route.snr_towards_count > MESH_TRACEROUTE_MAX_HOPS + 1U
                                     ? MESH_TRACEROUTE_MAX_HOPS + 1U
                                     : route.snr_towards_count);
    for (uint8_t i = 0; i < trace->snr_count; ++i) {
        trace->snr[i] = route.snr_towards[i];
    }
    trace->back_count =
        (uint8_t)(route.route_back_count > MESH_TRACEROUTE_MAX_HOPS ? MESH_TRACEROUTE_MAX_HOPS
                                                                    : route.route_back_count);
    for (uint8_t i = 0; i < trace->back_count; ++i) {
        trace->route_back[i] = route.route_back[i];
    }
    trace->snr_back_count = (uint8_t)(route.snr_back_count > MESH_TRACEROUTE_MAX_HOPS + 1U
                                          ? MESH_TRACEROUTE_MAX_HOPS + 1U
                                          : route.snr_back_count);
    for (uint8_t i = 0; i < trace->snr_back_count; ++i) {
        trace->snr_back[i] = route.snr_back[i];
    }

    const time_t now = time(NULL);
    trace->completed = now > 1600000000 ? (uint32_t)now : 0U;
    trace->state = MESH_TRACEROUTE_DONE;
    mesh_log_info("session", "Traceroute to 0x%08x: %u hops out, %u back", trace->target,
                  (unsigned)trace->route_count, (unsigned)trace->back_count);
    return true;
}

static void mesh_session_handle_log_record(const meshtastic_LogRecord *record) {
    char message[sizeof(record->message) + 1U];
    memcpy(message, record->message, sizeof record->message);
    message[sizeof record->message] = '\0';

    const char *component = "radio.log";
    switch (record->level) {
    case meshtastic_LogRecord_Level_CRITICAL:
    case meshtastic_LogRecord_Level_ERROR:
        mesh_log_error(component, "%s", message);
        break;
    case meshtastic_LogRecord_Level_WARNING:
        mesh_log_warn(component, "%s", message);
        break;
    case meshtastic_LogRecord_Level_INFO:
        mesh_log_info(component, "%s", message);
        break;
    case meshtastic_LogRecord_Level_DEBUG:
        mesh_log_debug(component, "%s", message);
        break;
    case meshtastic_LogRecord_Level_TRACE:
    case meshtastic_LogRecord_Level_UNSET:
    default:
        mesh_log_trace(component, "%s", message);
        break;
    }
}

static void mesh_session_handle_channel(struct mesh_session *session,
                                        const meshtastic_Channel *channel) {
    if (channel->index < 0 || (size_t)channel->index >= MESH_SESSION_MAX_CHANNELS) {
        mesh_log_debug("session", "Ignoring channel with index %d", (int)channel->index);
        return;
    }
    mesh_radio_settings_apply_channel(&session->settings, channel);
    struct mesh_channel_summary *slot = &session->handshake.channels[channel->index];
    memset(slot, 0, sizeof *slot);
    slot->index = (uint8_t)channel->index;
    slot->role = (uint8_t)channel->role;
    if (channel->has_settings) {
        snprintf(slot->name, sizeof slot->name, "%s", channel->settings.name);
        slot->psk_len = (uint8_t)channel->settings.psk.size;
        slot->uplink_enabled = channel->settings.uplink_enabled;
        slot->downlink_enabled = channel->settings.downlink_enabled;
        if (channel->settings.has_module_settings) {
            slot->position_precision = channel->settings.module_settings.position_precision;
        }
    }
    if ((size_t)channel->index + 1U > session->handshake.channel_count) {
        session->handshake.channel_count = (size_t)channel->index + 1U;
    }
    if (channel->role != meshtastic_Channel_Role_DISABLED) {
        mesh_log_info("session", "Channel %d: %s (%s)", (int)channel->index,
                      slot->name[0] != '\0' ? slot->name : "<default>",
                      channel->role == meshtastic_Channel_Role_PRIMARY ? "primary" : "secondary");
    }
}

void mesh_session_handle_from_radio(struct mesh_session *session, const uint8_t *payload,
                                    size_t len) {
    if (session == NULL || payload == NULL || len == 0U) {
        return;
    }

    meshtastic_FromRadio message = meshtastic_FromRadio_init_default;
    pb_istream_t stream = pb_istream_from_buffer(payload, len);
    if (!pb_decode(&stream, meshtastic_FromRadio_fields, &message)) {
        mesh_log_warn("session", "Failed to decode FromRadio: %s", PB_GET_ERROR(&stream));
        return;
    }

    struct mesh_handshake_status *handshake = &session->handshake;
    switch (message.which_payload_variant) {
    case meshtastic_FromRadio_my_info_tag:
        handshake->has_my_info = true;
        handshake->my_info = message.my_info;
        mesh_log_info("session", "MyNodeInfo: node=%u, node_count=%u", message.my_info.my_node_num,
                      message.my_info.nodedb_count);
        break;
    case meshtastic_FromRadio_node_info_tag:
        mesh_session_store_node_summary(session, &message.node_info);
        /* Our own NodeInfo carries the owner record the User settings section shows. */
        if (message.node_info.has_user && handshake->has_my_info &&
            message.node_info.num == handshake->my_info.my_node_num) {
            mesh_radio_settings_apply_owner(&session->settings, &message.node_info.user);
        }
        break;
    case meshtastic_FromRadio_channel_tag:
        mesh_session_handle_channel(session, &message.channel);
        break;
    case meshtastic_FromRadio_config_tag:
        handshake->has_config = true;
        handshake->config = message.config;
        mesh_radio_settings_apply_config(&session->settings, &message.config);
        mesh_log_debug("session", "Received config fragment (variant %u)",
                       (unsigned)message.config.which_payload_variant);
        break;
    case meshtastic_FromRadio_moduleConfig_tag:
        mesh_radio_settings_apply_module_config(&session->settings, &message.moduleConfig);
        mesh_log_debug("session", "Received module config fragment (variant %u)",
                       (unsigned)message.moduleConfig.which_payload_variant);
        break;
    case meshtastic_FromRadio_metadata_tag:
        mesh_radio_settings_apply_metadata(&session->settings, &message.metadata);
        mesh_log_info("session", "Device metadata: firmware %s, hw_model %u",
                      message.metadata.firmware_version, (unsigned)message.metadata.hw_model);
        break;
    case meshtastic_FromRadio_config_complete_id_tag:
        handshake->config_complete_id = message.config_complete_id;
        if (handshake->request_in_flight && message.config_complete_id == handshake->request_id) {
            handshake->request_in_flight = false;
            handshake->config_complete = true;
            mesh_log_info("session", "Config sync complete for request %u",
                          message.config_complete_id);
        } else {
            mesh_log_debug("session", "Received config_complete_id=%u (pending=%s request=%u)",
                           message.config_complete_id, handshake->request_in_flight ? "yes" : "no",
                           handshake->request_id);
        }
        break;
    case meshtastic_FromRadio_packet_tag:
        /* Admin replies come from ourselves; they are not traffic and never a message. */
        if (mesh_radio_settings_ingest(&session->settings, &message.packet) == 1) {
            break;
        }
        /* A RouteDiscovery is not a message either, and the node it came from is already
           being touched below - so claim it after the touch, not before. */
        if (message.packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
            message.packet.decoded.portnum == meshtastic_PortNum_TRACEROUTE_APP) {
            mesh_session_touch_node_from_packet(session, &message.packet);
            (void)mesh_session_handle_traceroute(session, &message.packet);
            break;
        }
        mesh_session_touch_node_from_packet(session, &message.packet);
        mesh_session_apply_packet_details(session, &message.packet);
        mesh_message_ingest(&session->messages, &message.packet,
                            handshake->has_my_info ? handshake->my_info.my_node_num : 0U);
        break;
    case meshtastic_FromRadio_log_record_tag:
        mesh_session_handle_log_record(&message.log_record);
        break;
    default:
        mesh_log_debug("session", "Ignoring FromRadio payload tag %" PRIu32,
                       (uint32_t)message.which_payload_variant);
        break;
    }
}

/*
 * Pushes our wall clock at the radio once per connection. A node with no GPS and no phone ever
 * attached sits at 00:00 forever, which also means every packet it hands us has rx_time 0 and
 * the UI can say nothing about when anything arrived. `set_time_only` is the firmware's
 * convenience for exactly this, and it is what the phone clients do on connect.
 *
 * Silent either way: a radio that already has a better clock (GPS, another node) is free to
 * ignore us, and a Brick whose own clock is not credible pushes nothing at all.
 */
static void mesh_session_sync_clock(struct mesh_session *session) {
    const time_t now = time(NULL);
    if (now <= 0 || (uint64_t)now > UINT32_MAX) {
        return;
    }
    const uint32_t epoch = (uint32_t)now;
    if (epoch < MESH_RADIO_CLOCK_MIN_EPOCH) {
        mesh_log_info("session", "Not setting the radio clock: our own clock reads %u", epoch);
        return;
    }
    const int queued = mesh_radio_settings_queue_time(&session->settings, epoch);
    if (queued > 0) {
        mesh_log_info("session", "Setting the radio clock to %u (%d requests)", epoch, queued);
    } else if (queued < 0) {
        mesh_log_warn("session", "Could not queue the radio clock: %d", queued);
    }
}

/*
 * Once the handshake has completed, ask for the metadata and the owner (proof that the
 * AdminMessage round trip and its session passkey work on this radio), push our clock at the
 * radio, then send whatever else is queued, one request at a time.
 */
void mesh_session_tick(struct mesh_session *session, uint64_t now_ms) {
    if (session == NULL) {
        return;
    }
    /* Nothing on the mesh reports a traceroute that was dropped on the way out or on the way
       back, so the clock is the only thing that can end a lost one. Checked before the link
       guards below: a trace outlives a momentary stall, and a row reading "tracing" forever
       is worse than one that says it gave up. */
    struct mesh_traceroute *trace = &session->traceroute;
    if (trace->state == MESH_TRACEROUTE_PENDING && now_ms > trace->sent_ms &&
        now_ms - trace->sent_ms > MESH_TRACEROUTE_TIMEOUT_MS) {
        trace->state = MESH_TRACEROUTE_TIMEOUT;
        mesh_log_info("session", "Traceroute to 0x%08x timed out", trace->target);
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return;
    }
    if (session->handshake.config_complete && !session->admin_probe_queued) {
        session->admin_probe_queued = true;
        mesh_radio_settings_queue_probe(&session->settings);
        mesh_session_sync_clock(session);
    }

    struct mesh_admin_request request;
    if (!mesh_radio_settings_next_request(&session->settings, now_ms, &request)) {
        return;
    }
    request.my_node = session->handshake.my_info.my_node_num;
    request.packet_id = mesh_session_next_packet_id(session);

    uint8_t payload[MESH_SESSION_MAX_PACKET];
    size_t written = 0U;
    int result = mesh_radio_settings_encode_request(&session->settings, &request, payload,
                                                    sizeof payload, &written);
    if (result < 0) {
        mesh_log_warn("session", "Admin request encode failed: %d", result);
        return;
    }
    result = mesh_session_send_raw(session, payload, written, 0U);
    if (result < 0) {
        /* A failed send may already have dropped the link (and reset the settings with it);
           the note lands in the fresh struct so the app still hears about the lost write. */
        mesh_log_warn("session", "Admin request send failed: %d", result);
        mesh_radio_settings_mark_unsent(&session->settings, &request, result);
        return;
    }
    mesh_radio_settings_mark_sent(&session->settings, request.packet_id, now_ms);
    mesh_log_info("session", "Sent admin request kind=%u type=%u id=%u", (unsigned)request.kind,
                  (unsigned)request.type, request.packet_id);
}

int mesh_session_send_packet(struct mesh_session *session, const uint8_t *packet, size_t len) {
    return mesh_session_send_raw(session, packet, len, 0U);
}

/* The node in the cache with this id, or NULL. */
static struct mesh_node_summary *mesh_session_find_node(struct mesh_session *session,
                                                        uint32_t node_id) {
    for (size_t i = 0; i < session->handshake.node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        if (session->handshake.nodes[i].node_id == node_id) {
            return &session->handshake.nodes[i];
        }
    }
    return NULL;
}

int mesh_session_set_node_ignored(struct mesh_session *session, uint32_t node_id, bool ignored) {
    if (session == NULL || node_id == 0U) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    /* Ignoring the radio we are talking through would drop our own traffic. */
    if (node_id == session->handshake.my_info.my_node_num) {
        return -EINVAL;
    }

    struct mesh_node_summary *summary = mesh_session_find_node(session, node_id);
    if (summary == NULL) {
        return -ENOENT;
    }
    if (summary->is_ignored == ignored) {
        return 0; /* already what the user asked for; nothing to send */
    }

    const int queued = mesh_radio_settings_queue_ignored(&session->settings, node_id, ignored);
    if (queued < 0) {
        return queued;
    }
    summary->is_ignored = ignored;
    mesh_log_info("session", "%s node 0x%08x in the NodeDB (%d requests)",
                  ignored ? "Ignoring" : "No longer ignoring", node_id, queued);
    return queued;
}

int mesh_session_request_node_info(struct mesh_session *session, uint32_t dest) {
    if (session == NULL || dest == 0U || dest == MESH_MESSAGE_BROADCAST_ADDR) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    if (dest == session->handshake.my_info.my_node_num) {
        return -EINVAL;
    }

    /*
     * The firmware answers a NODEINFO_APP carrying want_response with its own NodeInfo. What
     * we send is our own User record, which is also how the far end learns *our* name - the
     * exchange the phone apps offer is the same packet travelling in both directions.
     *
     * Which is exactly why there is no fallback here. A NodeInfo is applied by overwriting
     * the record wholesale - our own mesh_session_apply_user() blanks the names and drops the
     * public key when the incoming User does not carry them, and the firmware's NodeDB does
     * the same - so sending a placeholder User with nothing but an id in it would erase this
     * node's identity on every peer that received it. The owner arrives with our own NodeInfo
     * during the handshake, moments after config_complete, so refusing until then costs a
     * retry at worst.
     */
    if (!session->settings.has_owner) {
        return -EAGAIN;
    }
    const meshtastic_User user = session->settings.owner;

    uint8_t body[192];
    pb_ostream_t body_stream = pb_ostream_from_buffer(body, sizeof body);
    if (!pb_encode(&body_stream, meshtastic_User_fields, &user)) {
        mesh_log_error("session", "Failed to encode our User: %s", PB_GET_ERROR(&body_stream));
        return -EIO;
    }

    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket *packet = &to_radio.packet;
    packet->to = dest;
    packet->id = mesh_session_next_packet_id(session);
    packet->want_ack = false; /* the NodeInfo coming back is the answer */
    packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet->decoded.portnum = meshtastic_PortNum_NODEINFO_APP;
    packet->decoded.want_response = true;
    memcpy(packet->decoded.payload.bytes, body, body_stream.bytes_written);
    packet->decoded.payload.size = (pb_size_t)body_stream.bytes_written;

    uint8_t payload[MESH_SESSION_MAX_PACKET];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &to_radio)) {
        mesh_log_error("session", "Failed to encode NodeInfo request: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }

    const int result = mesh_session_send_raw(session, payload, stream.bytes_written, 0U);
    if (result == 0) {
        mesh_log_info("session", "Asked node 0x%08x to introduce itself", dest);
    }
    return result;
}

/*
 * Meshtastic packet ids only need to be unique per sender for a few minutes, so a cheap
 * xorshift seeded from the monotonic clock is enough. Zero is reserved by the protocol to mean
 * "no id", so it is never handed out.
 */
uint32_t mesh_session_next_packet_id(struct mesh_session *session) {
    if (session->next_packet_id == 0U) {
        uint32_t seed = (uint32_t)mesh_session_now_ms();
        if (session->handshake.has_my_info) {
            seed ^= session->handshake.my_info.my_node_num;
        }
        session->next_packet_id = (seed == 0U) ? 0x9E3779B9U : seed;
    }

    uint32_t value = session->next_packet_id;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    session->next_packet_id = (value == 0U) ? 0x9E3779B9U : value;
    return session->next_packet_id;
}

int mesh_session_send_text(struct mesh_session *session, uint32_t dest, uint8_t channel,
                           const char *text, bool want_ack, uint32_t *out_packet_id) {
    if (session == NULL || text == NULL) {
        return -EINVAL;
    }
    if (session->send == NULL) {
        return -ENOTCONN;
    }

    /* Broadcasts are never acked directly by the mesh; asking for one just wastes airtime. */
    const bool broadcast = (dest == MESH_MESSAGE_BROADCAST_ADDR);
    const bool request_ack = want_ack && !broadcast;

    struct mesh_message_text_request request = {
        .dest = dest,
        .packet_id = mesh_session_next_packet_id(session),
        .text = text,
        .channel = channel,
        .hop_limit = 0U,
        .want_ack = request_ack,
    };

    uint8_t payload[MESH_SESSION_MAX_PACKET];
    size_t written = 0U;
    int encode_result = mesh_message_encode_text(&request, payload, sizeof payload, &written);
    if (encode_result < 0) {
        return encode_result;
    }

    /* Record before the send so a failure has something to mark. */
    struct mesh_message record;
    memset(&record, 0, sizeof record);
    record.packet_id = request.packet_id;
    record.from = session->handshake.has_my_info ? session->handshake.my_info.my_node_num : 0U;
    record.to = dest;
    record.channel = channel;
    record.direction = MESH_MESSAGE_OUTBOUND;
    record.ack = request_ack ? MESH_MESSAGE_ACK_PENDING : MESH_MESSAGE_ACK_NONE;
    snprintf(record.text, sizeof record.text, "%s", text);
    mesh_message_log_append(&session->messages, &record);

    int result = mesh_session_send_raw(session, payload, written, request.packet_id);
    if (result < 0) {
        mesh_message_log_mark_ack(&session->messages, request.packet_id, MESH_MESSAGE_ACK_FAILED,
                                  0U);
        return result;
    }

    if (out_packet_id != NULL) {
        *out_packet_id = request.packet_id;
    }
    mesh_log_info("session", "Queued text message id=%u to 0x%08x on channel %u", request.packet_id,
                  dest, (unsigned)channel);
    return 0;
}

void mesh_session_packet_failed(struct mesh_session *session, uint32_t packet_id) {
    if (session == NULL || packet_id == 0U) {
        return;
    }
    mesh_message_log_mark_ack(&session->messages, packet_id, MESH_MESSAGE_ACK_FAILED, 0U);
}

int mesh_session_refresh_settings(struct mesh_session *session) {
    if (session == NULL) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    return (int)mesh_radio_settings_queue_all(&session->settings);
}

int mesh_session_write_settings(struct mesh_session *session,
                                const struct mesh_admin_request *write) {
    if (session == NULL || write == NULL) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    const int queued = mesh_radio_settings_queue_write(&session->settings, write);
    if (queued > 0) {
        mesh_log_info("session", "Queued settings write kind=%u type=%u (%d requests)",
                      (unsigned)write->kind, (unsigned)write->type, queued);
    }
    return queued;
}

int mesh_session_set_node_favorite(struct mesh_session *session, uint32_t node_id, bool favorite) {
    if (session == NULL || node_id == 0U) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }

    struct mesh_node_summary *summary = mesh_session_find_node(session, node_id);
    if (summary == NULL) {
        return -ENOENT;
    }
    if (summary->is_favorite == favorite) {
        return 0; /* already what the user asked for; nothing to send */
    }

    const int queued = mesh_radio_settings_queue_favorite(&session->settings, node_id, favorite);
    if (queued < 0) {
        return queued;
    }
    summary->is_favorite = favorite;
    mesh_log_info("session", "%s node 0x%08x in the NodeDB (%d requests)",
                  favorite ? "Pinned" : "Unpinned", node_id, queued);
    return queued;
}

int mesh_session_toggle_node_muted(struct mesh_session *session, uint32_t node_id) {
    if (session == NULL || node_id == 0U) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    struct mesh_node_summary *summary = mesh_session_find_node(session, node_id);
    if (summary == NULL) {
        return -ENOENT;
    }
    const int queued = mesh_radio_settings_queue_toggle_muted(&session->settings, node_id);
    if (queued <= 0) {
        /* 0 is the same toggle already queued and deduplicated. The favorite and ignore pair
           can flip their cached flag anyway, because they send the state they want and a
           second request for it is a no-op; a toggle cannot. Flipping twice here for one
           toggle on the wire would leave the row stating the opposite of the truth. */
        return queued;
    }
    /* The wire verb is a toggle, so there is no wanted state to send and none to assume: the
       cached flag follows the request rather than leading it. */
    summary->is_muted = !summary->is_muted;
    mesh_log_info("session", "%s node 0x%08x in the NodeDB (%d requests)",
                  summary->is_muted ? "Muted" : "Unmuted", node_id, queued);
    return queued;
}

int mesh_session_remove_node(struct mesh_session *session, uint32_t node_id) {
    if (session == NULL || node_id == 0U) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    /* Removing the radio we are talking through would take our own record out from under
       every screen that resolves a name through it. */
    if (node_id == session->handshake.my_info.my_node_num) {
        return -EINVAL;
    }
    struct mesh_node_summary *summary = mesh_session_find_node(session, node_id);
    if (summary == NULL) {
        return -ENOENT;
    }
    const int queued = mesh_radio_settings_queue_remove_node(&session->settings, node_id);
    if (queued < 0) {
        return queued;
    }
    /* Drop it here too. There is no read-back, and an entry left in place would sit in the
       list looking removed-but-present until the next connection re-syncs the NodeDB. */
    const size_t index = (size_t)(summary - session->handshake.nodes);
    const size_t last = session->handshake.node_count - 1U;
    if (index < last) {
        memmove(&session->handshake.nodes[index], &session->handshake.nodes[index + 1U],
                (last - index) * sizeof session->handshake.nodes[0]);
    }
    memset(&session->handshake.nodes[last], 0, sizeof session->handshake.nodes[last]);
    session->handshake.node_count = last;
    mesh_log_info("session", "Removed node 0x%08x from the NodeDB (%d requests)", node_id, queued);
    return queued;
}

/* Meshtastic's fixed-point 1e-7 degrees: 90 and 180 degrees as the wire carries them. */
#define MESH_SESSION_LATITUDE_MAX 900000000
#define MESH_SESSION_LONGITUDE_MAX 1800000000

static int mesh_session_queue_fixed_position(struct mesh_session *session,
                                             const struct mesh_admin_request *write) {
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    return mesh_radio_settings_queue_write(&session->settings, write);
}

int mesh_session_set_fixed_position(struct mesh_session *session, int32_t latitude_i,
                                    int32_t longitude_i, bool has_altitude, int32_t altitude) {
    if (session == NULL) {
        return -EINVAL;
    }
    if (latitude_i > MESH_SESSION_LATITUDE_MAX || latitude_i < -MESH_SESSION_LATITUDE_MAX ||
        longitude_i > MESH_SESSION_LONGITUDE_MAX || longitude_i < -MESH_SESSION_LONGITUDE_MAX) {
        return -EINVAL;
    }
    struct mesh_admin_request write;
    memset(&write, 0, sizeof write);
    write.kind = MESH_ADMIN_SET_FIXED_POSITION;
    write.type = (uint32_t)meshtastic_AdminMessage_ConfigType_POSITION_CONFIG;
    write.payload.position.has_latitude_i = true;
    write.payload.position.latitude_i = latitude_i;
    write.payload.position.has_longitude_i = true;
    write.payload.position.longitude_i = longitude_i;
    write.payload.position.has_altitude = has_altitude;
    write.payload.position.altitude = has_altitude ? altitude : 0;
    /* Typed in by a person, which is exactly what LOC_MANUAL means; without it the firmware
       would report the fix as though a GPS had produced it. */
    write.payload.position.location_source = meshtastic_Position_LocSource_LOC_MANUAL;
    const int queued = mesh_session_queue_fixed_position(session, &write);
    if (queued > 0) {
        mesh_log_info("session", "Fixed position set to %d, %d (%d requests)", (int)latitude_i,
                      (int)longitude_i, queued);
    }
    return queued;
}

int mesh_session_clear_fixed_position(struct mesh_session *session) {
    if (session == NULL) {
        return -EINVAL;
    }
    struct mesh_admin_request write;
    memset(&write, 0, sizeof write);
    write.kind = MESH_ADMIN_REMOVE_FIXED_POSITION;
    write.type = (uint32_t)meshtastic_AdminMessage_ConfigType_POSITION_CONFIG;
    const int queued = mesh_session_queue_fixed_position(session, &write);
    if (queued > 0) {
        mesh_log_info("session", "Fixed position cleared (%d requests)", queued);
    }
    return queued;
}

/* For the log line only; the UI has its own labels. */
static const char *mesh_session_action_name(enum mesh_admin_request_kind kind) {
    switch (kind) {
    case MESH_ADMIN_REBOOT:
        return "reboot";
    case MESH_ADMIN_SHUTDOWN:
        return "shutdown";
    case MESH_ADMIN_RESET_NODEDB:
        return "nodedb reset";
    case MESH_ADMIN_FACTORY_RESET_CONFIG:
        return "factory reset (config)";
    case MESH_ADMIN_FACTORY_RESET_DEVICE:
        return "factory reset (device)";
    default:
        return "?";
    }
}

int mesh_session_radio_action(struct mesh_session *session, enum mesh_admin_request_kind kind) {
    if (session == NULL || !mesh_admin_request_is_action(kind)) {
        return -EINVAL;
    }
    if (session->send == NULL || !session->handshake.has_my_info) {
        return -ENOTCONN;
    }
    const int queued =
        mesh_radio_settings_queue_action(&session->settings, kind, MESH_RADIO_ACTION_DELAY_SECONDS);
    if (queued < 0) {
        return queued;
    }
    /* Loud on purpose: this is the one thing the Settings tab does that cannot be undone by
       pressing the opposite row, and the log is what says who asked for it. */
    mesh_log_warn("session", "Requested %s of node 0x%08x (%d requests)",
                  mesh_session_action_name(kind), session->handshake.my_info.my_node_num, queued);
    return queued;
}

const struct mesh_handshake_status *mesh_session_handshake(const struct mesh_session *session) {
    return session != NULL ? &session->handshake : NULL;
}

const struct mesh_message_log *mesh_session_messages(const struct mesh_session *session) {
    return session != NULL ? &session->messages : NULL;
}

const struct mesh_radio_settings *mesh_session_settings(const struct mesh_session *session) {
    return session != NULL ? &session->settings : NULL;
}

const struct mesh_radio_stats *mesh_session_radio_stats(const struct mesh_session *session) {
    return session != NULL ? &session->stats : NULL;
}

int mesh_session_send_traceroute(struct mesh_session *session, uint32_t dest) {
    if (session == NULL) {
        return -EINVAL;
    }
    if (session->send == NULL) {
        return -ENOTCONN;
    }
    /* A broadcast trace would ask the whole mesh to answer at once, and tracing the route to
       ourselves is a question with no links in it. */
    if (dest == 0U || dest == MESH_MESSAGE_BROADCAST_ADDR ||
        (session->handshake.has_my_info && dest == session->handshake.my_info.my_node_num)) {
        return -EINVAL;
    }
    if (session->traceroute.state == MESH_TRACEROUTE_PENDING) {
        return -EBUSY;
    }

    /* An empty RouteDiscovery: every node that forwards it appends itself, so what we send is
       the question and what comes back is the answer. */
    meshtastic_RouteDiscovery route = meshtastic_RouteDiscovery_init_default;
    uint8_t body[64];
    pb_ostream_t body_stream = pb_ostream_from_buffer(body, sizeof body);
    if (!pb_encode(&body_stream, meshtastic_RouteDiscovery_fields, &route)) {
        mesh_log_error("session", "Failed to encode RouteDiscovery: %s",
                       PB_GET_ERROR(&body_stream));
        return -EIO;
    }

    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket *packet = &to_radio.packet;
    packet->to = dest;
    packet->id = mesh_session_next_packet_id(session);
    packet->want_ack = false; /* the reply is the ack; a Routing ack as well is just airtime */
    packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet->decoded.portnum = meshtastic_PortNum_TRACEROUTE_APP;
    packet->decoded.want_response = true;
    memcpy(packet->decoded.payload.bytes, body, body_stream.bytes_written);
    packet->decoded.payload.size = (pb_size_t)body_stream.bytes_written;

    uint8_t payload[MESH_SESSION_MAX_PACKET];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &to_radio)) {
        mesh_log_error("session", "Failed to encode traceroute: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }

    struct mesh_traceroute *trace = &session->traceroute;
    memset(trace, 0, sizeof *trace);
    trace->state = MESH_TRACEROUTE_PENDING;
    trace->target = dest;
    trace->packet_id = packet->id;
    trace->sent_ms = mesh_session_now_ms();

    const int result = mesh_session_send_raw(session, payload, stream.bytes_written, 0U);
    if (result < 0) {
        trace->state = MESH_TRACEROUTE_TIMEOUT;
        return result;
    }
    mesh_log_info("session", "Traceroute to 0x%08x sent (id %u)", dest, trace->packet_id);
    return 0;
}

const struct mesh_traceroute *mesh_session_traceroute(const struct mesh_session *session) {
    return session != NULL ? &session->traceroute : NULL;
}
