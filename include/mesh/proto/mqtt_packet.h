#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MQTT 3.1.1 on the wire: the packets a client proxying for a radio needs, and nothing else.
 *
 * This is a codec, not a client. It knows how a CONNECT is laid out and how to find the topic in
 * a PUBLISH; it holds no socket, no state and no opinion about what should be sent when. That
 * belongs to src/core/mqtt_proxy.c, and keeping the two apart is what makes the fiddly half -
 * variable-length integers, length-prefixed strings, a flags nibble that is part of the packet
 * type for some packets and free for others - testable against byte arrays with no broker in
 * sight. It is the same split as stream_framing.c (the format) and stream_link.c (the link).
 *
 * **3.1.1, not 5.0.** The radio's own MQTT client speaks 3.1.1 and every broker still accepts
 * it, while 5.0 adds a property system to every packet that would be pure wire format for
 * something this never uses. A client proxy has to interoperate with whatever the firmware
 * would have connected to itself; matching the firmware's dialect is the whole job.
 *
 * **QoS 0 only, in both directions.** The firmware publishes at QoS 0, so nothing downstream is
 * expecting an acknowledgement, and the mesh underneath offers no delivery guarantee for a QoS 1
 * ledger to preserve. Honouring QoS 1 inbound would mean a PUBACK for a packet whose value has
 * expired by the time the radio hears it. The subscribe asks for 0 and a broker that grants more
 * is still only sent 0; a PUBLISH that arrives at QoS 1 or 2 is decoded (the packet id is in a
 * different place, so it must be) and then treated like any other.
 */

/* ------------------------------------------------------------------ packet types */

/*
 * The high nibble of byte 0. The ones this client never sends or receives are here anyway,
 * because the decoder has to be able to name what it just refused to handle.
 */
enum mesh_mqtt_packet_type {
    MESH_MQTT_PACKET_NONE = 0, /* 0 is reserved and always malformed */
    MESH_MQTT_CONNECT = 1,
    MESH_MQTT_CONNACK = 2,
    MESH_MQTT_PUBLISH = 3,
    MESH_MQTT_PUBACK = 4,
    MESH_MQTT_PUBREC = 5,
    MESH_MQTT_PUBREL = 6,
    MESH_MQTT_PUBCOMP = 7,
    MESH_MQTT_SUBSCRIBE = 8,
    MESH_MQTT_SUBACK = 9,
    MESH_MQTT_UNSUBSCRIBE = 10,
    MESH_MQTT_UNSUBACK = 11,
    MESH_MQTT_PINGREQ = 12,
    MESH_MQTT_PINGRESP = 13,
    MESH_MQTT_DISCONNECT = 14,
};

/*
 * What a broker said when it refused the connection. These are the 3.1.1 CONNACK codes, and
 * they are kept apart rather than collapsed into "rejected" because they ask the person holding
 * the device to do different things: 4 and 5 are a wrong password and a missing permission,
 * which are settings they can fix, and 3 is the broker having a bad day, which is not.
 */
enum mesh_mqtt_connack_code {
    MESH_MQTT_CONNACK_ACCEPTED = 0,
    MESH_MQTT_CONNACK_BAD_PROTOCOL = 1,
    MESH_MQTT_CONNACK_BAD_CLIENT_ID = 2,
    MESH_MQTT_CONNACK_UNAVAILABLE = 3,
    MESH_MQTT_CONNACK_BAD_CREDENTIALS = 4,
    MESH_MQTT_CONNACK_NOT_AUTHORISED = 5,
};

/* A SUBACK return code of 0x80 is the broker declining the filter outright. The others are the
   QoS it granted, which may be lower than the one asked for and never higher. */
#define MESH_MQTT_SUBACK_FAILURE 0x80U

/*
 * The largest remaining-length a variable-length integer can express, and the most bytes one
 * takes. Four bytes of seven bits each: a fifth continuation byte is malformed, not a bigger
 * number, which is the one bound that keeps a decoder off the end of a hostile buffer.
 */
#define MESH_MQTT_REMAINING_MAX 268435455UL
#define MESH_MQTT_REMAINING_BYTES_MAX 4U
/* Type byte plus the longest length that can follow it. */
#define MESH_MQTT_HEADER_MAX (1U + MESH_MQTT_REMAINING_BYTES_MAX)

/* ------------------------------------------------------------------ decoding */

/*
 * A packet's fixed header, decoded on its own.
 *
 * Deliberately separate from its body. A broker may send a retained message of any size it
 * likes, and one arriving that is larger than anything this client could forward must be
 * *skipped* - the bytes still have to come off the stream in order or every packet after it is
 * garbage. Reporting the length before the body exists is what lets the reader count those bytes
 * away without ever holding them, so the inbound buffer is sized for what a radio can accept
 * rather than for what a broker might send.
 */
struct mesh_mqtt_header {
    enum mesh_mqtt_packet_type type;
    /* The low nibble of byte 0. Part of the packet type for most packets - a SUBSCRIBE whose
       flags are not 0b0010 is malformed - and carries DUP/QoS/RETAIN for a PUBLISH. */
    uint8_t flags;
    size_t remaining;  /* body length, which may legitimately be 0 */
    size_t header_len; /* 2..5: what to skip to reach the body */
};

/*
 * Reads a fixed header out of the front of `data`.
 *
 * Returns the header length in bytes, 0 when `len` does not yet hold a whole one (call again
 * with more), or -EBADMSG when what is there cannot become one: a reserved packet type, a fifth
 * length byte, or flags that contradict the type. -EINVAL for bad arguments.
 *
 * A 0 return is *not* an error and must not be treated as one - it is the ordinary state of a
 * stream read that landed mid-packet.
 */
int mesh_mqtt_decode_header(const uint8_t *data, size_t len, struct mesh_mqtt_header *out);

/*
 * A PUBLISH taken apart. `topic` is NOT NUL-terminated and `payload` points into the buffer the
 * body came from, so both are only valid while that buffer is.
 */
struct mesh_mqtt_incoming {
    const char *topic;
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
    uint8_t qos;
    bool retained;
    bool duplicate;
    /* Present only at QoS 1 and 2, where it sits between the topic and the payload. Zero at
       QoS 0, where there is no field at all - not a packet id that happens to be zero. */
    uint16_t packet_id;
};

/*
 * Interprets a PUBLISH body. `flags` is the header's, since QoS and RETAIN live there rather
 * than in the body. Returns 0, or -EBADMSG when the body is too short for its own topic length,
 * when it claims a QoS of 3, or when the topic is empty - which 3.1.1 forbids, and which on a
 * stream is most likely the reader being at the wrong offset.
 *
 * On success `topic` and `payload` both point *into* `body`, always. Nothing is ever substituted
 * for an absent field, because a caller doing arithmetic against the body would then be doing it
 * against a different buffer.
 */
int mesh_mqtt_decode_publish(uint8_t flags, const uint8_t *body, size_t len,
                             struct mesh_mqtt_incoming *out);

/*
 * Interprets a CONNACK body: two bytes, a session-present flag and a return code. Returns 0, or
 * -EBADMSG when the body is not exactly two bytes or the reserved bits of the first are set.
 * `session_present` may be NULL.
 */
int mesh_mqtt_decode_connack(const uint8_t *body, size_t len, uint8_t *code, bool *session_present);

/*
 * Interprets a SUBACK body: a packet id and one return code per filter in the SUBSCRIBE it
 * answers. Only the first code is reported, because this client subscribes to one filter at a
 * time. Returns 0, or -EBADMSG when the body is shorter than an id and one code.
 */
int mesh_mqtt_decode_suback(const uint8_t *body, size_t len, uint16_t *packet_id, uint8_t *code);

/* ------------------------------------------------------------------ encoding */

/*
 * What goes in a CONNECT.
 *
 * No will. A last-will message is how a client that vanishes announces it, and this one has
 * nothing to announce: the thing that went away is a radio's link to a proxy, not a node, and
 * the mesh has its own idea of who is present that a retained MQTT message would only
 * contradict.
 *
 * `username` and `password` may be NULL or empty, and an empty username with a password set is
 * refused by the encoder rather than sent - 3.1.1 says a password without a username is
 * malformed, and brokers differ entertainingly about what they do with one.
 */
struct mesh_mqtt_connect {
    const char *client_id; /* required; may be empty only with clean_session */
    const char *username;  /* optional */
    const char *password;  /* optional, and only with a username */
    uint16_t keepalive_s;  /* 0 disables the broker's own timeout */
    bool clean_session;
};

/*
 * Each of these writes one whole packet - fixed header included - into `out` and returns its
 * length, or -ENOSPC when `cap` is too small, or -EINVAL for arguments that cannot be encoded
 * (a NULL client id, a string longer than 65535, a password with no username).
 *
 * Nothing here allocates and nothing here is partial: a call that returns an error wrote
 * nothing the caller should send.
 */
int mesh_mqtt_encode_connect(uint8_t *out, size_t cap, const struct mesh_mqtt_connect *params);

/*
 * A PUBLISH at QoS 0, which is why there is no packet id argument: at QoS 0 the field does not
 * exist in the packet at all.
 */
int mesh_mqtt_encode_publish(uint8_t *out, size_t cap, const char *topic, const uint8_t *payload,
                             size_t payload_len, bool retained);

/* A SUBSCRIBE for one filter at QoS 0. `packet_id` must be non-zero; 3.1.1 reserves 0. */
int mesh_mqtt_encode_subscribe(uint8_t *out, size_t cap, uint16_t packet_id, const char *filter);

/* PINGREQ and DISCONNECT, which are two bytes each and have no body. Returns -EINVAL for a type
   that is not one of those two. */
int mesh_mqtt_encode_empty(uint8_t *out, size_t cap, enum mesh_mqtt_packet_type type);

/* ------------------------------------------------------------------ topics */

/*
 * True when `topic` is one a broker will accept in a PUBLISH: non-empty, short enough for the
 * wire, no wildcard, and no embedded NUL. Wildcards are the one worth checking - `+` and `#`
 * are legal in a *filter* and forbidden in a published topic, and a broker's response to one is
 * to drop the connection rather than to say so.
 */
bool mesh_mqtt_topic_is_publishable(const char *topic);

#ifdef __cplusplus
}
#endif
