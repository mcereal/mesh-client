/*
 * The MQTT 3.1.1 codec, against bytes.
 *
 * Every case here is a byte array, deliberately. A codec tested only by round-tripping its own
 * output agrees with itself and can still be wrong about the wire - and the wire is the whole
 * point of this file, because the far end is somebody else's broker and the near end has to
 * match what the radio's own client would have sent. So the packets a broker sees are written
 * out here by hand, from the specification, and compared byte for byte.
 */

#include "framework/mesh_test.h"

#include "mesh/proto/mqtt_packet.h"

#include <errno.h>
#include <string.h>

static bool bytes_match(const uint8_t *got, size_t got_len, const uint8_t *want, size_t want_len) {
    return got_len == want_len && memcmp(got, want, want_len) == 0;
}

/* ------------------------------------------------------------------ CONNECT */

MESH_TEST_CASE(mqtt_connect_is_the_3_1_1_shape, unit) {
    struct mesh_mqtt_connect params;
    memset(&params, 0, sizeof params);
    params.client_id = "brick";
    params.keepalive_s = 60U;
    params.clean_session = true;

    uint8_t out[64];
    const int written = mesh_mqtt_encode_connect(out, sizeof out, &params);

    /* Fixed header, then "MQTT"/4, then flags=clean session, keepalive 60, then the id. */
    static const uint8_t want[] = {
        0x10U, 0x11U,                            /* CONNECT, 17 bytes to follow */
        0x00U, 0x04U, 'M', 'Q', 'T', 'T', 0x04U, /* protocol name and level */
        0x02U,                                   /* clean session, no credentials */
        0x00U, 0x3CU,                            /* keepalive: 60 seconds */
        0x00U, 0x05U, 'b', 'r', 'i', 'c', 'k',   /* client id */
    };
    if (written < 0 || !bytes_match(out, (size_t)written, want, sizeof want)) {
        record_failure(test_name, "a minimal CONNECT should be the shape 3.1.1 specifies");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_connect_carries_credentials, unit) {
    struct mesh_mqtt_connect params;
    memset(&params, 0, sizeof params);
    params.client_id = "b";
    params.username = "u";
    params.password = "p";
    params.keepalive_s = 0U;
    params.clean_session = true;

    uint8_t out[64];
    const int written = mesh_mqtt_encode_connect(out, sizeof out, &params);

    /* 0xC2: username, password, clean session. The two strings follow the client id, username
       first - the order is fixed by the specification, not by the flags. */
    static const uint8_t want[] = {
        0x10U, 0x13U, 0x00U, 0x04U, 'M',   'Q',   'T', 'T',   0x04U, 0xC2U, 0x00U,
        0x00U, 0x00U, 0x01U, 'b',   0x00U, 0x01U, 'u', 0x00U, 0x01U, 'p',
    };
    if (written < 0 || !bytes_match(out, (size_t)written, want, sizeof want)) {
        record_failure(test_name, "credentials should follow the client id, username first");
        return;
    }
    record_success(test_name);
}

/*
 * The two CONNECTs that cannot be encoded.
 *
 * Both are refusals the broker would otherwise make for us, and both would arrive as a CONNACK
 * code that means something else to whoever is reading it. A password with no username is
 * malformed per the specification and brokers disagree entertainingly about what they do with
 * one; an anonymous client id is only legal with a clean session, and a broker's "identifier
 * rejected" for it reads as a complaint about a name the user never chose.
 */
MESH_TEST_CASE(mqtt_connect_refuses_what_a_broker_would, unit) {
    uint8_t out[64];

    struct mesh_mqtt_connect orphan_password;
    memset(&orphan_password, 0, sizeof orphan_password);
    orphan_password.client_id = "b";
    orphan_password.password = "p";
    orphan_password.clean_session = true;
    if (mesh_mqtt_encode_connect(out, sizeof out, &orphan_password) != -EINVAL) {
        record_failure(test_name, "a password with no username should be refused");
        return;
    }

    struct mesh_mqtt_connect anonymous;
    memset(&anonymous, 0, sizeof anonymous);
    anonymous.client_id = "";
    anonymous.clean_session = false;
    if (mesh_mqtt_encode_connect(out, sizeof out, &anonymous) != -EINVAL) {
        record_failure(test_name, "an empty client id needs a clean session");
        return;
    }
    anonymous.clean_session = true;
    if (mesh_mqtt_encode_connect(out, sizeof out, &anonymous) <= 0) {
        record_failure(test_name, "an empty client id is legal with a clean session");
        return;
    }
    record_success(test_name);
}

/* ------------------------------------------------------------------ PUBLISH */

MESH_TEST_CASE(mqtt_publish_is_the_3_1_1_shape, unit) {
    static const uint8_t payload[] = {0xDEU, 0xADU};
    uint8_t out[32];
    const int written =
        mesh_mqtt_encode_publish(out, sizeof out, "msh/2/e", payload, sizeof payload, false);

    static const uint8_t want[] = {
        0x30U, 0x0BU,                                    /* PUBLISH, QoS 0, 11 to follow */
        0x00U, 0x07U, 'm', 's', 'h', '/', '2', '/', 'e', /* topic */
        0xDEU, 0xADU,                                    /* payload, no packet id at QoS 0 */
    };
    if (written < 0 || !bytes_match(out, (size_t)written, want, sizeof want)) {
        record_failure(test_name, "a QoS 0 PUBLISH should carry no packet id");
        return;
    }

    /* And the retain bit is the low bit of byte 0, not a byte of its own. */
    const int retained =
        mesh_mqtt_encode_publish(out, sizeof out, "msh/2/e", payload, sizeof payload, true);
    if (retained != written || out[0] != 0x31U) {
        record_failure(test_name, "retain should set the low bit of the type byte");
        return;
    }
    record_success(test_name);
}

/*
 * A PUBLISH at QoS 1 puts a packet id between the topic and the payload.
 *
 * This is the trap the decoder exists to avoid. A reader that takes the payload as "everything
 * after the topic" is right at QoS 0 and silently two bytes off at QoS 1 - the packet still
 * decodes, the topic is still right, and the bytes handed on are wrong. Nothing downstream can
 * tell, because a service envelope with two extra bytes on the front is just a protobuf that
 * fails to parse.
 */
MESH_TEST_CASE(mqtt_publish_finds_the_payload_at_every_qos, unit) {
    static const uint8_t qos0[] = {0x00U, 0x01U, 'a', 0xAAU, 0xBBU};
    static const uint8_t qos1[] = {0x00U, 0x01U, 'a', 0x12U, 0x34U, 0xAAU, 0xBBU};
    static const uint8_t want_payload[] = {0xAAU, 0xBBU};

    struct mesh_mqtt_incoming message;
    if (mesh_mqtt_decode_publish(0x00U, qos0, sizeof qos0, &message) != 0 ||
        !bytes_match(message.payload, message.payload_len, want_payload, sizeof want_payload) ||
        message.packet_id != 0U || message.qos != 0U) {
        record_failure(test_name, "a QoS 0 payload starts right after the topic");
        return;
    }

    if (mesh_mqtt_decode_publish(0x02U, qos1, sizeof qos1, &message) != 0 ||
        !bytes_match(message.payload, message.payload_len, want_payload, sizeof want_payload) ||
        message.packet_id != 0x1234U || message.qos != 1U) {
        record_failure(test_name, "a QoS 1 payload starts after the packet id");
        return;
    }
    if (message.topic_len != 1U || message.topic[0] != 'a') {
        record_failure(test_name, "the topic should be the same either way");
        return;
    }

    /* DUP and RETAIN come off the same nibble and neither moves the payload. */
    if (mesh_mqtt_decode_publish(0x09U, qos0, sizeof qos0, &message) != 0 || !message.retained ||
        !message.duplicate ||
        !bytes_match(message.payload, message.payload_len, want_payload, sizeof want_payload)) {
        record_failure(test_name, "DUP and RETAIN should not move the payload");
        return;
    }

    /* QoS 3 does not exist; where the payload starts is unanswerable, so it is refused. */
    if (mesh_mqtt_decode_publish(0x06U, qos1, sizeof qos1, &message) != -EBADMSG) {
        record_failure(test_name, "QoS 3 should be refused");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_publish_refuses_a_truncated_body, unit) {
    struct mesh_mqtt_incoming message;

    /* A topic length that runs past the end of the body it was read from. */
    static const uint8_t overrun[] = {0x00U, 0x40U, 'a'};
    if (mesh_mqtt_decode_publish(0x00U, overrun, sizeof overrun, &message) != -EBADMSG) {
        record_failure(test_name, "a topic longer than its body should be refused");
        return;
    }
    /* Too short to hold even the topic's length prefix. */
    static const uint8_t stub[] = {0x00U};
    if (mesh_mqtt_decode_publish(0x00U, stub, sizeof stub, &message) != -EBADMSG) {
        record_failure(test_name, "a body with no length prefix should be refused");
        return;
    }
    /* A QoS 1 publish whose body ends before its packet id. */
    static const uint8_t no_id[] = {0x00U, 0x01U, 'a', 0x12U};
    if (mesh_mqtt_decode_publish(0x02U, no_id, sizeof no_id, &message) != -EBADMSG) {
        record_failure(test_name, "a QoS 1 body with half a packet id should be refused");
        return;
    }

    /*
     * An empty *topic* is not legal - 3.1.1 requires at least one character - and refusing it is
     * also what keeps `topic` pointing inside the body it came from rather than at a substitute
     * from somewhere else. A fuzzer found the version that did not.
     */
    static const uint8_t no_topic[] = {0x00U, 0x00U, 0xAAU};
    if (mesh_mqtt_decode_publish(0x00U, no_topic, sizeof no_topic, &message) != -EBADMSG) {
        record_failure(test_name, "an empty topic should be refused");
        return;
    }

    /* An empty payload is legal and is not a truncation: a zero-byte message is how a retained
       one is cleared, and a broker will send one. */
    static const uint8_t empty[] = {0x00U, 0x01U, 'a'};
    if (mesh_mqtt_decode_publish(0x00U, empty, sizeof empty, &message) != 0 ||
        message.payload_len != 0U) {
        record_failure(test_name, "an empty payload is legal");
        return;
    }
    record_success(test_name);
}

/* ------------------------------------------------------------------ SUBSCRIBE */

MESH_TEST_CASE(mqtt_subscribe_is_flagged_qos1, unit) {
    uint8_t out[32];
    const int written = mesh_mqtt_encode_subscribe(out, sizeof out, 0x0001U, "msh/2/e/#");

    /*
     * 0x82, not 0x80. The 0b0010 nibble is required on a SUBSCRIBE by the specification, and a
     * broker that gets any other value is required to close the connection - which presents as
     * a link that dies immediately after CONNACK with nothing said about why.
     */
    static const uint8_t want[] = {
        0x82U, 0x0EU,                                              /* SUBSCRIBE, QoS 1 flags */
        0x00U, 0x01U,                                              /* packet id */
        0x00U, 0x09U, 'm', 's', 'h', '/', '2', '/', 'e', '/', '#', /* filter */
        0x00U,                                                     /* the QoS asked for */
    };
    if (written < 0 || !bytes_match(out, (size_t)written, want, sizeof want)) {
        record_failure(test_name, "a SUBSCRIBE should carry the 0b0010 flags nibble");
        return;
    }

    /* Packet id 0 is reserved, and a broker's SUBACK for one could not be matched to anything. */
    if (mesh_mqtt_encode_subscribe(out, sizeof out, 0U, "msh/#") != -EINVAL) {
        record_failure(test_name, "packet id 0 should be refused");
        return;
    }
    record_success(test_name);
}

/* ------------------------------------------------------------------ the fixed header */

MESH_TEST_CASE(mqtt_header_waits_for_every_byte, unit) {
    /* A PINGREQ and a CONNACK back to back, fed one byte at a time. */
    static const uint8_t stream[] = {0xC0U, 0x00U, 0x20U, 0x02U, 0x00U, 0x00U};
    struct mesh_mqtt_header header;

    for (size_t len = 0U; len < 2U; ++len) {
        if (mesh_mqtt_decode_header(stream, len, &header) != 0) {
            record_failure(test_name, "a partial header should report 'not yet', not an error");
            return;
        }
    }
    if (mesh_mqtt_decode_header(stream, 2U, &header) != 2 || header.type != MESH_MQTT_PINGREQ ||
        header.remaining != 0U) {
        record_failure(test_name, "a PINGREQ is two bytes and no body");
        return;
    }
    if (mesh_mqtt_decode_header(stream + 2, sizeof stream - 2U, &header) != 2 ||
        header.type != MESH_MQTT_CONNACK || header.remaining != 2U || header.header_len != 2U) {
        record_failure(test_name, "the next packet should start where the last one ended");
        return;
    }
    record_success(test_name);
}

/*
 * The remaining length is a variable-length integer, and every width boundary is a place a
 * decoder can be off by one packet.
 */
MESH_TEST_CASE(mqtt_header_reads_every_length_width, unit) {
    static const struct {
        uint8_t bytes[5];
        size_t len;
        size_t remaining;
        size_t header_len;
    } cases[] = {
        {{0x30U, 0x00U}, 2U, 0U, 2U},
        {{0x30U, 0x7FU}, 2U, 127U, 2U},
        {{0x30U, 0x80U, 0x01U}, 3U, 128U, 3U},
        {{0x30U, 0xFFU, 0x7FU}, 3U, 16383U, 3U},
        {{0x30U, 0x80U, 0x80U, 0x01U}, 4U, 16384U, 4U},
        {{0x30U, 0xFFU, 0xFFU, 0x7FU}, 4U, 2097151U, 4U},
        {{0x30U, 0x80U, 0x80U, 0x80U, 0x01U}, 5U, 2097152U, 5U},
        {{0x30U, 0xFFU, 0xFFU, 0xFFU, 0x7FU}, 5U, MESH_MQTT_REMAINING_MAX, 5U},
    };

    struct mesh_mqtt_header header;
    for (size_t i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        const int result = mesh_mqtt_decode_header(cases[i].bytes, cases[i].len, &header);
        if (result != (int)cases[i].header_len || header.remaining != cases[i].remaining) {
            record_failure(test_name, "a remaining length should read back at every width");
            return;
        }
    }

    /* A fifth continuation byte is not a larger number, it is a malformed packet - and it is
       the bound that stops an unterminated run of 0xFF walking off the buffer. */
    static const uint8_t overlong[] = {0x30U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x01U};
    if (mesh_mqtt_decode_header(overlong, sizeof overlong, &header) != -EBADMSG) {
        record_failure(test_name, "a five-byte remaining length should be refused");
        return;
    }
    record_success(test_name);
}

/*
 * The header checks that say the stream has gone wrong.
 *
 * These are cheap and they are the only detection a framed protocol gets: once the reader's idea
 * of where a packet starts has drifted, everything after it decodes as nonsense that looks
 * plausible. Refusing a reserved type or an illegal flags nibble turns that into one dropped
 * connection instead.
 */
MESH_TEST_CASE(mqtt_header_refuses_a_desynchronised_stream, unit) {
    struct mesh_mqtt_header header;
    static const struct {
        uint8_t bytes[2];
        const char *why;
    } refused[] = {
        {{0x00U, 0x00U}, "type 0 is reserved"},
        {{0xF0U, 0x00U}, "type 15 is reserved"},
        {{0x12U, 0x00U}, "CONNECT must have no flags"},
        {{0xC1U, 0x00U}, "PINGREQ must have no flags"},
        {{0x80U, 0x00U}, "SUBSCRIBE must be flagged 0b0010"},
        {{0x83U, 0x00U}, "SUBSCRIBE must be flagged exactly 0b0010"},
    };

    for (size_t i = 0U; i < sizeof refused / sizeof refused[0]; ++i) {
        if (mesh_mqtt_decode_header(refused[i].bytes, 2U, &header) != -EBADMSG) {
            record_failure(test_name, refused[i].why);
            return;
        }
    }

    /* A PUBLISH may carry anything in its nibble, which is what makes the check above safe to
       apply to every other type. */
    static const uint8_t publish[] = {0x3BU, 0x00U};
    if (mesh_mqtt_decode_header(publish, sizeof publish, &header) != 2 ||
        header.type != MESH_MQTT_PUBLISH || header.flags != 0x0BU) {
        record_failure(test_name, "a PUBLISH owns its whole flags nibble");
        return;
    }
    record_success(test_name);
}

/* ------------------------------------------------------------------ acknowledgements */

MESH_TEST_CASE(mqtt_acknowledgements_decode, unit) {
    uint8_t code = 0xFFU;
    bool session_present = true;

    static const uint8_t accepted[] = {0x00U, 0x00U};
    if (mesh_mqtt_decode_connack(accepted, sizeof accepted, &code, &session_present) != 0 ||
        code != MESH_MQTT_CONNACK_ACCEPTED || session_present) {
        record_failure(test_name, "an accepted CONNACK should decode");
        return;
    }
    static const uint8_t refused[] = {0x01U, 0x05U};
    if (mesh_mqtt_decode_connack(refused, sizeof refused, &code, &session_present) != 0 ||
        code != MESH_MQTT_CONNACK_NOT_AUTHORISED || !session_present) {
        record_failure(test_name, "a refusal should carry its reason");
        return;
    }
    /* A CONNACK is exactly two bytes; the reserved bits of the first are not free. */
    static const uint8_t reserved_set[] = {0x02U, 0x00U};
    if (mesh_mqtt_decode_connack(reserved_set, sizeof reserved_set, &code, NULL) != -EBADMSG ||
        mesh_mqtt_decode_connack(accepted, 3U, &code, NULL) != -EBADMSG) {
        record_failure(test_name, "a malformed CONNACK should be refused");
        return;
    }

    uint16_t packet_id = 0U;
    static const uint8_t granted[] = {0x00U, 0x07U, 0x00U};
    if (mesh_mqtt_decode_suback(granted, sizeof granted, &packet_id, &code) != 0 ||
        packet_id != 7U || code != 0U) {
        record_failure(test_name, "a SUBACK should carry the id it answers");
        return;
    }
    static const uint8_t declined[] = {0x00U, 0x07U, 0x80U};
    if (mesh_mqtt_decode_suback(declined, sizeof declined, &packet_id, &code) != 0 ||
        code != MESH_MQTT_SUBACK_FAILURE) {
        record_failure(test_name, "a declined filter should be readable as one");
        return;
    }
    record_success(test_name);
}

/* ------------------------------------------------------------------ bounds */

/*
 * Every encoder checks the whole packet against the buffer, not just its body.
 *
 * The distinction is the bug this pins: an encoder that wrote its fixed header first and checked
 * afterwards would leave a valid-looking header in a buffer with no room for what it describes,
 * and the caller would send it.
 */
MESH_TEST_CASE(mqtt_encoders_refuse_a_buffer_one_byte_short, unit) {
    uint8_t out[64];
    struct mesh_mqtt_connect params;
    memset(&params, 0, sizeof params);
    params.client_id = "brick";
    params.clean_session = true;

    const int connect_len = mesh_mqtt_encode_connect(out, sizeof out, &params);
    const int publish_len = mesh_mqtt_encode_publish(out, sizeof out, "a", NULL, 0U, false);
    const int subscribe_len = mesh_mqtt_encode_subscribe(out, sizeof out, 1U, "a");
    if (connect_len <= 0 || publish_len <= 0 || subscribe_len <= 0) {
        record_failure(test_name, "the packets should encode at all");
        return;
    }

    memset(out, 0xEEU, sizeof out);
    if (mesh_mqtt_encode_connect(out, (size_t)connect_len - 1U, &params) != -ENOSPC ||
        mesh_mqtt_encode_publish(out, (size_t)publish_len - 1U, "a", NULL, 0U, false) != -ENOSPC ||
        mesh_mqtt_encode_subscribe(out, (size_t)subscribe_len - 1U, 1U, "a") != -ENOSPC ||
        mesh_mqtt_encode_empty(out, 1U, MESH_MQTT_PINGREQ) != -ENOSPC) {
        record_failure(test_name, "one byte short should be refused, not truncated");
        return;
    }
    for (size_t i = 0U; i < sizeof out; ++i) {
        if (out[i] != 0xEEU) {
            record_failure(test_name, "a refused encode should not have written anything");
            return;
        }
    }
    record_success(test_name);
}

/*
 * A body that crosses 127 bytes grows its length prefix, and the packet has to grow with it.
 *
 * Getting this wrong is a one-byte overflow that only appears for payloads past a threshold no
 * small test would reach by accident - which is exactly why this one reaches it on purpose.
 */
MESH_TEST_CASE(mqtt_publish_grows_its_length_prefix, unit) {
    uint8_t payload[126];
    memset(payload, 0x5AU, sizeof payload);
    uint8_t out[160];

    /* topic "a" is 3 bytes on the wire, so 125 bytes of payload is a body of exactly 128. */
    const int written = mesh_mqtt_encode_publish(out, sizeof out, "a", payload, 125U, false);
    if (written != 1 + 2 + 128) {
        record_failure(test_name, "a 128-byte body needs a two-byte length");
        return;
    }
    struct mesh_mqtt_header header;
    if (mesh_mqtt_decode_header(out, (size_t)written, &header) != 3 || header.remaining != 128U) {
        record_failure(test_name, "the two-byte length should read back");
        return;
    }

    /* And one less stays inside a single byte. */
    const int smaller = mesh_mqtt_encode_publish(out, sizeof out, "a", payload, 124U, false);
    if (smaller != 1 + 1 + 127 || mesh_mqtt_decode_header(out, (size_t)smaller, &header) != 2 ||
        header.remaining != 127U) {
        record_failure(test_name, "a 127-byte body fits a one-byte length");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(mqtt_topics_reject_wildcards, unit) {
    if (!mesh_mqtt_topic_is_publishable("msh/2/e/LongFast/!abcd1234")) {
        record_failure(test_name, "an ordinary topic should be publishable");
        return;
    }
    /* Legal in a filter, forbidden in a published topic - and a broker's answer to one is to
       drop the connection rather than to say so, which reads as a flapping link. */
    if (mesh_mqtt_topic_is_publishable("msh/2/e/#") || mesh_mqtt_topic_is_publishable("msh/+/e") ||
        mesh_mqtt_topic_is_publishable("") || mesh_mqtt_topic_is_publishable(NULL)) {
        record_failure(test_name, "a wildcard or empty topic should not be publishable");
        return;
    }
    record_success(test_name);
}
