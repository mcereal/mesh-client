#include "mesh/core/message.h"

#include "mesh/i18n/strings.h"

#include "mesh/utils/log.h"
#include "mesh/utils/text.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <errno.h>
#include <string.h>

void mesh_message_log_reset(struct mesh_message_log *log) {
    if (log == NULL) {
        return;
    }
    memset(log, 0, sizeof(*log));
}

struct mesh_message *mesh_message_log_append(struct mesh_message_log *log,
                                             const struct mesh_message *message) {
    if (log == NULL || message == NULL) {
        return NULL;
    }

    size_t slot;
    if (log->count < MESH_MESSAGE_LOG_CAPACITY) {
        slot = (log->head + log->count) % MESH_MESSAGE_LOG_CAPACITY;
        log->count++;
    } else {
        /* Full: overwrite the oldest and walk head forward. */
        slot = log->head;
        log->head = (log->head + 1U) % MESH_MESSAGE_LOG_CAPACITY;
        if (log->dropped < UINT32_MAX) {
            log->dropped++;
        }
    }

    log->entries[slot] = *message;
    /* The text field is the only thing backends draw; guarantee termination regardless of
       what the caller handed us. */
    log->entries[slot].text[MESH_MESSAGE_TEXT_MAX] = '\0';
    return &log->entries[slot];
}

const struct mesh_message *mesh_message_log_at(const struct mesh_message_log *log, size_t index) {
    if (log == NULL || index >= log->count) {
        return NULL;
    }
    return &log->entries[(log->head + index) % MESH_MESSAGE_LOG_CAPACITY];
}

bool mesh_message_in_conversation(const struct mesh_message *message, uint32_t peer,
                                  uint8_t channel) {
    if (message == NULL) {
        return false;
    }
    const bool broadcast = (message->to == MESH_MESSAGE_BROADCAST_ADDR);
    if (peer == MESH_MESSAGE_BROADCAST_ADDR) {
        return broadcast && message->channel == channel;
    }
    /* Either direction: what makes a direct conversation one thing is that both halves of it
       name the same node, once as the sender and once as the recipient. */
    return !broadcast && (message->from == peer || message->to == peer);
}

uint32_t mesh_message_log_forget(struct mesh_message_log *log, uint32_t peer, uint8_t channel) {
    if (log == NULL || log->count == 0U) {
        return 0U;
    }

    /* Compact in place into a fresh ring rather than shuffling the old one: the entries are
       oldest-first through a modulo, and every alternative to one forward pass gets that
       wrapping wrong in some corner. */
    struct mesh_message_log kept;
    memset(&kept, 0, sizeof(kept));
    kept.dropped = log->dropped;

    uint32_t removed = 0U;
    for (size_t i = 0; i < log->count; ++i) {
        const struct mesh_message *message =
            &log->entries[(log->head + i) % MESH_MESSAGE_LOG_CAPACITY];
        if (mesh_message_in_conversation(message, peer, channel)) {
            removed++;
            continue;
        }
        kept.entries[kept.count++] = *message;
    }
    if (removed > 0U) {
        *log = kept;
    }
    return removed;
}

struct mesh_message *mesh_message_log_find(struct mesh_message_log *log, uint32_t packet_id) {
    if (log == NULL || packet_id == 0U) {
        return NULL;
    }

    /* Newest first: a packet id can be reused after a wrap, and the recent one is the match
       a caller means. */
    for (size_t i = log->count; i > 0U; --i) {
        size_t slot = (log->head + (i - 1U)) % MESH_MESSAGE_LOG_CAPACITY;
        if (log->entries[slot].packet_id == packet_id) {
            return &log->entries[slot];
        }
    }
    return NULL;
}

bool mesh_message_log_mark_ack(struct mesh_message_log *log, uint32_t packet_id,
                               enum mesh_message_ack ack, uint8_t error) {
    struct mesh_message *entry = mesh_message_log_find(log, packet_id);
    if (entry == NULL || entry->direction != MESH_MESSAGE_OUTBOUND) {
        return false;
    }

    entry->ack = (uint8_t)ack;
    entry->ack_error = error;
    return true;
}

const char *mesh_message_ack_to_string(enum mesh_message_ack ack) {
    switch (ack) {
    case MESH_MESSAGE_ACK_NONE:
        return "none";
    case MESH_MESSAGE_ACK_PENDING:
        return "pending";
    case MESH_MESSAGE_ACK_DELIVERED:
        return "delivered";
    case MESH_MESSAGE_ACK_FAILED:
        return "failed";
    }
    return "unknown";
}

int mesh_message_encode_text(const struct mesh_message_text_request *request, uint8_t *out,
                             size_t out_len, size_t *written) {
    if (request == NULL || request->text == NULL || out == NULL || written == NULL) {
        return -EINVAL;
    }

    size_t text_len = strlen(request->text);
    if (text_len == 0U) {
        return -EINVAL;
    }
    if (text_len > MESH_MESSAGE_TEXT_MAX) {
        return -EMSGSIZE;
    }

    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;

    meshtastic_MeshPacket *packet = &to_radio.packet;
    packet->to = request->dest;
    packet->channel = request->channel;
    packet->id = request->packet_id;
    packet->want_ack = request->want_ack;
    if (request->hop_limit != 0U) {
        packet->hop_limit = request->hop_limit;
    }
    packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    memcpy(packet->decoded.payload.bytes, request->text, text_len);
    packet->decoded.payload.size = (pb_size_t)text_len;

    pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &to_radio)) {
        mesh_log_error("message", "Failed to encode text message: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }

    *written = stream.bytes_written;
    return 0;
}

/*
 * The Routing_Error a failed delivery came back with, as a catalog id.
 *
 * Two callers want two different languages out of it: a bubble wants the user's, and the log
 * wants English, because a bug report the maintainer cannot read is worse than none. Keeping
 * the *mapping* here and letting each caller pick the locale is what avoids a second table
 * that would drift out of step with this one.
 */
static enum mesh_str_id mesh_message_ack_error_id(uint8_t error) {
    switch ((meshtastic_Routing_Error)error) {
    case meshtastic_Routing_Error_NONE:
        return MESH_STR_ACK_DELIVERED;
    case meshtastic_Routing_Error_NO_ROUTE:
        return MESH_STR_ACK_NO_ROUTE;
    case meshtastic_Routing_Error_GOT_NAK:
        return MESH_STR_ACK_GOT_NAK;
    case meshtastic_Routing_Error_TIMEOUT:
        return MESH_STR_ACK_TIMEOUT;
    case meshtastic_Routing_Error_NO_INTERFACE:
        return MESH_STR_ACK_NO_INTERFACE;
    case meshtastic_Routing_Error_MAX_RETRANSMIT:
        /* The common one: the packet went out and nothing acked it. Out of range, on another
           LoRa config, or off. */
        return MESH_STR_ACK_MAX_RETRANSMIT;
    case meshtastic_Routing_Error_NO_CHANNEL:
        return MESH_STR_ACK_NO_CHANNEL;
    case meshtastic_Routing_Error_TOO_LARGE:
        return MESH_STR_ACK_TOO_LARGE;
    case meshtastic_Routing_Error_NO_RESPONSE:
        return MESH_STR_ACK_NO_RESPONSE;
    case meshtastic_Routing_Error_DUTY_CYCLE_LIMIT:
        return MESH_STR_ACK_DUTY_CYCLE;
    case meshtastic_Routing_Error_BAD_REQUEST:
        return MESH_STR_ACK_BAD_REQUEST;
    case meshtastic_Routing_Error_NOT_AUTHORIZED:
        return MESH_STR_ACK_NOT_AUTHORIZED;
    case meshtastic_Routing_Error_PKI_FAILED:
        return MESH_STR_ACK_PKI_FAILED;
    case meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY:
        return MESH_STR_ACK_PKI_UNKNOWN_PUBKEY;
    case meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY:
        return MESH_STR_ACK_ADMIN_BAD_SESSION;
    case meshtastic_Routing_Error_ADMIN_PUBLIC_KEY_UNAUTHORIZED:
        return MESH_STR_ACK_ADMIN_UNAUTHORIZED;
    case meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        return MESH_STR_ACK_RATE_LIMIT;
    case meshtastic_Routing_Error_PKI_SEND_FAIL_PUBLIC_KEY:
        return MESH_STR_ACK_PKI_SEND_FAIL;
    default:
        break;
    }
    return MESH_STR_ACK_UNKNOWN_ERROR;
}

/* What a bubble shows, in the reader's language. */
const char *mesh_message_ack_error_to_string(uint8_t error) {
    return mesh_str(mesh_message_ack_error_id(error));
}

/* Routing replies carry the id of the message they are answering in Data.request_id. */
static int mesh_message_handle_routing(struct mesh_message_log *log, const meshtastic_Data *data) {
    if (data->request_id == 0U) {
        return 0;
    }

    meshtastic_Routing routing = meshtastic_Routing_init_default;
    pb_istream_t stream = pb_istream_from_buffer(data->payload.bytes, data->payload.size);
    if (!pb_decode(&stream, meshtastic_Routing_fields, &routing)) {
        mesh_log_debug("message", "Ignoring undecodable Routing reply for id %u", data->request_id);
        return 0;
    }

    if (routing.which_variant != meshtastic_Routing_error_reason_tag) {
        /* A route request/reply, not a delivery result for one of our messages. */
        return 0;
    }

    const bool delivered = (routing.error_reason == meshtastic_Routing_Error_NONE);
    if (mesh_message_log_mark_ack(log, data->request_id,
                                  delivered ? MESH_MESSAGE_ACK_DELIVERED : MESH_MESSAGE_ACK_FAILED,
                                  (uint8_t)routing.error_reason)) {
        if (delivered) {
            mesh_log_info("message", "Message %u delivered", data->request_id);
        } else {
            /* English, whatever the UI is set to: this line is read by whoever is tailing
               the device log, and a diagnostic that changes language with the handheld's
               settings is a diagnostic that cannot be searched for. */
            mesh_log_warn("message", "Message %u failed: %s (Routing_Error %u)", data->request_id,
                          mesh_str_in(mesh_i18n_locale_english(),
                                      mesh_message_ack_error_id((uint8_t)routing.error_reason)),
                          (unsigned)routing.error_reason);
        }
    }
    return 0;
}

int mesh_message_ingest(struct mesh_message_log *log, const meshtastic_MeshPacket *packet,
                        uint32_t my_node_num) {
    if (log == NULL || packet == NULL) {
        return -EINVAL;
    }

    if (packet->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        /* Encrypted: we hold no channel keys, so there is nothing to show. */
        return 0;
    }

    const meshtastic_Data *data = &packet->decoded;

    if (data->portnum == meshtastic_PortNum_ROUTING_APP) {
        return mesh_message_handle_routing(log, data);
    }

    /*
     * Three ports carry a plain text payload addressed to a channel: the ordinary one, the
     * detection sensor module's, and the firmware's critical alert. Upstream describes the
     * latter two as "same as Text Message", and they are - they were simply never accepted
     * here, so a sensor tripping or an alert going out reached this client and vanished.
     */
    enum mesh_message_kind kind;
    switch (data->portnum) {
    case meshtastic_PortNum_TEXT_MESSAGE_APP:
        kind = MESH_MESSAGE_KIND_TEXT;
        break;
    case meshtastic_PortNum_ALERT_APP:
        kind = MESH_MESSAGE_KIND_ALERT;
        break;
    case meshtastic_PortNum_DETECTION_SENSOR_APP:
        kind = MESH_MESSAGE_KIND_DETECTION;
        break;
    default:
        return 0;
    }

    struct mesh_message message;
    memset(&message, 0, sizeof(message));
    message.kind = (uint8_t)kind;
    message.packet_id = packet->id;
    message.from = packet->from;
    message.to = packet->to;
    message.channel = packet->channel;
    message.rx_time = packet->has_rx_time ? packet->rx_time : 0U;
    message.rx_snr = packet->rx_snr;
    message.direction = (my_node_num != 0U && packet->from == my_node_num) ? MESH_MESSAGE_OUTBOUND
                                                                           : MESH_MESSAGE_INBOUND;
    /* hop_start is only trustworthy once the firmware populates it (2.5.0+); a zero start is
       "unknown", not "direct neighbour". */
    if (packet->hop_start != 0U && packet->hop_start >= packet->hop_limit) {
        message.has_hops_away = true;
        message.hops_away = (uint8_t)(packet->hop_start - packet->hop_limit);
    }
    message.pki_encrypted = packet->pki_encrypted;
    message.reply_id = data->reply_id;
    /* `emoji` is a fixed32 used as a flag: non-zero means the payload is an emoji reacting to
       reply_id rather than something to read on its own line. */
    message.is_reaction = (data->emoji != 0U);
    mesh_text_sanitise(data->payload.bytes, data->payload.size, message.text, sizeof(message.text));

    if (message.text[0] == '\0') {
        return 0;
    }

    /*
     * The radio echoes our own sends back to us. Refresh the entry we already hold instead of
     * showing the message twice.
     *
     * The echo is the only place some of this can come from. mesh_session_send_text() records
     * the message before the radio has done anything with it, so `pki_encrypted` starts false
     * and the radio's own decision - it picks per-packet, from whether it holds the recipient's
     * public key - arrives only here. Copying just the timestamps left every outbound direct
     * message without its padlock however it actually went out.
     */
    struct mesh_message *existing = mesh_message_log_find(log, message.packet_id);
    if (existing != NULL && existing->direction == MESH_MESSAGE_OUTBOUND &&
        message.direction == MESH_MESSAGE_OUTBOUND) {
        existing->rx_time = message.rx_time;
        existing->rx_snr = message.rx_snr;
        existing->pki_encrypted = message.pki_encrypted;
        return 0;
    }

    if (mesh_message_log_append(log, &message) == NULL) {
        return -ENOMEM;
    }

    static const char *const k_kind_names[] = {"text", "alert", "detection"};
    mesh_log_info("message", "%s %s from 0x%08x on channel %u (%zu chars)",
                  message.direction == MESH_MESSAGE_OUTBOUND ? "Echoed" : "Received",
                  message.is_reaction ? "reaction" : k_kind_names[message.kind], message.from,
                  (unsigned)message.channel, strlen(message.text));
    return 1;
}
