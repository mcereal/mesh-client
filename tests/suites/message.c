#define _POSIX_C_SOURCE 200809L

/* Encoding, ingesting and acking text messages. */

#include "framework/mesh_test.h"
#include "support/proto_fixture.h"

#include "mesh/core/message.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/*
 * Golden frame for the wire format, derived by hand from the .proto field numbers rather than
 * from our own encoder - the point is to notice if the encoding ever drifts.
 *
 * ToRadio.packet          field 1, LEN      -> 0x0A, len 0x12
 *   MeshPacket.to         field 2, FIXED32  -> 0x15, FF FF FF FF (broadcast)
 *   MeshPacket.decoded    field 4, LEN      -> 0x22, len 0x06
 *     Data.portnum        field 1, varint   -> 0x08, 0x01 (TEXT_MESSAGE_APP)
 *     Data.payload        field 2, LEN      -> 0x12, len 0x02, "hi"
 *   MeshPacket.id         field 6, FIXED32  -> 0x35, 2A 00 00 00
 * channel 0 and want_ack false are singular defaults and are omitted by nanopb.
 */
MESH_TEST_CASE(message_encode_text_golden, unit) {
    static const uint8_t k_expected[] = {0x0A, 0x12, 0x15, 0xFF, 0xFF, 0xFF, 0xFF,
                                         0x22, 0x06, 0x08, 0x01, 0x12, 0x02, 0x68,
                                         0x69, 0x35, 0x2A, 0x00, 0x00, 0x00};

    struct mesh_message_text_request request = {
        .dest = MESH_MESSAGE_BROADCAST_ADDR,
        .packet_id = 42U,
        .text = "hi",
        .channel = 0U,
        .hop_limit = 0U,
        .want_ack = false,
    };

    uint8_t buffer[64];
    size_t written = 0U;
    MESH_TEST_FAIL_IF(mesh_message_encode_text(&request, buffer, sizeof buffer, &written) != 0,
                      "encode failed");

    MESH_TEST_FAIL_IF(written != sizeof k_expected,
                      "encoded length does not match the golden frame");

    MESH_TEST_FAIL_IF(memcmp(buffer, k_expected, sizeof k_expected) != 0,
                      "encoded bytes do not match the golden frame");

    record_success(test_name);
}

MESH_TEST_CASE(message_encode_text_roundtrip, unit) {
    struct mesh_message_text_request request = {
        .dest = 0x433D1A2CU,
        .packet_id = 0x1234U,
        .text = "hello mesh",
        .channel = 2U,
        .hop_limit = 3U,
        .want_ack = true,
    };

    uint8_t buffer[256];
    size_t written = 0U;
    MESH_TEST_FAIL_IF(mesh_message_encode_text(&request, buffer, sizeof buffer, &written) != 0,
                      "encode failed");

    meshtastic_ToRadio decoded = meshtastic_ToRadio_init_default;
    pb_istream_t stream = pb_istream_from_buffer(buffer, written);
    MESH_TEST_FAIL_IF(!pb_decode(&stream, meshtastic_ToRadio_fields, &decoded), "decode failed");

    MESH_TEST_FAIL_IF(decoded.which_payload_variant != meshtastic_ToRadio_packet_tag,
                      "expected a packet variant");

    const meshtastic_MeshPacket *packet = &decoded.packet;
    MESH_TEST_FAIL_IF(packet->to != request.dest || packet->id != request.packet_id ||
                          packet->channel != request.channel ||
                          packet->hop_limit != request.hop_limit || !packet->want_ack,
                      "packet header fields did not survive the roundtrip");

    /* The firmware stamps `from` itself; a client must not claim a node number. */
    MESH_TEST_FAIL_IF(packet->from != 0U, "from should be left for the firmware to fill in");

    MESH_TEST_FAIL_IF(packet->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
                          packet->decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP,
                      "expected a decoded TEXT_MESSAGE_APP payload");

    MESH_TEST_FAIL_IF(
        packet->decoded.payload.size != strlen(request.text) ||
            memcmp(packet->decoded.payload.bytes, request.text, strlen(request.text)) != 0,
        "payload text mismatch");

    record_success(test_name);
}

MESH_TEST_CASE(message_encode_text_limits, unit) {
    uint8_t buffer[512];
    size_t written = 0U;

    struct mesh_message_text_request request = {
        .dest = MESH_MESSAGE_BROADCAST_ADDR,
        .packet_id = 1U,
        .text = "",
        .channel = 0U,
        .hop_limit = 0U,
        .want_ack = false,
    };

    MESH_TEST_FAIL_IF(mesh_message_encode_text(&request, buffer, sizeof buffer, &written) !=
                          -EINVAL,
                      "empty text should be rejected");

    char oversized[MESH_MESSAGE_TEXT_MAX + 8U];
    memset(oversized, 'a', sizeof oversized - 1U);
    oversized[sizeof oversized - 1U] = '\0';
    request.text = oversized;
    MESH_TEST_FAIL_IF(mesh_message_encode_text(&request, buffer, sizeof buffer, &written) !=
                          -EMSGSIZE,
                      "oversized text should be rejected with -EMSGSIZE");

    record_success(test_name);
}

MESH_TEST_CASE(message_log_ring, unit) {
    struct mesh_message_log log;
    mesh_message_log_reset(&log);

    /* Overfill by four so eviction, ordering and the dropped counter all get exercised. */
    const size_t total = MESH_MESSAGE_LOG_CAPACITY + 4U;
    for (size_t i = 0; i < total; ++i) {
        struct mesh_message message;
        memset(&message, 0, sizeof(message));
        message.packet_id = (uint32_t)(i + 1U);
        snprintf(message.text, sizeof(message.text), "message %zu", i);
        MESH_TEST_FAIL_IF(mesh_message_log_append(&log, &message) == NULL, "append failed");
    }

    MESH_TEST_FAIL_IF(log.count != MESH_MESSAGE_LOG_CAPACITY, "log should saturate at capacity");

    MESH_TEST_FAIL_IF(log.dropped != 4U, "dropped counter should report the evicted entries");

    /* Index 0 is the oldest surviving entry, which is the fifth one we appended. */
    const struct mesh_message *oldest = mesh_message_log_at(&log, 0U);
    MESH_TEST_FAIL_IF(oldest == NULL || oldest->packet_id != 5U, "oldest surviving entry is wrong");

    const struct mesh_message *newest = mesh_message_log_at(&log, log.count - 1U);
    MESH_TEST_FAIL_IF(newest == NULL || newest->packet_id != (uint32_t)total,
                      "newest entry is wrong");

    MESH_TEST_FAIL_IF(mesh_message_log_at(&log, log.count) != NULL,
                      "out-of-range index should return NULL");

    MESH_TEST_FAIL_IF(mesh_message_log_find(&log, 1U) != NULL,
                      "an evicted packet id should not be found");

    record_success(test_name);
}

MESH_TEST_CASE(message_ingest_text, unit) {
    struct mesh_message_log log;
    mesh_message_log_reset(&log);

    /* Control bytes are what a hostile or buggy node can put on the wire; they must not reach
       a framebuffer or a log line intact. */
    const char payload[] = "hi\nthere\x01!";
    meshtastic_MeshPacket packet = mesh_test_make_decoded_packet(
        0x11111111U, 0x22222222U, 3U, 77U, meshtastic_PortNum_TEXT_MESSAGE_APP, payload,
        sizeof(payload) - 1U);
    packet.has_rx_time = true;
    packet.rx_time = 1234U;
    packet.rx_snr = 5.5F;

    MESH_TEST_FAIL_IF(mesh_message_ingest(&log, &packet, 0x22222222U) != 1,
                      "a text packet should be appended");

    const struct mesh_message *message = mesh_message_log_at(&log, 0U);
    MESH_TEST_FAIL_IF(message == NULL, "appended message not readable");

    MESH_TEST_FAIL_IF(message->direction != MESH_MESSAGE_INBOUND,
                      "a packet from another node is inbound");

    MESH_TEST_FAIL_IF(message->from != 0x11111111U || message->to != 0x22222222U ||
                          message->channel != 3U || message->packet_id != 77U ||
                          message->rx_time != 1234U,
                      "packet metadata did not survive ingest");

    MESH_TEST_FAIL_IF(strcmp(message->text, "hi there?!") != 0,
                      "control bytes should be folded to space and '?'");

    record_success(test_name);
}

MESH_TEST_CASE(message_ingest_ignores_other_payloads, unit) {
    struct mesh_message_log log;
    mesh_message_log_reset(&log);

    meshtastic_MeshPacket position = mesh_test_make_decoded_packet(
        1U, 2U, 0U, 5U, meshtastic_PortNum_POSITION_APP, "\x01\x02", 2U);
    MESH_TEST_FAIL_IF(mesh_message_ingest(&log, &position, 2U) != 0 || log.count != 0U,
                      "a non-text portnum should add nothing");

    /* We hold no channel keys, so an encrypted packet is opaque and must be skipped rather
       than parsed as though its bytes were a Data message. */
    meshtastic_MeshPacket encrypted = meshtastic_MeshPacket_init_default;
    encrypted.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
    encrypted.encrypted.size = 4U;
    memcpy(encrypted.encrypted.bytes, "\xDE\xAD\xBE\xEF", 4U);
    MESH_TEST_FAIL_IF(mesh_message_ingest(&log, &encrypted, 2U) != 0 || log.count != 0U,
                      "an encrypted packet should add nothing");

    MESH_TEST_FAIL_IF(mesh_message_ingest(NULL, &position, 2U) != -EINVAL ||
                          mesh_message_ingest(&log, NULL, 2U) != -EINVAL,
                      "NULL arguments should be rejected");

    record_success(test_name);
}

MESH_TEST_CASE(message_ingest_echo_is_not_duplicated, unit) {
    struct mesh_message_log log;
    mesh_message_log_reset(&log);

    const uint32_t my_node = 0xAABBCCDDU;

    /* What the transport records locally when it queues a send. */
    struct mesh_message sent;
    memset(&sent, 0, sizeof(sent));
    sent.packet_id = 99U;
    sent.from = my_node;
    sent.to = 0x1234U;
    sent.direction = MESH_MESSAGE_OUTBOUND;
    sent.ack = MESH_MESSAGE_ACK_PENDING;
    snprintf(sent.text, sizeof(sent.text), "ping");
    mesh_message_log_append(&log, &sent);

    /* The radio echoes the same packet back to us over FromRadio. */
    meshtastic_MeshPacket echo = mesh_test_make_decoded_packet(
        my_node, 0x1234U, 0U, 99U, meshtastic_PortNum_TEXT_MESSAGE_APP, "ping", 4U);
    echo.has_rx_time = true;
    echo.rx_time = 4242U;

    MESH_TEST_FAIL_IF(mesh_message_ingest(&log, &echo, my_node) != 0,
                      "an echo should not be reported as a new message");

    MESH_TEST_FAIL_IF(log.count != 1U, "the echo should not create a second entry");

    const struct mesh_message *message = mesh_message_log_at(&log, 0U);
    MESH_TEST_FAIL_IF(message == NULL || message->rx_time != 4242U,
                      "the echo should refresh the existing entry");

    MESH_TEST_FAIL_IF(message->ack != MESH_MESSAGE_ACK_PENDING,
                      "an echo is not a delivery confirmation");

    record_success(test_name);
}

MESH_TEST_CASE(message_routing_ack, unit) {
    struct mesh_message_log log;
    mesh_message_log_reset(&log);

    struct mesh_message sent;
    memset(&sent, 0, sizeof(sent));
    sent.packet_id = 321U;
    sent.direction = MESH_MESSAGE_OUTBOUND;
    sent.ack = MESH_MESSAGE_ACK_PENDING;
    snprintf(sent.text, sizeof(sent.text), "are you there");
    mesh_message_log_append(&log, &sent);

    meshtastic_MeshPacket ack = mesh_test_make_routing_reply(321U, meshtastic_Routing_Error_NONE);
    MESH_TEST_FAIL_IF(mesh_message_ingest(&log, &ack, 2U) != 0,
                      "a routing reply adds no message of its own");

    const struct mesh_message *message = mesh_message_log_at(&log, 0U);
    MESH_TEST_FAIL_IF(message == NULL || message->ack != MESH_MESSAGE_ACK_DELIVERED,
                      "a NONE error reason means delivered");

    /* And a failure reason marks the same message failed, carrying the reason through. */
    struct mesh_message second;
    memset(&second, 0, sizeof(second));
    second.packet_id = 322U;
    second.direction = MESH_MESSAGE_OUTBOUND;
    second.ack = MESH_MESSAGE_ACK_PENDING;
    snprintf(second.text, sizeof(second.text), "still there");
    mesh_message_log_append(&log, &second);

    meshtastic_MeshPacket nak =
        mesh_test_make_routing_reply(322U, meshtastic_Routing_Error_NO_RESPONSE);
    MESH_TEST_FAIL_IF(mesh_message_ingest(&log, &nak, 2U) != 0,
                      "a routing failure adds no message of its own");

    const struct mesh_message *failed = mesh_message_log_at(&log, 1U);
    MESH_TEST_FAIL_IF(failed == NULL || failed->ack != MESH_MESSAGE_ACK_FAILED ||
                          failed->ack_error != (uint8_t)meshtastic_Routing_Error_NO_RESPONSE,
                      "an error reason should mark the message failed");

    /* A reply naming a message we never sent must not disturb anything. */
    meshtastic_MeshPacket stray =
        mesh_test_make_routing_reply(9999U, meshtastic_Routing_Error_NONE);
    MESH_TEST_FAIL_IF(mesh_message_ingest(&log, &stray, 2U) != 0 || log.count != 2U,
                      "an unmatched routing reply should be ignored");

    record_success(test_name);
}

/* Message text reaches --status --json, and JSON must be valid UTF-8, so malformed sequences
   from the radio have to be replaced rather than copied through. */
MESH_TEST_CASE(message_ingest_invalid_utf8, unit) {
    struct {
        const char *label;
        const char *payload;
        size_t payload_len;
        const char *expected;
    } cases[] = {
        /* Well-formed multi-byte characters survive intact. */
        {"two-byte", "caf\xC3\xA9", 5U, "caf\xC3\xA9"},
        {"three-byte", "\xE2\x82\xAC", 3U, "\xE2\x82\xAC"},
        {"four-byte", "\xF0\x9F\x93\xA1", 4U, "\xF0\x9F\x93\xA1"},
        /* Bytes that can never lead a sequence. */
        /* Split so the hex escape does not swallow the trailing 'b' as another hex digit. */
        {"invalid-lead",
         "a\xFF"
         "b",
         3U, "a?b"},
        {"stray-continuation", "a\x80\x62", 3U, "a?b"},
        /* Truncated: a valid lead with the continuation bytes missing. */
        {"truncated", "a\xE2\x82", 3U, "a??"},
        /* Overlong encoding of '/' - the classic path-traversal smuggling trick. */
        {"overlong", "\xC0\xAF", 2U, "??"},
        /* UTF-16 surrogate half, not a valid character in UTF-8. */
        {"surrogate", "\xED\xA0\x80", 3U, "???"},
        /* Above U+10FFFF. */
        {"out-of-range", "\xF4\x90\x80\x80", 4U, "????"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct mesh_message_log log;
        mesh_message_log_reset(&log);

        meshtastic_MeshPacket packet = mesh_test_make_decoded_packet(
            1U, 2U, 0U, (uint32_t)(i + 1U), meshtastic_PortNum_TEXT_MESSAGE_APP, cases[i].payload,
            cases[i].payload_len);

        MESH_TEST_FAIL_IF(mesh_message_ingest(&log, &packet, 2U) != 1, cases[i].label);

        const struct mesh_message *message = mesh_message_log_at(&log, 0U);
        MESH_TEST_FAIL_IF(message == NULL || strcmp(message->text, cases[i].expected) != 0,
                          cases[i].label);
    }

    record_success(test_name);
}

/* A failed delivery has to say which failure it was: "!!" alone sends the user looking in the
   wrong place, and the reasons call for completely different fixes. */
MESH_TEST_CASE(message_routing_failure_reason, unit) {
    struct mesh_message_log log;
    mesh_message_log_reset(&log);

    struct mesh_message sent;
    memset(&sent, 0, sizeof sent);
    sent.packet_id = 4242U;
    sent.direction = MESH_MESSAGE_OUTBOUND;
    sent.to = 0x9E9D0AD8U;
    sent.ack = (uint8_t)MESH_MESSAGE_ACK_PENDING;
    snprintf(sent.text, sizeof sent.text, "%s", "test54");
    mesh_message_log_append(&log, &sent);

    /* What the firmware sends back when nothing acked the packet. */
    meshtastic_Routing routing = meshtastic_Routing_init_default;
    routing.which_variant = meshtastic_Routing_error_reason_tag;
    routing.error_reason = meshtastic_Routing_Error_MAX_RETRANSMIT;

    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_default;
    packet.from = 0x9E9D0AD8U;
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum = meshtastic_PortNum_ROUTING_APP;
    packet.decoded.request_id = 4242U;
    pb_ostream_t stream =
        pb_ostream_from_buffer(packet.decoded.payload.bytes, sizeof packet.decoded.payload.bytes);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Routing_fields, &routing),
                      "failed to encode the routing reply");
    packet.decoded.payload.size = (pb_size_t)stream.bytes_written;

    mesh_message_ingest(&log, &packet, 0x11111111U);

    const struct mesh_message *entry = mesh_message_log_find(&log, 4242U);
    MESH_TEST_FAIL_IF(entry == NULL || entry->ack != (uint8_t)MESH_MESSAGE_ACK_FAILED,
                      "the message should be marked failed");
    MESH_TEST_FAIL_IF(entry->ack_error != (uint8_t)meshtastic_Routing_Error_MAX_RETRANSMIT,
                      "the routing error should be kept");
    MESH_TEST_FAIL_IF(
        strcmp(mesh_message_ack_error_to_string(entry->ack_error), "no ack after retries") != 0,
        "the reason should be named");
    /* An unknown code from a newer firmware still has to render as something. */
    MESH_TEST_FAIL_IF(strcmp(mesh_message_ack_error_to_string(200U), "unknown error") != 0,
                      "an unrecognised reason should still say something");

    record_success(test_name);
}
