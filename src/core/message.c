#include "mesh/core/message.h"

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
 * The Routing_Error a failed delivery came back with, in words. Without this a failed message
 * is just "!!" on the screen and "failed" in the log, which says nothing about whether to try
 * again, move the node, or fix a key - and those are entirely different problems.
 */
const char *mesh_message_ack_error_to_string(uint8_t error) {
    switch ((meshtastic_Routing_Error)error) {
    case meshtastic_Routing_Error_NONE:
        return "delivered";
    case meshtastic_Routing_Error_NO_ROUTE:
        return "no route to that node";
    case meshtastic_Routing_Error_GOT_NAK:
        return "rejected by the mesh";
    case meshtastic_Routing_Error_TIMEOUT:
        return "timed out";
    case meshtastic_Routing_Error_NO_INTERFACE:
        return "no radio interface";
    case meshtastic_Routing_Error_MAX_RETRANSMIT:
        /* The common one: the packet went out and nothing acked it. Out of range, on another
           LoRa config, or off. */
        return "no ack after retries";
    case meshtastic_Routing_Error_NO_CHANNEL:
        return "no matching channel";
    case meshtastic_Routing_Error_TOO_LARGE:
        return "message too large";
    case meshtastic_Routing_Error_NO_RESPONSE:
        return "no response";
    case meshtastic_Routing_Error_DUTY_CYCLE_LIMIT:
        return "duty cycle limit";
    case meshtastic_Routing_Error_BAD_REQUEST:
        return "bad request";
    case meshtastic_Routing_Error_NOT_AUTHORIZED:
        return "not authorized";
    case meshtastic_Routing_Error_PKI_FAILED:
        return "encryption failed";
    case meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY:
        return "no public key for that node";
    case meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY:
        return "admin session expired";
    case meshtastic_Routing_Error_ADMIN_PUBLIC_KEY_UNAUTHORIZED:
        return "admin key not authorized";
    case meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        return "rate limited";
    case meshtastic_Routing_Error_PKI_SEND_FAIL_PUBLIC_KEY:
        return "public key send failed";
    default:
        break;
    }
    return "unknown error";
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
            mesh_log_warn("message", "Message %u failed: %s (Routing_Error %u)", data->request_id,
                          mesh_message_ack_error_to_string((uint8_t)routing.error_reason),
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

    if (data->portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) {
        return 0;
    }

    struct mesh_message message;
    memset(&message, 0, sizeof(message));
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
    mesh_text_sanitise(data->payload.bytes, data->payload.size, message.text, sizeof(message.text));

    if (message.text[0] == '\0') {
        return 0;
    }

    /* The radio echoes our own sends back to us. Refresh the entry we already hold instead of
       showing the message twice. */
    struct mesh_message *existing = mesh_message_log_find(log, message.packet_id);
    if (existing != NULL && existing->direction == MESH_MESSAGE_OUTBOUND &&
        message.direction == MESH_MESSAGE_OUTBOUND) {
        existing->rx_time = message.rx_time;
        existing->rx_snr = message.rx_snr;
        return 0;
    }

    if (mesh_message_log_append(log, &message) == NULL) {
        return -ENOMEM;
    }

    mesh_log_info("message", "%s text from 0x%08x on channel %u (%zu chars)",
                  message.direction == MESH_MESSAGE_OUTBOUND ? "Echoed" : "Received", message.from,
                  (unsigned)message.channel, strlen(message.text));
    return 1;
}
