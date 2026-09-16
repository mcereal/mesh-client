#include "mesh/proto/mqtt_packet.h"

#include <errno.h>
#include <string.h>

/*
 * The protocol name and level that identify 3.1.1. Both are fixed: the name is literally the
 * four bytes "MQTT" length-prefixed like any other string, and 4 is the level number 3.1.1 was
 * given (3.1 was 3, and 5.0 is 5). A broker that does not speak it answers CONNACK 1 rather
 * than dropping the connection, which is the one refusal this client can explain properly.
 */
static const uint8_t mqtt_protocol_header[] = {0x00U, 0x04U, 'M', 'Q', 'T', 'T', 0x04U};

/* CONNECT flags, byte 8 of the variable header. Bit 0 is reserved and must be zero. */
#define MQTT_CONNECT_FLAG_CLEAN_SESSION 0x02U
#define MQTT_CONNECT_FLAG_PASSWORD 0x40U
#define MQTT_CONNECT_FLAG_USERNAME 0x80U

/* PUBLISH flags, the low nibble of byte 0. */
#define MQTT_PUBLISH_FLAG_RETAIN 0x01U
#define MQTT_PUBLISH_FLAG_QOS_MASK 0x06U
#define MQTT_PUBLISH_FLAG_QOS_SHIFT 1U
#define MQTT_PUBLISH_FLAG_DUP 0x08U

/*
 * The flags nibble three packet types are required to carry.
 *
 * 0b0010 is not decoration. SUBSCRIBE, UNSUBSCRIBE and PUBREL all travel at QoS 1 by
 * definition, and 3.1.1 froze that into the fixed header rather than leaving it settable - a
 * broker that receives one of these with any other nibble is required to close the connection.
 * Sending it right matters as much as checking it.
 */
#define MQTT_FLAGS_QOS1 0x02U

/* The longest string MQTT can length-prefix, since the prefix is 16 bits. */
#define MQTT_STRING_MAX 65535U

/* ------------------------------------------------------------------ primitives */

/*
 * The variable-length integer 3.1.1 uses for a remaining length: seven bits of value per byte,
 * the top bit saying another follows, little end first.
 *
 * Returns the bytes consumed, 0 when `len` ran out mid-number, or -EBADMSG for a fifth
 * continuation byte. That last one is the bound that matters: without it a stream of 0xFF is an
 * unterminated number, and a decoder reading it walks off whatever buffer it was given.
 */
static int mqtt_read_varint(const uint8_t *data, size_t len, size_t *out) {
    uint32_t value = 0U;
    uint32_t multiplier = 1U;

    for (size_t i = 0U; i < MESH_MQTT_REMAINING_BYTES_MAX; ++i) {
        if (i >= len) {
            return 0;
        }
        const uint8_t byte = data[i];
        value += (uint32_t)(byte & 0x7FU) * multiplier;
        multiplier *= 128U;
        if ((byte & 0x80U) == 0U) {
            *out = (size_t)value;
            return (int)(i + 1U);
        }
    }
    return -EBADMSG;
}

/* The same number written out. Returns the bytes used, which is 1..4 for anything in range. */
static size_t mqtt_write_varint(uint8_t *out, size_t value) {
    size_t written = 0U;
    do {
        uint8_t byte = (uint8_t)(value % 128U);
        value /= 128U;
        if (value > 0U) {
            byte = (uint8_t)(byte | 0x80U);
        }
        out[written++] = byte;
    } while (value > 0U);
    return written;
}

/* How many bytes the length prefix for a body of `value` will take. Needed before the body is
   written, because the prefix comes first and its own width depends on what follows it. */
static size_t mqtt_varint_width(size_t value) {
    size_t width = 1U;
    while (value >= 128U) {
        value /= 128U;
        width++;
    }
    return width;
}

static uint16_t mqtt_read_u16(const uint8_t *data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static void mqtt_write_u16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)(value & 0xFFU);
}

/* A length-prefixed string, written at `out`. The caller has already checked it fits. */
static size_t mqtt_write_string(uint8_t *out, const char *text, size_t text_len) {
    mqtt_write_u16(out, (uint16_t)text_len);
    if (text_len > 0U) {
        memcpy(out + 2, text, text_len);
    }
    return 2U + text_len;
}

/* strlen for an optional string, where NULL and "" mean the same thing to every caller here. */
static size_t mqtt_optional_len(const char *text) { return text == NULL ? 0U : strlen(text); }

/* ------------------------------------------------------------------ the fixed header */

/*
 * Whether byte 0's low nibble is one this packet type is allowed to carry.
 *
 * Checked rather than ignored because it is the cheapest possible detection of a desynchronised
 * stream. If the reader's idea of where a packet starts has drifted - the one failure a framed
 * protocol has to notice - then the odds are overwhelming that the byte it thinks is a header
 * carries flags its type forbids, and saying so here turns a silent stream of nonsense into one
 * dropped connection and a reconnect.
 */
static bool mqtt_flags_are_legal(enum mesh_mqtt_packet_type type, uint8_t flags) {
    switch (type) {
    case MESH_MQTT_PUBLISH:
        /* Free: DUP, QoS and RETAIN all live here. A QoS of 3 is malformed, but that is
           mesh_mqtt_decode_publish()'s to refuse - a packet this decoder cannot interpret can
           still have its boundary found, and conflating the two would leave no way to skip one. */
        return true;
    case MESH_MQTT_PUBREL:
    case MESH_MQTT_SUBSCRIBE:
    case MESH_MQTT_UNSUBSCRIBE:
        return flags == MQTT_FLAGS_QOS1;
    case MESH_MQTT_CONNECT:
    case MESH_MQTT_CONNACK:
    case MESH_MQTT_PUBACK:
    case MESH_MQTT_PUBREC:
    case MESH_MQTT_PUBCOMP:
    case MESH_MQTT_SUBACK:
    case MESH_MQTT_UNSUBACK:
    case MESH_MQTT_PINGREQ:
    case MESH_MQTT_PINGRESP:
    case MESH_MQTT_DISCONNECT:
        return flags == 0U;
    case MESH_MQTT_PACKET_NONE:
    default:
        return false;
    }
}

int mesh_mqtt_decode_header(const uint8_t *data, size_t len, struct mesh_mqtt_header *out) {
    if (data == NULL || out == NULL) {
        return -EINVAL;
    }
    if (len == 0U) {
        return 0;
    }

    const uint8_t first = data[0];
    const uint8_t raw_type = (uint8_t)(first >> 4);
    const uint8_t flags = (uint8_t)(first & 0x0FU);
    /* 0 and 15 are both reserved in 3.1.1 and neither will ever be anything. */
    if (raw_type == 0U || raw_type == 15U) {
        return -EBADMSG;
    }
    const enum mesh_mqtt_packet_type type = (enum mesh_mqtt_packet_type)raw_type;
    if (!mqtt_flags_are_legal(type, flags)) {
        return -EBADMSG;
    }

    size_t remaining = 0U;
    const int varint = mqtt_read_varint(data + 1, len - 1U, &remaining);
    if (varint <= 0) {
        return varint; /* 0 for "not yet", -EBADMSG for a fifth byte */
    }

    out->type = type;
    out->flags = flags;
    out->remaining = remaining;
    out->header_len = 1U + (size_t)varint;
    return (int)out->header_len;
}

/* ------------------------------------------------------------------ bodies in */

int mesh_mqtt_decode_publish(uint8_t flags, const uint8_t *body, size_t len,
                             struct mesh_mqtt_incoming *out) {
    if (out == NULL || (body == NULL && len > 0U)) {
        return -EINVAL;
    }
    memset(out, 0, sizeof *out);

    const uint8_t qos =
        (uint8_t)((flags & MQTT_PUBLISH_FLAG_QOS_MASK) >> MQTT_PUBLISH_FLAG_QOS_SHIFT);
    if (qos > 2U) {
        return -EBADMSG;
    }
    if (len < 2U) {
        return -EBADMSG;
    }

    const size_t topic_len = (size_t)mqtt_read_u16(body);
    /*
     * A topic name is at least one character (3.1.1 4.7.3), so a zero-length one is malformed -
     * and on a stream, malformed here most likely means the reader is at the wrong offset.
     *
     * Refusing it also keeps a promise this function makes about its output: `topic` always
     * points *into* the body it was handed. The alternative - returning "" so a caller never
     * sees a NULL - hands back a string literal from somewhere else entirely, and any caller
     * doing arithmetic against the body would be doing it against the wrong buffer. A fuzzer
     * found exactly that.
     */
    if (topic_len == 0U) {
        return -EBADMSG;
    }
    size_t offset = 2U + topic_len;
    if (offset > len) {
        return -EBADMSG;
    }

    /*
     * The packet id is present only above QoS 0, and it sits *between* the topic and the
     * payload. Reading it unconditionally would eat the first two bytes of every QoS 0 payload,
     * which is a corruption with no symptom at this layer - the packet still decodes, it just
     * carries the wrong bytes.
     */
    if (qos > 0U) {
        if (len - offset < 2U) {
            return -EBADMSG;
        }
        out->packet_id = mqtt_read_u16(body + offset);
        offset += 2U;
    }

    out->topic = (const char *)(body + 2);
    out->topic_len = topic_len;
    out->payload = body + offset;
    out->payload_len = len - offset;
    out->qos = qos;
    out->retained = (flags & MQTT_PUBLISH_FLAG_RETAIN) != 0U;
    out->duplicate = (flags & MQTT_PUBLISH_FLAG_DUP) != 0U;
    return 0;
}

int mesh_mqtt_decode_connack(const uint8_t *body, size_t len, uint8_t *code,
                             bool *session_present) {
    if (body == NULL || code == NULL) {
        return -EINVAL;
    }
    /* Exactly two, not at least two: a CONNACK is a fixed shape and a longer one means this is
       not the packet we think it is. */
    if (len != 2U) {
        return -EBADMSG;
    }
    if ((body[0] & 0xFEU) != 0U) {
        return -EBADMSG;
    }
    if (session_present != NULL) {
        *session_present = (body[0] & 0x01U) != 0U;
    }
    *code = body[1];
    return 0;
}

int mesh_mqtt_decode_suback(const uint8_t *body, size_t len, uint16_t *packet_id, uint8_t *code) {
    if (body == NULL || packet_id == NULL || code == NULL) {
        return -EINVAL;
    }
    if (len < 3U) {
        return -EBADMSG;
    }
    *packet_id = mqtt_read_u16(body);
    *code = body[2];
    return 0;
}

/* ------------------------------------------------------------------ bodies out */

/*
 * Writes a fixed header for a body of `body_len` and returns where the body goes, or NULL when
 * the whole packet would not fit in `cap`.
 *
 * The check is made here, once, against the total - header plus body - rather than by each
 * encoder against its own part. An encoder that checked only its body could write a header into
 * the last bytes of the buffer and then discover there was no room for what it described.
 */
static uint8_t *mqtt_begin_packet(uint8_t *out, size_t cap, enum mesh_mqtt_packet_type type,
                                  uint8_t flags, size_t body_len, size_t *total) {
    if (body_len > MESH_MQTT_REMAINING_MAX) {
        return NULL;
    }
    const size_t header_len = 1U + mqtt_varint_width(body_len);
    if (cap < header_len + body_len) {
        return NULL;
    }
    out[0] = (uint8_t)(((uint8_t)type << 4) | (flags & 0x0FU));
    (void)mqtt_write_varint(out + 1, body_len);
    *total = header_len + body_len;
    return out + header_len;
}

int mesh_mqtt_encode_connect(uint8_t *out, size_t cap, const struct mesh_mqtt_connect *params) {
    if (out == NULL || params == NULL || params->client_id == NULL) {
        return -EINVAL;
    }

    const size_t client_id_len = strlen(params->client_id);
    const size_t username_len = mqtt_optional_len(params->username);
    const size_t password_len = mqtt_optional_len(params->password);

    if (client_id_len > MQTT_STRING_MAX || username_len > MQTT_STRING_MAX ||
        password_len > MQTT_STRING_MAX) {
        return -EINVAL;
    }
    /*
     * A zero-length client id asks the broker to assign one, which 3.1.1 only permits with a
     * clean session - there would be nothing to reattach a persistent session to. Refused here
     * rather than sent, because the broker's own refusal arrives as CONNACK 2 and reads to the
     * user as the broker rejecting a name they never chose.
     */
    if (client_id_len == 0U && !params->clean_session) {
        return -EINVAL;
    }
    /* 3.1.1: a password may only accompany a username. */
    if (password_len > 0U && username_len == 0U) {
        return -EINVAL;
    }

    size_t body_len = sizeof mqtt_protocol_header + 1U + 2U; /* name+level, flags, keepalive */
    body_len += 2U + client_id_len;
    if (username_len > 0U) {
        body_len += 2U + username_len;
    }
    if (password_len > 0U) {
        body_len += 2U + password_len;
    }

    size_t total = 0U;
    uint8_t *body = mqtt_begin_packet(out, cap, MESH_MQTT_CONNECT, 0U, body_len, &total);
    if (body == NULL) {
        return -ENOSPC;
    }

    uint8_t flags = 0U;
    if (params->clean_session) {
        flags = (uint8_t)(flags | MQTT_CONNECT_FLAG_CLEAN_SESSION);
    }
    if (username_len > 0U) {
        flags = (uint8_t)(flags | MQTT_CONNECT_FLAG_USERNAME);
    }
    if (password_len > 0U) {
        flags = (uint8_t)(flags | MQTT_CONNECT_FLAG_PASSWORD);
    }

    size_t at = 0U;
    memcpy(body, mqtt_protocol_header, sizeof mqtt_protocol_header);
    at += sizeof mqtt_protocol_header;
    body[at++] = flags;
    mqtt_write_u16(body + at, params->keepalive_s);
    at += 2U;
    at += mqtt_write_string(body + at, params->client_id, client_id_len);
    if (username_len > 0U) {
        at += mqtt_write_string(body + at, params->username, username_len);
    }
    if (password_len > 0U) {
        at += mqtt_write_string(body + at, params->password, password_len);
    }
    return (int)total;
}

int mesh_mqtt_encode_publish(uint8_t *out, size_t cap, const char *topic, const uint8_t *payload,
                             size_t payload_len, bool retained) {
    if (out == NULL || topic == NULL || (payload == NULL && payload_len > 0U)) {
        return -EINVAL;
    }
    const size_t topic_len = strlen(topic);
    if (topic_len == 0U || topic_len > MQTT_STRING_MAX) {
        return -EINVAL;
    }

    const size_t body_len = 2U + topic_len + payload_len;
    size_t total = 0U;
    const uint8_t flags = retained ? (uint8_t)MQTT_PUBLISH_FLAG_RETAIN : 0U;
    uint8_t *body = mqtt_begin_packet(out, cap, MESH_MQTT_PUBLISH, flags, body_len, &total);
    if (body == NULL) {
        return -ENOSPC;
    }

    size_t at = mqtt_write_string(body, topic, topic_len);
    if (payload_len > 0U) {
        memcpy(body + at, payload, payload_len);
    }
    return (int)total;
}

int mesh_mqtt_encode_subscribe(uint8_t *out, size_t cap, uint16_t packet_id, const char *filter) {
    if (out == NULL || filter == NULL || packet_id == 0U) {
        return -EINVAL;
    }
    const size_t filter_len = strlen(filter);
    if (filter_len == 0U || filter_len > MQTT_STRING_MAX) {
        return -EINVAL;
    }

    const size_t body_len = 2U + 2U + filter_len + 1U;
    size_t total = 0U;
    uint8_t *body =
        mqtt_begin_packet(out, cap, MESH_MQTT_SUBSCRIBE, MQTT_FLAGS_QOS1, body_len, &total);
    if (body == NULL) {
        return -ENOSPC;
    }

    mqtt_write_u16(body, packet_id);
    size_t at = 2U;
    at += mqtt_write_string(body + at, filter, filter_len);
    body[at] = 0U; /* the QoS asked for */
    return (int)total;
}

int mesh_mqtt_encode_empty(uint8_t *out, size_t cap, enum mesh_mqtt_packet_type type) {
    if (out == NULL) {
        return -EINVAL;
    }
    if (type != MESH_MQTT_PINGREQ && type != MESH_MQTT_DISCONNECT) {
        return -EINVAL;
    }
    size_t total = 0U;
    if (mqtt_begin_packet(out, cap, type, 0U, 0U, &total) == NULL) {
        return -ENOSPC;
    }
    return (int)total;
}

/* ------------------------------------------------------------------ topics */

bool mesh_mqtt_topic_is_publishable(const char *topic) {
    if (topic == NULL) {
        return false;
    }
    const size_t len = strlen(topic);
    if (len == 0U || len > MQTT_STRING_MAX) {
        return false;
    }
    for (size_t i = 0U; i < len; ++i) {
        if (topic[i] == '+' || topic[i] == '#') {
            return false;
        }
    }
    return true;
}
