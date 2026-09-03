#include "mesh/mesh_message.h"

#include "mesh/log.h"

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

/*
 * Length of the well-formed UTF-8 sequence starting at `bytes`, or 0 when the bytes there are
 * not one. Rejects overlong forms, UTF-16 surrogates and anything above U+10FFFF, per RFC 3629.
 *
 * A lenient decoder is not good enough here: JSON text must be valid UTF-8, so a malformed byte
 * copied through to --status --json would produce a document that standards-compliant parsers
 * reject - and the bytes come from the radio, where any node can choose them.
 */
static size_t mesh_message_utf8_sequence_len(const uint8_t *bytes, size_t available) {
    if (available == 0U) {
        return 0U;
    }

    const uint8_t lead = bytes[0];
    if (lead < 0x80U) {
        return 1U;
    }

    size_t length;
    uint8_t second_min;
    uint8_t second_max;

    if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2U;
        second_min = 0x80U;
        second_max = 0xBFU;
    } else if (lead == 0xE0U) {
        length = 3U; /* E0 80..9F would be an overlong two-byte form */
        second_min = 0xA0U;
        second_max = 0xBFU;
    } else if (lead == 0xEDU) {
        length = 3U; /* ED A0..BF encodes a UTF-16 surrogate, which is not a character */
        second_min = 0x80U;
        second_max = 0x9FU;
    } else if ((lead >= 0xE1U && lead <= 0xECU) || lead == 0xEEU || lead == 0xEFU) {
        length = 3U;
        second_min = 0x80U;
        second_max = 0xBFU;
    } else if (lead == 0xF0U) {
        length = 4U; /* F0 80..8F would be an overlong three-byte form */
        second_min = 0x90U;
        second_max = 0xBFU;
    } else if (lead >= 0xF1U && lead <= 0xF3U) {
        length = 4U;
        second_min = 0x80U;
        second_max = 0xBFU;
    } else if (lead == 0xF4U) {
        length = 4U; /* F4 90.. would be above U+10FFFF */
        second_min = 0x80U;
        second_max = 0x8FU;
    } else {
        /* 0x80-0xBF is a stray continuation; 0xC0/0xC1 are overlong; 0xF5-0xFF are out of range. */
        return 0U;
    }

    if (available < length) {
        return 0U;
    }
    if (bytes[1] < second_min || bytes[1] > second_max) {
        return 0U;
    }
    for (size_t i = 2; i < length; ++i) {
        if (bytes[i] < 0x80U || bytes[i] > 0xBFU) {
            return 0U;
        }
    }

    return length;
}

/*
 * Message text arrives from the mesh: any node can put arbitrary bytes here, and it lands in a
 * framebuffer, a log line and a JSON document. Fold C0 controls and DEL away, and replace any
 * byte that is not part of a well-formed UTF-8 sequence with '?', so no downstream consumer has
 * to re-check it. Well-formed multi-byte characters are copied through whole - the CLI and JSON
 * paths handle them, and the framebuffer font maps anything it cannot draw to '?'.
 */
static void mesh_message_sanitise_text(const uint8_t *payload, size_t len, char *out,
                                       size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }

    size_t written = 0;
    size_t i = 0;
    while (i < len) {
        const uint8_t byte = payload[i];
        if (byte == '\0') {
            /* Embedded NUL: treat it as the end of the text rather than truncating silently
               later in a str* call. */
            break;
        }

        if (byte < 0x80U) {
            if (written + 1U > out_len - 1U) {
                break;
            }
            if (byte == '\t' || byte == '\n' || byte == '\r') {
                out[written++] = ' ';
            } else if (byte < 0x20U || byte == 0x7FU) {
                out[written++] = '?';
            } else {
                out[written++] = (char)byte;
            }
            i += 1U;
            continue;
        }

        const size_t sequence = mesh_message_utf8_sequence_len(&payload[i], len - i);
        if (sequence == 0U) {
            if (written + 1U > out_len - 1U) {
                break;
            }
            out[written++] = '?';
            i += 1U;
            continue;
        }

        /* Never split a character across the truncation boundary. */
        if (written + sequence > out_len - 1U) {
            break;
        }
        memcpy(&out[written], &payload[i], sequence);
        written += sequence;
        i += sequence;
    }

    out[written] = '\0';
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
        mesh_log_info("message", "Message %u %s%s", data->request_id,
                      delivered ? "delivered" : "failed", delivered ? "" : " (see error reason)");
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
    mesh_message_sanitise_text(data->payload.bytes, data->payload.size, message.text,
                               sizeof(message.text));

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
