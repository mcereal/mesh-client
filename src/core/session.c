#define _POSIX_C_SOURCE 200809L

#include "mesh/session.h"

#include "mesh/log.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/channel.pb.h"
#include "meshtastic/mesh.pb.h"

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

static void mesh_session_store_node_summary(struct mesh_session *session,
                                            const meshtastic_NodeInfo *info) {
    struct mesh_handshake_status *handshake = &session->handshake;
    if (handshake->node_count > MESH_SESSION_MAX_NODES) {
        handshake->node_count = MESH_SESSION_MAX_NODES;
    }

    size_t index = handshake->node_count;
    for (size_t i = 0; i < handshake->node_count; ++i) {
        if (handshake->nodes[i].node_id == info->num) {
            index = i;
            break;
        }
    }

    if (index == handshake->node_count) {
        if (handshake->node_count >= MESH_SESSION_MAX_NODES) {
            if (!session->node_cache_warned) {
                mesh_log_warn("session",
                              "Node cache full (%u); further nodes dropped for this sync",
                              (unsigned)MESH_SESSION_MAX_NODES);
                session->node_cache_warned = true;
            }
            return;
        }
        handshake->node_count += 1U;
    }

    struct mesh_node_summary *summary = &handshake->nodes[index];
    memset(summary, 0, sizeof *summary);
    summary->node_id = info->num;
    summary->last_heard = info->last_heard;
    summary->snr = info->snr;
    summary->via_mqtt = info->via_mqtt;
    summary->has_hops_away = info->has_hops_away;
    summary->hops_away = info->hops_away;

    if (info->has_user) {
        snprintf(summary->long_name, sizeof summary->long_name, "%s", info->user.long_name);
        snprintf(summary->short_name, sizeof summary->short_name, "%s", info->user.short_name);
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

    struct mesh_node_summary *summary = NULL;
    for (size_t i = 0; i < handshake->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        if (handshake->nodes[i].node_id == packet->from) {
            summary = &handshake->nodes[i];
            break;
        }
    }
    if (summary == NULL) {
        if (handshake->node_count >= MESH_SESSION_MAX_NODES) {
            return;
        }
        summary = &handshake->nodes[handshake->node_count++];
        memset(summary, 0, sizeof *summary);
        summary->node_id = packet->from;
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
        mesh_session_touch_node_from_packet(session, &message.packet);
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
    if (session == NULL || session->send == NULL || !session->handshake.has_my_info) {
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

const struct mesh_handshake_status *mesh_session_handshake(const struct mesh_session *session) {
    return session != NULL ? &session->handshake : NULL;
}

const struct mesh_message_log *mesh_session_messages(const struct mesh_session *session) {
    return session != NULL ? &session->messages : NULL;
}

const struct mesh_radio_settings *mesh_session_settings(const struct mesh_session *session) {
    return session != NULL ? &session->settings : NULL;
}
