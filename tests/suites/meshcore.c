/*
 * MeshCore's companion protocol: the framing a serial link wraps it in, the codec, and the
 * conversation that fills the session's model.
 *
 * The SELF_INFO and DEVICE_INFO frames are the ones a Heltec V3 on companion-v1.17.1 sent over
 * BLE, byte for byte (support/meshcore_fixture.c); the rest are built to the firmware's layouts
 * (examples/companion_radio/MyMesh.cpp).
 */

#include "framework/mesh_test.h"
#include "support/meshcore_fixture.h"
#include "support/session_fixture.h"

#include "inkwell/base/time.h"
#include "mesh/core/meshcore.h"
#include "mesh/core/meshcore_backup.h"
#include "mesh/core/message.h"
#include "mesh/core/radio_backup.h"
#include "mesh/core/radio_profile.h"
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"
#include "mesh/proto/ble_profile.h"
#include "mesh/proto/stream_framing.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The recorded frames, the fake link and the contact builder are support/meshcore_fixture.c. */

static struct mesh_session g_model;
static struct mesh_meshcore g_meshcore;

static void feed(const struct mesh_protocol *protocol, const uint8_t *frame, size_t len) {
    mesh_protocol_receive(protocol, frame, len);
}

static void feed_code(const struct mesh_protocol *protocol, uint8_t code) {
    feed(protocol, &code, 1U);
}

/* A handshake to READY on a fresh model: one known contact ("Alice", key 0x40...), one channel. */
static bool handshake(struct mesh_protocol *protocol, struct mesh_test_meshcore_wire *wire) {
    mesh_session_init(&g_model);
    mesh_meshcore_init(&g_meshcore, &g_model);
    *protocol = mesh_meshcore_protocol(&g_meshcore);
    return mesh_test_meshcore_sync(&g_meshcore, protocol, wire);
}

static const struct mesh_node_summary *model_node(uint32_t id) {
    for (size_t i = 0; i < g_model.handshake.node_count; ++i) {
        if (g_model.handshake.nodes[i].node_id == id) {
            return &g_model.handshake.nodes[i];
        }
    }
    return NULL;
}

static const struct mesh_message *newest_message(void) {
    const size_t count = g_model.messages.count;
    return count > 0U ? mesh_message_log_at(&g_model.messages, count - 1U) : NULL;
}

/* ------------------------------------------------------------------ framing */

struct collected {
    uint8_t frames[4][MESH_MESHCORE_FRAME_MAX_PAYLOAD];
    size_t lens[4];
    size_t count;
    size_t junk;
};

static void collect_frame(const uint8_t *payload, size_t len, void *ctx) {
    struct collected *out = ctx;
    if (out->count < 4U) {
        memcpy(out->frames[out->count], payload, len);
        out->lens[out->count++] = len;
    }
}

static void collect_text(const uint8_t *text, size_t len, void *ctx) {
    (void)text;
    ((struct collected *)ctx)->junk += len;
}

/*
 * '>' and a little-endian length, the other way round from Meshtastic's big-endian 0x94 0xC3:
 * a frame split across reads is held until it is whole, and a '>' in the debug text around it
 * with a length no frame has is resynced past rather than swallowing what follows.
 */
MESH_TEST_CASE(meshcore_framing_parses_split_frames_among_junk, unit) {
    struct mesh_stream_parser parser;
    mesh_stream_parser_reset(&parser);
    struct collected got;
    memset(&got, 0, sizeof got);
    const struct mesh_stream_parser_callbacks callbacks = {collect_frame, collect_text, &got};

    const uint8_t stream[] = {'b', 'o', 'o', 't', '>', 0xFF, 0xFF, '>', 0x02, 0x00, 0x0A};
    const uint8_t rest[] = {0x0B, '>', 0x01, 0x00, 0x83};
    mesh_stream_framing_meshcore.push(&parser, stream, sizeof stream, &callbacks);
    MESH_TEST_FAIL_IF(got.count != 0U, "half a frame is held, not delivered");
    mesh_stream_framing_meshcore.push(&parser, rest, sizeof rest, &callbacks);
    MESH_TEST_FAIL_IF(got.count != 2U, "both frames arrive once whole");
    MESH_TEST_FAIL_IF(got.lens[0] != 2U || got.frames[0][0] != 0x0A || got.frames[0][1] != 0x0B,
                      "the first frame is the two bytes its length names");
    MESH_TEST_FAIL_IF(got.lens[1] != 1U || got.frames[1][0] != 0x83, "the second is one push");
    MESH_TEST_FAIL_IF(got.junk != 7U, "the text and the impossible header are junk");

    uint8_t out[16];
    size_t written = 0U;
    const uint8_t payload[] = {0x16, 0x03};
    MESH_TEST_FAIL_IF(mesh_stream_framing_meshcore.encode(payload, sizeof payload, out, sizeof out,
                                                          &written) != 0 ||
                          written != 5U || out[0] != '<' || out[1] != 2U || out[2] != 0U ||
                          out[3] != 0x16,
                      "a command goes out as '<', the length low byte first, the frame");
    uint8_t big[MESH_MESHCORE_FRAME_MAX_PAYLOAD + 1U];
    memset(big, 0, sizeof big);
    uint8_t big_out[sizeof big + 8U];
    MESH_TEST_FAIL_IF(mesh_stream_framing_meshcore.encode(big, sizeof big, big_out, sizeof big_out,
                                                          &written) != -EMSGSIZE,
                      "a frame past the firmware's MAX_FRAME_SIZE is refused");
    MESH_TEST_FAIL_IF(mesh_stream_framing_meshcore.wake_byte != -1,
                      "MeshCore's receiver needs no wake burst");
    MESH_TEST_FAIL_IF(!mesh_ble_profile_usable(&mesh_ble_profile_meshcore),
                      "the Nordic UART profile is one a link can carry");
    record_success(test_name);
}

/* ------------------------------------------------------------------ codec */

MESH_TEST_CASE(meshcore_decodes_what_a_heltec_sent, unit) {
    struct mesh_meshcore_device_info device;
    MESH_TEST_FAIL_IF(mesh_meshcore_decode_device_info(mesh_test_meshcore_device_info,
                                                       sizeof mesh_test_meshcore_device_info,
                                                       &device) != 0,
                      "DEVICE_INFO decodes");
    MESH_TEST_FAIL_IF(device.firmware_version != 13U || device.max_contacts != 350U ||
                          device.max_channels != 40U || device.ble_pin != 632090U,
                      "version 13, 350 contacts, 40 channels, the fixed PIN");
    MESH_TEST_FAIL_IF(strcmp(device.model, "Heltec V3") != 0 ||
                          strcmp(device.version, "v1.17.1-d929643") != 0 ||
                          strcmp(device.build_date, "14-Aug-2026") != 0,
                      "the fixed-width strings stop at their padding");

    struct mesh_meshcore_self_info self;
    MESH_TEST_FAIL_IF(mesh_meshcore_decode_self_info(mesh_test_meshcore_self_info,
                                                     sizeof mesh_test_meshcore_self_info,
                                                     &self) != 0,
                      "SELF_INFO decodes");
    MESH_TEST_FAIL_IF(self.adv_type != MESH_MESHCORE_ADV_CHAT || self.tx_power_dbm != 22U ||
                          self.public_key[0] != 0xb8 || self.public_key[31] != 0x58,
                      "a chat node at 22 dBm with its key in place");
    MESH_TEST_FAIL_IF(self.frequency_khz != 910525U || self.bandwidth_hz != 62500U ||
                          self.spreading_factor != 7U || self.coding_rate != 5U,
                      "the radio settings are kHz, Hz, SF and CR");
    MESH_TEST_FAIL_IF(strcmp(self.name, "MPBC") != 0, "the name runs to the end, unterminated");
    MESH_TEST_FAIL_IF(mesh_meshcore_node_id(self.public_key, 32U) != 0xb8da09b0U,
                      "a node's number is its key's first four bytes, big-endian");
    MESH_TEST_FAIL_IF(mesh_meshcore_decode_self_info(mesh_test_meshcore_self_info, 20U, &self) !=
                          -EBADMSG,
                      "a short SELF_INFO is refused, not read past");
    record_success(test_name);
}

/*
 * Four shapes of one message: direct or channel, and before or after app version 3 put an SNR
 * in front. The radio encodes each as it arrives against whatever the last app asked for, so a
 * queue drained on connect can hold both.
 */
MESH_TEST_CASE(meshcore_decodes_every_message_shape, unit) {
    const uint8_t v3_direct[] = {16, 0xF6, 0, 0,    1,    2,    3,    4,   5,
                                 6,  0x02, 0, 0x10, 0x20, 0x30, 0x40, 'h', 'i'};
    struct mesh_meshcore_message message;
    MESH_TEST_FAIL_IF(mesh_meshcore_decode_message(v3_direct, sizeof v3_direct, &message) != 0,
                      "a V3 direct message decodes");
    MESH_TEST_FAIL_IF(message.channel || !message.has_snr || message.snr_q4 != -10 ||
                          message.sender_prefix[5] != 6 || message.path_len != 2U ||
                          message.timestamp != 0x40302010U || message.text_len != 2U ||
                          memcmp(message.text, "hi", 2U) != 0,
                      "SNR x4, the six-byte prefix, two hops, the text");

    const uint8_t v1_channel[] = {8, 3, 0xFF, 0, 1, 0, 0, 0, 'A', ':', ' ', 'x'};
    MESH_TEST_FAIL_IF(mesh_meshcore_decode_message(v1_channel, sizeof v1_channel, &message) != 0,
                      "a pre-V3 channel message decodes");
    MESH_TEST_FAIL_IF(!message.channel || message.has_snr || message.channel_index != 3U ||
                          message.path_len != MESH_MESHCORE_PATH_NONE || message.text_len != 4U,
                      "channel 3, no SNR, came direct");

    const uint8_t signed_room[] = {7, 1, 2, 3,    4,    5,    6,    0,   2,  0,
                                   0, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD, 'y', 'o'};
    MESH_TEST_FAIL_IF(mesh_meshcore_decode_message(signed_room, sizeof signed_room, &message) != 0,
                      "a room server's signed post decodes");
    MESH_TEST_FAIL_IF(!message.has_author || message.author_prefix[0] != 0xAA ||
                          message.text_len != 2U || message.text[0] != 'y',
                      "the author's four bytes come off the front of the text");

    MESH_TEST_FAIL_IF(mesh_meshcore_decode_message(v3_direct, 9U, &message) != -EBADMSG,
                      "a message cut short is refused");
    record_success(test_name);
}

MESH_TEST_CASE(meshcore_encodes_commands_to_layout, unit) {
    uint8_t out[MESH_MESHCORE_MAX_FRAME];
    const uint8_t prefix[6] = {1, 2, 3, 4, 5, 6};
    const int len = mesh_meshcore_encode_text(prefix, MESH_MESHCORE_TXT_PLAIN, 1U, 0x01020304U,
                                              "hey", out, sizeof out);
    MESH_TEST_FAIL_IF(len != 16, "code, type, attempt, timestamp, prefix, text");
    MESH_TEST_FAIL_IF(out[0] != MESH_MESHCORE_CMD_SEND_TXT_MSG || out[1] != 0U || out[2] != 1U ||
                          out[3] != 0x04 || out[6] != 0x01 || out[7] != 1U || out[12] != 6U ||
                          memcmp(out + 13, "hey", 3U) != 0,
                      "little-endian timestamp, then the prefix, then the text unterminated");

    char long_text[MESH_MESHCORE_TEXT_MAX + 2U];
    memset(long_text, 'x', sizeof long_text - 1U);
    long_text[sizeof long_text - 1U] = '\0';
    MESH_TEST_FAIL_IF(mesh_meshcore_encode_channel_text(0U, 0U, long_text, out, sizeof out) !=
                          -EMSGSIZE,
                      "text past the firmware's MAX_TEXT_LEN is refused rather than truncated");

    MESH_TEST_FAIL_IF(mesh_meshcore_encode_app_start("MeshClient", out, sizeof out) != 18 ||
                          out[0] != 1U || out[7] != 0U || memcmp(out + 8, "MeshClient", 10U) != 0,
                      "APP_START is seven reserved bytes then the name");
    record_success(test_name);
}

/* ------------------------------------------------------------------ conversation */

/*
 * One command on the wire at a time, each answer queuing the next, and what the walk learned
 * lands in the session's model: this radio as the roster owner, the contact with its key and
 * position, the channel, and a completed sync the screens key off.
 */
MESH_TEST_CASE(meshcore_handshake_fills_the_model, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");

    const struct mesh_handshake_status *status = &g_model.handshake;
    MESH_TEST_FAIL_IF(!status->config_complete || status->request_in_flight,
                      "the model's sync is complete");
    MESH_TEST_FAIL_IF(!status->has_my_info || status->my_info.my_node_num != 0xb8da09b0U ||
                          mesh_session_roster_owner(&g_model) != 0xb8da09b0U,
                      "this radio owns the roster");
    MESH_TEST_FAIL_IF(!mesh_session_attached(&g_model), "the model reads as linked");

    const struct mesh_node_summary *alice = model_node(0x40414243U);
    MESH_TEST_FAIL_IF(alice == NULL, "the contact is in the roster under its key's number");
    MESH_TEST_FAIL_IF(strcmp(alice->long_name, "Alice") != 0 ||
                          strcmp(alice->short_name, "Alic") != 0 || !alice->has_user,
                      "named by its advert, the short name four bytes of it");
    MESH_TEST_FAIL_IF(alice->public_key_len != 32U || alice->public_key[31] != 0x40 + 31 ||
                          !alice->in_nodedb || !alice->has_hops_away || alice->hops_away != 2U,
                      "its key, on the radio, two hops out");
    MESH_TEST_FAIL_IF(!alice->position.valid || alice->position.latitude_i != 377749000,
                      "the advert's microdegrees become the roster's 1e-7");
    MESH_TEST_FAIL_IF(alice->last_heard != 1700000000U, "heard at the radio's lastmod");

    MESH_TEST_FAIL_IF(status->channel_count != 1U ||
                          strcmp(status->channels[0].name, "Public") != 0 ||
                          status->channels[0].role != 1U,
                      "slot 0 is the primary channel, by name");

    /* The session's own sends are refused while MeshCore has the link. */
    MESH_TEST_FAIL_IF(mesh_session_send_heartbeat(&g_model) == 0,
                      "nothing Meshtastic's reaches a MeshCore radio");

    mesh_protocol_detach(&protocol);
    MESH_TEST_FAIL_IF(mesh_session_attached(&g_model) || mesh_meshcore_ready(&g_meshcore),
                      "detach takes the link from both");
    record_success(test_name);
}

/*
 * Seen on a Heltec V3 just switched to MeshCore over its cable: the radio formatted its flash for
 * longer than a command waits, the link was opened again, and the radio answered both
 * connections' DEVICE_QUERY and APP_START at once. The second pair must not be taken as the
 * answers to what followed, or the handshake's END_OF_CONTACTS finds nothing outstanding and the
 * sync never finishes.
 */
MESH_TEST_CASE(meshcore_handshake_ignores_a_repeated_device_and_self_info, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    mesh_session_init(&g_model);
    mesh_meshcore_init(&g_meshcore, &g_model);
    protocol = mesh_meshcore_protocol(&g_meshcore);
    memset(&wire, 0, sizeof wire);
    mesh_protocol_attach(&protocol, mesh_test_meshcore_wire_send, &wire);
    MESH_TEST_FAIL_IF(mesh_protocol_begin(&protocol) != 0, "the handshake starts");
    uint8_t device[sizeof mesh_test_meshcore_device_info];
    memcpy(device, mesh_test_meshcore_device_info, sizeof device);
    device[3] = 1U;
    feed(&protocol, device, sizeof device);
    feed(&protocol, mesh_test_meshcore_self_info, sizeof mesh_test_meshcore_self_info);
    /* The earlier connection's answers, late. */
    feed(&protocol, device, sizeof device);
    feed(&protocol, mesh_test_meshcore_self_info, sizeof mesh_test_meshcore_self_info);
    if (mesh_test_meshcore_wire_last(&wire) == MESH_MESHCORE_CMD_SET_DEVICE_TIME) {
        feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    }
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_GET_CONTACTS,
                      "the contacts are asked for once the clock is answered");
    uint8_t end[5] = {MESH_MESHCORE_RESP_END_OF_CONTACTS};
    feed(&protocol, end, sizeof end);
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_GET_CHANNEL,
                      "the end of the contacts moves the sync on to the channels");
    uint8_t channel[2 + 32 + 16] = {MESH_MESHCORE_RESP_CHANNEL_INFO};
    feed(&protocol, channel, sizeof channel);
    MESH_TEST_FAIL_IF(!mesh_meshcore_ready(&g_meshcore), "the sync finishes");
    mesh_protocol_detach(&protocol);
    record_success(test_name);
}

/*
 * A push that a message is waiting starts the drain, and the drain runs until the radio says
 * there is no more - a direct message resolves to the contact whose key it starts with, and a
 * channel message's "Name: " becomes its sender when a node goes by that name.
 */
MESH_TEST_CASE(meshcore_drains_messages_into_the_log, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");

    feed_code(&protocol, MESH_MESHCORE_PUSH_MSG_WAITING);
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_SYNC_NEXT_MESSAGE,
                      "a waiting message is asked for");
    const uint8_t direct[] = {16,   0x14, 0, 0, 0x40, 0x41, 0x42, 0x43, 0x44,
                              0x45, 0xFF, 0, 0, 0,    0,    0,    'y',  'o'};
    const size_t before = wire.count;
    feed(&protocol, direct, sizeof direct);
    MESH_TEST_FAIL_IF(wire.count != before + 1U || mesh_test_meshcore_wire_last(&wire) !=
                                                       MESH_MESHCORE_CMD_SYNC_NEXT_MESSAGE,
                      "each message asks for the next");
    const struct mesh_message *message = newest_message();
    MESH_TEST_FAIL_IF(message == NULL || message->from != 0x40414243U ||
                          message->to != 0xb8da09b0U || strcmp(message->text, "yo") != 0 ||
                          message->rx_snr != 5.0F || !message->pki_encrypted,
                      "a direct message from Alice to us, SNR 5");

    const uint8_t channel[] = {17,  0,   0,   0,   0,   1,   0,   0,   0,   0,  0,
                               'A', 'l', 'i', 'c', 'e', ':', ' ', 'h', 'e', 'y'};
    feed(&protocol, channel, sizeof channel);
    message = newest_message();
    MESH_TEST_FAIL_IF(message == NULL || message->to != MESH_MESSAGE_BROADCAST_ADDR ||
                          message->channel != 0U || message->from != 0x40414243U ||
                          strcmp(message->text, "hey") != 0,
                      "a channel message from a known name loses the prefix and gains a sender");

    const uint8_t stranger[] = {17, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 'B', 'o', 'b', ':', ' ', 'x'};
    feed(&protocol, stranger, sizeof stranger);
    message = newest_message();
    MESH_TEST_FAIL_IF(message == NULL || message->from != 0U ||
                          strcmp(message->text, "Bob: x") != 0,
                      "a name nobody holds stays in the text");

    const size_t drained = wire.count;
    feed_code(&protocol, MESH_MESHCORE_RESP_NO_MORE_MESSAGES);
    MESH_TEST_FAIL_IF(wire.count != drained, "no more messages ends the drain");
    record_success(test_name);
}

/*
 * A direct message is PENDING until the ack the SENT reply named comes back; unanswered, it is
 * tried again with the same timestamp and a higher attempt, the last attempt after a path reset,
 * and then marked failed. A channel message is done once the radio says OK.
 */
MESH_TEST_CASE(meshcore_direct_messages_wait_for_their_ack, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");

    uint32_t packet_id = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, 0x12345678U, 0U, "x", &packet_id) !=
                          -ENOENT,
                      "a node whose key the roster lacks cannot be written to");
    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, 0x40414243U, 0U, "hello", &packet_id) !=
                          0,
                      "a direct message to Alice goes out");
    const size_t first = wire.count - 1U;
    MESH_TEST_FAIL_IF(wire.frames[first][0] != MESH_MESHCORE_CMD_SEND_TXT_MSG ||
                          wire.frames[first][2] != 0U || wire.frames[first][7] != 0x40 ||
                          wire.ids[first] != packet_id,
                      "attempt 0, to Alice's prefix, carrying the log entry's id");
    MESH_TEST_FAIL_IF(newest_message()->ack != MESH_MESSAGE_ACK_PENDING, "logged as pending");

    uint8_t sent[10] = {MESH_MESHCORE_RESP_SENT, 1};
    mesh_test_put_u32(sent + 2, 0xDEADBEEFU);
    mesh_test_put_u32(sent + 6, 1000U);
    feed(&protocol, sent, sizeof sent);

    /* The wrong ack changes nothing; the right one delivers. */
    uint8_t confirmed[9] = {MESH_MESHCORE_PUSH_SEND_CONFIRMED};
    mesh_test_put_u32(confirmed + 1, 0x0BADF00DU);
    feed(&protocol, confirmed, sizeof confirmed);
    MESH_TEST_FAIL_IF(newest_message()->ack != MESH_MESSAGE_ACK_PENDING, "another ack is ignored");
    mesh_test_put_u32(confirmed + 1, 0xDEADBEEFU);
    feed(&protocol, confirmed, sizeof confirmed);
    MESH_TEST_FAIL_IF(newest_message()->ack != MESH_MESSAGE_ACK_DELIVERED,
                      "the named ack marks it delivered");

    /* Now one that is never acked. */
    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, 0x40414243U, 0U, "again", &packet_id) !=
                          0,
                      "a second message goes out");
    uint64_t now = g_meshcore.now_ms;
    for (unsigned attempt = 0U; attempt < MESH_MESHCORE_SEND_ATTEMPTS; ++attempt) {
        const uint8_t *frame = wire.frames[wire.count - 1U];
        MESH_TEST_FAIL_IF(frame[0] != MESH_MESHCORE_CMD_SEND_TXT_MSG || frame[2] != attempt,
                          "each attempt counts up");
        mesh_test_put_u32(sent + 2, 0x1000U + attempt);
        feed(&protocol, sent, sizeof sent);
        now = g_meshcore.now_ms + 60000U;
        mesh_protocol_tick(&protocol, now);
        if (attempt + 2U == MESH_MESHCORE_SEND_ATTEMPTS) {
            MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_RESET_PATH,
                              "the last attempt resets the route first");
            feed_code(&protocol, MESH_MESHCORE_RESP_OK);
        }
    }
    const struct mesh_message *failed = mesh_message_log_find(&g_model.messages, packet_id);
    MESH_TEST_FAIL_IF(failed == NULL || failed->ack != MESH_MESSAGE_ACK_FAILED,
                      "unacknowledged after every attempt, it failed");

    MESH_TEST_FAIL_IF(
        mesh_meshcore_send_text(&g_meshcore, MESH_MESSAGE_BROADCAST_ADDR, 0U, "all", &packet_id) !=
                0 ||
            mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_SEND_CHANNEL_TXT_MSG,
        "a channel message goes out");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(newest_message()->ack != MESH_MESSAGE_ACK_NONE,
                      "OK is all a channel message gets, and it is not left pending");
    record_success(test_name);
}

/* A command the radio never answers is given up on, and two in a row is a dead link. */
MESH_TEST_CASE(meshcore_unanswered_commands_end_the_link, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");

    MESH_TEST_FAIL_IF(mesh_meshcore_send_advert(&g_meshcore, true) != 0 ||
                          wire.frames[wire.count - 1U][1] != 1U,
                      "a flooded advert is asked for");
    (void)mesh_meshcore_send_advert(&g_meshcore, false);
    const size_t written = wire.count;
    MESH_TEST_FAIL_IF(mesh_protocol_silent(&protocol), "a link with an answer due is not silent");
    mesh_protocol_tick(&protocol, g_meshcore.awaiting_since_ms + MESH_MESHCORE_REPLY_TIMEOUT_MS);
    MESH_TEST_FAIL_IF(wire.count != written + 1U, "the next command goes once one is given up on");
    MESH_TEST_FAIL_IF(mesh_protocol_silent(&protocol), "one timeout is not yet a dead link");
    mesh_protocol_tick(&protocol, g_meshcore.awaiting_since_ms + MESH_MESHCORE_REPLY_TIMEOUT_MS);
    MESH_TEST_FAIL_IF(!mesh_protocol_silent(&protocol), "two in a row is");
    record_success(test_name);
}

/* A handshake that loses a step can never finish: DEVICE_INFO with no SELF_INFO after it is a
   dead link at the first timeout, not the second, since nothing else would be sent to time out. */
MESH_TEST_CASE(meshcore_a_handshake_step_unanswered_ends_the_link, unit) {
    static struct mesh_test_meshcore_wire wire;
    memset(&wire, 0, sizeof wire);
    mesh_session_init(&g_model);
    mesh_meshcore_init(&g_meshcore, &g_model);
    const struct mesh_protocol protocol = mesh_meshcore_protocol(&g_meshcore);
    mesh_protocol_attach(&protocol, mesh_test_meshcore_wire_send, &wire);
    (void)mesh_protocol_begin(&protocol);
    feed(&protocol, mesh_test_meshcore_device_info, sizeof mesh_test_meshcore_device_info);
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_APP_START,
                      "the radio described, APP_START asks for its identity");
    MESH_TEST_FAIL_IF(mesh_protocol_silent(&protocol), "an answer due is not silence");
    mesh_protocol_tick(&protocol, g_meshcore.awaiting_since_ms + MESH_MESHCORE_REPLY_TIMEOUT_MS);
    MESH_TEST_FAIL_IF(!mesh_protocol_silent(&protocol),
                      "a SELF_INFO that never comes ends the link, to be asked again");
    record_success(test_name);
}

static uint32_t frame_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

/*
 * A Brick with no network has no date. Its uptime is not one either - a recipient reads it as
 * 1970 - so a message is stamped from the radio's clock, read at connect, and stamps never
 * repeat: the timestamp is inside the encrypted payload, and a repeated text in the same second
 * would be one packet to a mesh that deduplicates. With neither clock, 0 says "no date".
 */
MESH_TEST_CASE(meshcore_stamps_from_the_radio_clock_without_ours, unit) {
    inkwell_time_wall_set_fixed(1000U); /* a clock that has not been told the date */
    static struct mesh_test_meshcore_wire wire;
    memset(&wire, 0, sizeof wire);
    mesh_session_init(&g_model);
    mesh_meshcore_init(&g_meshcore, &g_model);
    const struct mesh_protocol protocol = mesh_meshcore_protocol(&g_meshcore);
    mesh_protocol_attach(&protocol, mesh_test_meshcore_wire_send, &wire);

    uint32_t packet_id = 0U;
    const bool undated = mesh_meshcore_send_text(&g_meshcore, MESH_MESSAGE_BROADCAST_ADDR, 0U, "a",
                                                 &packet_id) == 0 &&
                         frame_u32(wire.frames[wire.count - 1U] + 3) == 0U;
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);

    (void)mesh_protocol_begin(&protocol);
    feed(&protocol, mesh_test_meshcore_device_info, sizeof mesh_test_meshcore_device_info);
    feed(&protocol, mesh_test_meshcore_self_info, sizeof mesh_test_meshcore_self_info);
    const bool asked = mesh_test_meshcore_wire_last(&wire) == MESH_MESHCORE_CMD_GET_DEVICE_TIME;
    uint8_t clock[5] = {MESH_MESHCORE_RESP_CURR_TIME};
    mesh_test_put_u32(clock + 1, 1750000000U);
    feed(&protocol, clock, sizeof clock);
    const bool walked_on = mesh_test_meshcore_wire_last(&wire) == MESH_MESHCORE_CMD_GET_CONTACTS;
    uint8_t end[5] = {MESH_MESHCORE_RESP_END_OF_CONTACTS};
    feed(&protocol, end, sizeof end);

    /* The walk is waiting on a channel; these queue behind it and are read off the queue. */
    (void)mesh_meshcore_send_text(&g_meshcore, MESH_MESSAGE_BROADCAST_ADDR, 0U, "b", &packet_id);
    (void)mesh_meshcore_send_text(&g_meshcore, MESH_MESSAGE_BROADCAST_ADDR, 0U, "b", &packet_id);
    const size_t head = g_meshcore.queue_head;
    const uint32_t first =
        frame_u32(g_meshcore.queue[(head + 1U) % MESH_MESHCORE_QUEUE_LEN].frame + 3);
    const uint32_t second =
        frame_u32(g_meshcore.queue[(head + 2U) % MESH_MESHCORE_QUEUE_LEN].frame + 3);
    inkwell_time_wall_set_fixed(0U);

    MESH_TEST_FAIL_IF(!undated, "with no clock at all, a message says no date rather than 1970");
    MESH_TEST_FAIL_IF(!asked, "with no credible clock of ours, the radio's is asked for");
    MESH_TEST_FAIL_IF(!walked_on, "the answer moves the walk on to the contacts");
    MESH_TEST_FAIL_IF(first < 1750000000U || first > 1750000010U,
                      "a message is stamped from the radio's clock");
    MESH_TEST_FAIL_IF(second != first + 1U, "two messages in one second never share a stamp");
    record_success(test_name);
}

/*
 * A room server relays every post under its own key, with four bytes of the author's in front of
 * the text. The room stays the sender so its posts stay one conversation; the author is kept in
 * front of the text, by name when the roster knows them and by those bytes when it does not.
 */
MESH_TEST_CASE(meshcore_room_posts_keep_their_author, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");

    feed_code(&protocol, MESH_MESHCORE_PUSH_MSG_WAITING);
    const uint8_t known[] = {16, 0, 0, 0, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0xFF,
                             2,  0, 0, 0, 0,    0x40, 0x41, 0x42, 0x43, 'h',  'i'};
    feed(&protocol, known, sizeof known);
    const struct mesh_message *message = newest_message();
    MESH_TEST_FAIL_IF(message == NULL || message->from != 0x70717273U ||
                          strcmp(message->text, "Alice: hi") != 0,
                      "from the room, and Alice's post says so");

    const uint8_t stranger[] = {16, 0, 0, 0, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0xFF,
                                2,  0, 0, 0, 0,    0xAA, 0xBB, 0xCC, 0xDD, 'y',  'o'};
    feed(&protocol, stranger, sizeof stranger);
    message = newest_message();
    MESH_TEST_FAIL_IF(message == NULL || message->from != 0x70717273U ||
                          strcmp(message->text, "aabbccdd: yo") != 0,
                      "an author the roster does not hold is named by their key's bytes");
    record_success(test_name);
}

/*
 * A SENT the codec cannot read names no ack, so there is nothing to wait for: the message fails
 * then rather than sitting PENDING with no deadline, and its slot is free again.
 */
MESH_TEST_CASE(meshcore_unreadable_sent_fails_the_message, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");

    uint32_t packet_id = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, 0x40414243U, 0U, "x", &packet_id) != 0,
                      "a direct message goes out");
    const uint8_t truncated[] = {MESH_MESHCORE_RESP_SENT, 1};
    feed(&protocol, truncated, sizeof truncated);
    const struct mesh_message *message = mesh_message_log_find(&g_model.messages, packet_id);
    MESH_TEST_FAIL_IF(message == NULL || message->ack != MESH_MESSAGE_ACK_FAILED,
                      "the message fails");
    for (size_t i = 0; i < MESH_MESHCORE_PENDING_SENDS; ++i) {
        MESH_TEST_FAIL_IF(g_meshcore.pending[i].packet_id != 0U, "and gives its slot back");
    }
    record_success(test_name);
}

/* The firmware writes "Name: " in front of a channel message and cuts what no longer fits, so a
   channel's limit is what is left of 160 after the radio's own name. */
MESH_TEST_CASE(meshcore_channel_text_leaves_room_for_the_name, unit) {
    mesh_session_init(&g_model);
    mesh_meshcore_init(&g_meshcore, &g_model);
    MESH_TEST_FAIL_IF(mesh_meshcore_text_max(&g_meshcore, 0x40414243U) != MESH_MESHCORE_TEXT_MAX,
                      "a direct message carries the whole 160");
    MESH_TEST_FAIL_IF(mesh_meshcore_text_max(&g_meshcore, MESH_MESSAGE_BROADCAST_ADDR) !=
                          MESH_MESHCORE_TEXT_MAX - MESH_MESHCORE_NAME_LEN - 2U,
                      "before the radio has named itself the longest name is assumed");

    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const size_t name = strlen(g_meshcore.self.name);
    const size_t channel_max = mesh_meshcore_text_max(&g_meshcore, MESH_MESSAGE_BROADCAST_ADDR);
    MESH_TEST_FAIL_IF(name == 0U || channel_max != MESH_MESHCORE_TEXT_MAX - name - 2U,
                      "then the name it has");

    char text[MESH_MESHCORE_TEXT_MAX + 1U];
    memset(text, 'a', channel_max + 1U);
    text[channel_max + 1U] = '\0';
    uint32_t packet_id = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, MESH_MESSAGE_BROADCAST_ADDR, 0U, text,
                                              &packet_id) != -EMSGSIZE,
                      "a channel message the firmware would cut is refused whole");
    text[channel_max] = '\0';
    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, MESH_MESSAGE_BROADCAST_ADDR, 0U, text,
                                              &packet_id) != 0,
                      "and one that fits goes out");
    record_success(test_name);
}

/* SELF_INFO is the radio's settings too, and lands on the record the Settings tab reads in
   Meshtastic's shape: the name is the owner, the four radio numbers a preset-off LoRa config. */
MESH_TEST_CASE(meshcore_self_info_projects_the_settings, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const struct mesh_radio_settings *settings = mesh_session_settings(&g_model);
    MESH_TEST_FAIL_IF(settings == NULL || !settings->has_owner ||
                          strcmp(settings->owner.long_name, "MPBC") != 0,
                      "the radio's name is the owner's");
    MESH_TEST_FAIL_IF(!settings->has_lora || settings->lora.use_preset,
                      "the radio numbers are a LoRa config with no preset");
    MESH_TEST_FAIL_IF(settings->lora.bandwidth != 62U || settings->lora.spread_factor != 7U ||
                          settings->lora.coding_rate != 5U || settings->lora.tx_power != 22,
                      "62.5 kHz reads as Meshtastic writes it, and the rest as they are");
    MESH_TEST_FAIL_IF(settings->lora.override_frequency < 910.524f ||
                          settings->lora.override_frequency > 910.526f,
                      "and the frequency in MHz");
    MESH_TEST_FAIL_IF(!settings->has_position, "the position section has something to stand on");
    record_success(test_name);
}

/* A read-back with no advert location clears the fix the radio's own record held, so the
   Position rows do not show - and a save does not send back - coordinates the radio dropped. */
MESH_TEST_CASE(meshcore_self_info_without_a_location_clears_the_fix, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const struct mesh_node_summary *self =
        mesh_session_model_node(&g_model, g_meshcore.self_node, false);
    MESH_TEST_FAIL_IF(self == NULL || !self->position.valid, "the captured radio has a fix");

    MESH_TEST_FAIL_IF(mesh_meshcore_refresh_settings(&g_meshcore) != 1, "a refresh is asked");
    uint8_t cleared[sizeof mesh_test_meshcore_self_info];
    memcpy(cleared, mesh_test_meshcore_self_info, sizeof cleared);
    memset(cleared + 36, 0, 8U); /* lat_e6, lon_e6 */
    feed(&protocol, cleared, sizeof cleared);
    self = mesh_session_model_node(&g_model, g_meshcore.self_node, false);
    MESH_TEST_FAIL_IF(self == NULL || self->position.valid, "and the fix goes with the location");
    record_success(test_name);
}

/* A settings command the link refuses outright is settled as refused, so the save is reported
   and the next one is not held off behind it. */
MESH_TEST_CASE(meshcore_settings_write_the_link_refuses_is_settled, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const struct mesh_radio_settings *settings = mesh_session_settings(&g_model);
    const uint32_t failed = settings->writes_failed;
    struct mesh_meshcore_settings_write write;
    memset(&write, 0, sizeof write);
    write.set_name = true;
    memcpy(write.name, "Pine", 5U);
    wire.refuse = true;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != -EAGAIN,
                      "the link's refusal is the save's answer");
    MESH_TEST_FAIL_IF(settings->writes_failed != failed + 1U ||
                          settings->last_write_error != -EAGAIN,
                      "is refused by the link and settled as such");
    wire.refuse = false;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != 1,
                      "and the next save is not held behind it");
    record_success(test_name);
}

/* The Bluetooth PIN is one u32 in DEVICE_INFO rather than SELF_INFO: its OK is the whole
   answer, with no APP_START after it, and a value the firmware would refuse is never sent. */
MESH_TEST_CASE(meshcore_settings_write_sets_the_bluetooth_pin, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const struct mesh_radio_settings *settings = mesh_session_settings(&g_model);
    const uint32_t acked = settings->writes_acked;

    struct mesh_meshcore_settings_write write;
    memset(&write, 0, sizeof write);
    write.set_pin = true;
    write.ble_pin = 12345U;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != -EINVAL,
                      "five digits is a PIN the firmware refuses");
    write.ble_pin = 1000000U;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != -EINVAL,
                      "and so is seven");

    write.ble_pin = 482913U;
    const size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != 1, "one command");
    MESH_TEST_FAIL_IF(wire.count != before + 1U ||
                          wire.frames[before][0] != MESH_MESHCORE_CMD_SET_DEVICE_PIN ||
                          wire.lens[before] != 5U || frame_u32(wire.frames[before] + 1) != 482913U,
                      "SET_DEVICE_PIN and the PIN, little-endian");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(settings->writes_acked != acked + 1U, "its OK settles the save");
    MESH_TEST_FAIL_IF(wire.count != before + 1U, "and nothing is read back after it");
    MESH_TEST_FAIL_IF(g_meshcore.device.ble_pin != 482913U, "the OK moves DEVICE_INFO's PIN");

    /* Zero is the firmware's random PIN, not a PIN out of range. */
    write.ble_pin = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != 1,
                      "zero goes back to a random PIN");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(g_meshcore.device.ble_pin != 0U, "and the OK says so");
    record_success(test_name);
}

/* A save is each group's command and then APP_START, whose SELF_INFO is the read-back; the
   answers settle into the write counters the app's save toast watches. */
MESH_TEST_CASE(meshcore_settings_write_is_commands_then_a_read_back, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const struct mesh_radio_settings *settings = mesh_session_settings(&g_model);
    const uint32_t acked = settings->writes_acked;

    struct mesh_meshcore_settings_write write;
    memset(&write, 0, sizeof write);
    write.set_name = true;
    memcpy(write.name, "Pine", 5U);
    write.set_radio = true;
    write.frequency_khz = 869618U;
    write.bandwidth_hz = 62500U;
    write.spreading_factor = 8U;
    write.coding_rate = 8U;
    write.set_tx_power = true;
    write.tx_power_dbm = -2;
    write.set_position = true;
    write.latitude_e6 = -33868800;
    write.longitude_e6 = 151209300;
    const size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != 4,
                      "four groups are four commands");
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != -EBUSY,
                      "and a second save waits for the first");

    /* One at a time: each answer lets the next command out. */
    MESH_TEST_FAIL_IF(
        wire.count != before + 1U || wire.frames[before][0] != MESH_MESHCORE_CMD_SET_ADVERT_NAME ||
            wire.lens[before] != 5U || memcmp(wire.frames[before] + 1, "Pine", 4U) != 0,
        "the name goes first, unterminated");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    const uint8_t *radio = wire.frames[before + 1U];
    MESH_TEST_FAIL_IF(radio[0] != MESH_MESHCORE_CMD_SET_RADIO_PARAMS ||
                          wire.lens[before + 1U] != 11U || frame_u32(radio + 1) != 869618U ||
                          frame_u32(radio + 5) != 62500U || radio[9] != 8U || radio[10] != 8U,
                      "then the radio numbers as one command");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    const uint8_t *power = wire.frames[before + 2U];
    MESH_TEST_FAIL_IF(power[0] != MESH_MESHCORE_CMD_SET_RADIO_TX_POWER || (int8_t)power[1] != -2,
                      "then the power, signed");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    const uint8_t *place = wire.frames[before + 3U];
    MESH_TEST_FAIL_IF(place[0] != MESH_MESHCORE_CMD_SET_ADVERT_LATLON ||
                          (int32_t)frame_u32(place + 1) != -33868800 ||
                          (int32_t)frame_u32(place + 5) != 151209300,
                      "then the position");
    MESH_TEST_FAIL_IF(settings->writes_acked != acked, "nothing is settled while one is out");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(settings->writes_acked != acked + 1U, "the last OK settles the save");
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_APP_START,
                      "and the read-back follows");
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != -EBUSY,
                      "and no save is built over values a read-back is about to replace");
    /* Each OK moved the baseline, so a save made before the read-back lands is built over
       what the radio now holds rather than undoing it. */
    MESH_TEST_FAIL_IF(
        strcmp(g_meshcore.self.name, "Pine") != 0 || g_meshcore.self.frequency_khz != 869618U ||
            g_meshcore.self.spreading_factor != 8U || (int8_t)g_meshcore.self.tx_power_dbm != -2 ||
            g_meshcore.self.latitude_e6 != -33868800,
        "the baseline follows each OK ahead of the read-back");
    MESH_TEST_FAIL_IF(strcmp(settings->owner.long_name, "Pine") != 0 ||
                          settings->lora.spread_factor != 8U,
                      "and so do the settings the screens read");

    /* The other parameters are one command, all four bytes, and move the baseline too. */
    feed(&protocol, mesh_test_meshcore_self_info, sizeof mesh_test_meshcore_self_info);
    struct mesh_meshcore_settings_write other;
    memset(&other, 0, sizeof other);
    other.set_other = true;
    other.manual_add_contacts = 1U;
    other.telemetry_modes = 0x26U;
    other.advert_loc_policy = 1U;
    other.multi_acks = 1U;
    const size_t other_at = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &other) != 1, "one command");
    const uint8_t expected_other[5] = {MESH_MESHCORE_CMD_SET_OTHER_PARAMS, 1U, 0x26U, 1U, 1U};
    MESH_TEST_FAIL_IF(wire.lens[other_at] != 5U ||
                          memcmp(wire.frames[other_at], expected_other, 5U) != 0,
                      "manual add, telemetry modes, location policy, multi-acks");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(g_meshcore.self.telemetry_modes != 0x26U ||
                          g_meshcore.self.manual_add_contacts != 1U ||
                          g_meshcore.self.multi_acks != 1U,
                      "and the OK moves the baseline");

    /* A refusal fails the save with the radio's own code. */
    const uint32_t failed = settings->writes_failed;
    feed(&protocol, mesh_test_meshcore_self_info, sizeof mesh_test_meshcore_self_info);
    write.set_radio = false;
    write.set_position = false;
    write.set_name = false;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != 1, "power alone");
    const uint8_t refused[] = {MESH_MESHCORE_RESP_ERR, 6U};
    feed(&protocol, refused, sizeof refused);
    MESH_TEST_FAIL_IF(settings->writes_failed != failed + 1U || settings->last_write_error != 6,
                      "a refused write says which error");
    record_success(test_name);
}

/* A channel is one SET_CHANNEL - slot, the name in 32 bytes, the 16-byte secret - and read back
   by GET_CHANNEL for that slot alone. Its OK lands in the roster's channel and the settings. */
MESH_TEST_CASE(meshcore_channel_write_is_one_slot_whole, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    g_meshcore.device.max_channels = 8U;
    const struct mesh_radio_settings *settings = mesh_session_settings(&g_model);
    MESH_TEST_FAIL_IF(!settings->has_channel[0] || settings->channels[0].settings.psk.size != 16U ||
                          settings->channels[0].settings.psk.bytes[0] != 0x8b,
                      "the walk keeps each slot's secret for the editor");
    /* A new handshake forgets every slot until its walk reads it again. */
    mesh_protocol_detach(&protocol);
    memset(&wire, 0, sizeof wire);
    mesh_protocol_attach(&protocol, mesh_test_meshcore_wire_send, &wire);
    MESH_TEST_FAIL_IF(mesh_protocol_begin(&protocol) < 0 || settings->has_channel[0],
                      "a reconnect's walk starts with no slot to edit");
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "and walks to ready again");
    g_meshcore.device.max_channels = 8U;

    struct mesh_meshcore_settings_write write;
    memset(&write, 0, sizeof write);
    write.set_channel = true;
    write.channel_index = 2U;
    memcpy(write.channel_name, "#a-long-hashtag-channel-name", 29U);
    memset(write.channel_secret, 0x5a, sizeof write.channel_secret);
    const size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != 1, "one command");
    const uint8_t *frame = wire.frames[before];
    MESH_TEST_FAIL_IF(frame[0] != MESH_MESHCORE_CMD_SET_CHANNEL || wire.lens[before] != 50U ||
                          frame[1] != 2U ||
                          memcmp(frame + 2, "#a-long-hashtag-channel-name", 29U) != 0 ||
                          frame[2 + 29] != 0U || frame[34] != 0x5a || frame[49] != 0x5a,
                      "slot, name in its field, secret");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_GET_CHANNEL ||
                          wire.frames[wire.count - 1U][1] != 2U,
                      "read back by its own slot, not the whole handshake");
    MESH_TEST_FAIL_IF(strcmp(g_model.handshake.channels[2].name, "#a-long-hashtag-channel-name") !=
                              0 ||
                          g_model.handshake.channels[2].role != 2U,
                      "the OK names the channel, past Meshtastic's eleven bytes");
    MESH_TEST_FAIL_IF(!settings->has_channel[2] ||
                          settings->channels[2].settings.psk.bytes[0] != 0x5a,
                      "and gives the editor its secret");

    /* Cleared is the same command with nothing in it, and the slot reads unused. */
    feed_code(&protocol, MESH_MESHCORE_RESP_ERR); /* the read-back, refused: nothing changes */
    memset(&write.channel_name, 0, sizeof write.channel_name);
    memset(write.channel_secret, 0, sizeof write.channel_secret);
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != 1, "a clear");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(g_model.handshake.channels[2].role != 0U ||
                          settings->channels[2].has_settings,
                      "an emptied slot is unused");

    char too_long[40];
    memset(too_long, 'x', sizeof too_long);
    memcpy(write.channel_name, too_long, sizeof write.channel_name);
    feed_code(&protocol, MESH_MESHCORE_RESP_OK); /* the clear's read-back */
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != -EINVAL,
                      "a name that fills the field has no room for the firmware's NUL");
    write.channel_name[0] = '\0';
    write.channel_index = 8U;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != -EINVAL,
                      "and a slot past the radio's is refused");
    record_success(test_name);
}

MESH_TEST_CASE(meshcore_settings_write_refuses_what_it_cannot_send_whole, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    struct mesh_meshcore_settings_write write;
    memset(&write, 0, sizeof write);
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != -EINVAL,
                      "a save that writes nothing");
    write.set_name = true;
    memset(write.name, 'n', MESH_MESHCORE_NAME_LEN);
    write.name[MESH_MESHCORE_NAME_LEN] = '\0';
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != 1,
                      "a name at the firmware's limit is written");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    feed(&protocol, mesh_test_meshcore_self_info,
         sizeof mesh_test_meshcore_self_info); /* the read-back */
    write.set_name = true;
    write.name[0] = '\0';
    const size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != -EINVAL ||
                          g_meshcore.writes_outstanding != 0U,
                      "an empty name is refused before anything is queued");
    MESH_TEST_FAIL_IF(wire.count != before, "and nothing reached the radio");
    record_success(test_name);
}

/* Removing a contact is REMOVE_CONTACT with its whole key, and the roster lets it go at once:
   nothing reads the list back, and it returns by itself when the node next adverts. */
MESH_TEST_CASE(meshcore_remove_contact_takes_it_off_both_lists, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    MESH_TEST_FAIL_IF(mesh_meshcore_remove_contact(&g_meshcore, g_meshcore.self_node) != -EINVAL,
                      "this radio is not a contact of its own");
    MESH_TEST_FAIL_IF(mesh_meshcore_remove_contact(&g_meshcore, 0x12345678U) != -ENOENT,
                      "a node the roster does not hold has no key to name");
    g_meshcore.phase = MESH_MESHCORE_CHANNELS;
    MESH_TEST_FAIL_IF(mesh_meshcore_remove_contact(&g_meshcore, 0x40414243U) != -ENOTCONN,
                      "the roster may be the last radio's until the handshake is through");
    g_meshcore.phase = MESH_MESHCORE_READY;
    struct mesh_node_summary *alice = mesh_session_model_node(g_meshcore.model, 0x40414243U, false);
    MESH_TEST_FAIL_IF(alice == NULL, "Alice is on the roster");
    alice->in_nodedb = false;
    MESH_TEST_FAIL_IF(mesh_meshcore_remove_contact(&g_meshcore, 0x40414243U) != -ENOENT,
                      "a node the radio does not carry is no contact to remove");
    alice->in_nodedb = true;
    const size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_remove_contact(&g_meshcore, 0x40414243U) != 1,
                      "a contact is asked off");
    MESH_TEST_FAIL_IF(wire.count != before + 1U ||
                          wire.frames[before][0] != MESH_MESHCORE_CMD_REMOVE_CONTACT ||
                          wire.lens[before] != 33U || wire.frames[before][1] != 0x40 ||
                          wire.frames[before][32] != 0x40 + 31,
                      "by its whole key");
    MESH_TEST_FAIL_IF(model_node(0x40414243U) == NULL, "and stays listed until the radio agrees");
    feed_code(&protocol, MESH_MESHCORE_RESP_ERR);
    MESH_TEST_FAIL_IF(model_node(0x40414243U) == NULL, "a refusal leaves it on the list");
    MESH_TEST_FAIL_IF(mesh_meshcore_remove_contact(&g_meshcore, 0x40414243U) != 1, "asked again");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(model_node(0x40414243U) != NULL, "and the radio's OK takes it off");
    MESH_TEST_FAIL_IF(mesh_meshcore_remove_contact(&g_meshcore, 0x40414243U) != -ENOENT,
                      "a second press has nothing left to remove");
    record_success(test_name);
}

/* A node heard in manual-add mode is listed but not a contact; adding it sends the record the
   advert gave - key, kind, name, stamp, position, no route - and it joins the list on OK. */
MESH_TEST_CASE(meshcore_add_contact_from_a_heard_advert, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    uint8_t advert[160];
    const size_t advert_len =
        mesh_test_meshcore_contact(advert, MESH_MESHCORE_PUSH_NEW_ADVERT, 0x60, "Bob",
                                   MESH_MESHCORE_ADV_REPEATER, 0xffU, 1700000100U);
    feed(&protocol, advert, advert_len);
    const uint32_t bob = 0x60616263U;
    MESH_TEST_FAIL_IF(model_node(bob) == NULL || model_node(bob)->in_nodedb,
                      "a heard node is listed, and not a contact");
    MESH_TEST_FAIL_IF(mesh_meshcore_add_contact(&g_meshcore, 0x40414243U) != -EEXIST,
                      "a contact is not added twice");
    const size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_add_contact(&g_meshcore, bob) != 1, "a heard node is added");
    const uint8_t *frame = wire.frames[before];
    MESH_TEST_FAIL_IF(wire.count != before + 1U || wire.lens[before] != 144U ||
                          frame[0] != MESH_MESHCORE_CMD_ADD_UPDATE_CONTACT || frame[1] != 0x60 ||
                          frame[33] != MESH_MESHCORE_ADV_REPEATER || frame[35] != 0xffU ||
                          memcmp(frame + 100, "Bob", 4U) != 0 ||
                          (int32_t)frame_u32(frame + 136) != 37774900 ||
                          frame_u32(frame + 132) != 1700000100U - 60U ||
                          (int32_t)frame_u32(frame + 140) != -122419400,
                      "by the record its advert gave - the sender's stamp - with no route");
    MESH_TEST_FAIL_IF(model_node(bob)->in_nodedb, "and is not a contact until the radio agrees");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(!model_node(bob)->in_nodedb, "the OK makes it one");

    /* A node pushed off the roster while its add waits comes back on the OK, from the record. */
    size_t carol_len =
        mesh_test_meshcore_contact(advert, MESH_MESHCORE_PUSH_NEW_ADVERT, 0x80, "Carol",
                                   MESH_MESHCORE_ADV_CHAT, 0xffU, 1700000150U);
    feed(&protocol, advert, carol_len);
    MESH_TEST_FAIL_IF(mesh_meshcore_add_contact(&g_meshcore, 0x80818283U) != 1, "Carol is added");
    MESH_TEST_FAIL_IF(mesh_session_model_drop_node(g_meshcore.model, 0x80818283U) != 0,
                      "and dropped from the roster while the add waits");
    /* A Brick with no network time has the radio's clock, read at the handshake. */
    g_meshcore.radio_clock = 1700000500U;
    g_meshcore.radio_clock_at_ms = inkwell_time_monotonic_ms();
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(model_node(0x80818283U) == NULL || !model_node(0x80818283U)->in_nodedb ||
                          strcmp(model_node(0x80818283U)->long_name, "Carol") != 0,
                      "the OK puts her back, a contact, by the name sent");
    MESH_TEST_FAIL_IF(model_node(0x80818283U)->last_heard == 0U,
                      "and heard as of the OK, not never");
    static const char k_long[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    const size_t long_len =
        mesh_test_meshcore_contact(advert, MESH_MESHCORE_PUSH_NEW_ADVERT, 0x70, k_long,
                                   MESH_MESHCORE_ADV_CHAT, 0xffU, 1700000200U);
    feed(&protocol, advert, long_len);
    const size_t again = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_add_contact(&g_meshcore, 0x70717273U) != 1 ||
                          memcmp(wire.frames[again] + 100, k_long, 32U) != 0,
                      "a name the whole 32 bytes long is sent whole");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    record_success(test_name);
}

/* The adverts kept for adding are the newest: a node heard again is kept over one heard once. */
MESH_TEST_CASE(meshcore_heard_adverts_keep_the_newest, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    uint8_t advert[160];
    for (uint8_t n = 0U; n < MESH_MESHCORE_HEARD_ADVERTS; ++n) {
        const size_t len =
            mesh_test_meshcore_contact(advert, MESH_MESHCORE_PUSH_NEW_ADVERT, (uint8_t)(0x90U + n),
                                       "Node", MESH_MESHCORE_ADV_CHAT, 0xffU, 1700001000U + n);
        feed(&protocol, advert, len);
    }
    /* The first is heard again, and then a node not heard before takes the oldest slot. */
    size_t len = mesh_test_meshcore_contact(advert, MESH_MESHCORE_PUSH_NEW_ADVERT, 0x90U, "Node",
                                            MESH_MESHCORE_ADV_CHAT, 0xffU, 1700002000U);
    feed(&protocol, advert, len);
    len = mesh_test_meshcore_contact(advert, MESH_MESHCORE_PUSH_NEW_ADVERT, 0xB0U, "Node",
                                     MESH_MESHCORE_ADV_CHAT, 0xffU, 1700003000U);
    feed(&protocol, advert, len);
    size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_add_contact(&g_meshcore, 0x90919293U) != 1 ||
                          frame_u32(wire.frames[before] + 132) != 1700002000U - 60U,
                      "the node heard again keeps its advert");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    before = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_add_contact(&g_meshcore, 0x91929394U) != 1 ||
                          frame_u32(wire.frames[before] + 132) != 0U,
                      "and the one heard longest ago is the one let go");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);

    /* A new connection keeps none of them: an advert from before it may be older than what the
       sender now stamps, so a node heard then is added with no stamp. */
    MESH_TEST_FAIL_IF(mesh_protocol_begin(&protocol) != 0, "the link begins again");
    for (size_t i = 0; i < MESH_MESHCORE_HEARD_ADVERTS; ++i) {
        MESH_TEST_FAIL_IF(g_meshcore.heard_age[i] != 0U, "and no advert is kept from before");
    }
    record_success(test_name);
}

/*
 * MeshCore fills the model through the session, so its listener hears it exactly as it hears
 * Meshtastic: the contact list as a listing, an advert and a direct message as hearings, and
 * every message in either direction once.
 */
MESH_TEST_CASE(meshcore_announces_what_it_writes_into_the_model, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    static struct mesh_test_event_record record;
    memset(&record, 0, sizeof record);
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    /* The handshake initialises the model, so the observer goes on after it and the contact is
       fed again as a later listing would carry it. */
    mesh_session_set_observer(&g_model, mesh_test_event_record_fn, &record);
    uint8_t frame[160];
    feed(&protocol, frame,
         mesh_test_meshcore_contact(frame, MESH_MESHCORE_RESP_CONTACT, 0x40, "Alice",
                                    MESH_MESHCORE_ADV_CHAT, 2U, 1700000000U));
    MESH_TEST_FAIL_IF(mesh_test_event_count(&record, MESH_SESSION_EVENT_NODE_LISTED, 0x40414243U) !=
                          1U,
                      "a contact the radio heard is listed");
    MESH_TEST_FAIL_IF(record.events[0].hops != 2U || !record.events[0].has_hops,
                      "with the path the radio holds for it");

    const size_t len = mesh_test_meshcore_contact(frame, MESH_MESHCORE_PUSH_NEW_ADVERT, 0x90U,
                                                  "Node", MESH_MESHCORE_ADV_CHAT, 0U, 1700001000U);
    feed(&protocol, frame, len);
    MESH_TEST_FAIL_IF(mesh_test_event_count(&record, MESH_SESSION_EVENT_NODE_HEARD, 0x90919293U) !=
                          1U,
                      "an advert is a hearing");

    feed_code(&protocol, MESH_MESHCORE_PUSH_MSG_WAITING);
    const uint8_t direct[] = {16,   0x14, 0, 0, 0x40, 0x41, 0x42, 0x43, 0x44,
                              0x45, 0xFF, 0, 0, 0,    0,    0,    'y',  'o'};
    feed(&protocol, direct, sizeof direct);
    MESH_TEST_FAIL_IF(mesh_test_event_count(&record, MESH_SESSION_EVENT_NODE_HEARD, 0x40414243U) !=
                          1U,
                      "a direct message is a hearing of its sender");
    MESH_TEST_FAIL_IF(mesh_test_event_count(&record, MESH_SESSION_EVENT_MESSAGE, 0x40414243U) != 1U,
                      "and a message");
    feed_code(&protocol, MESH_MESHCORE_RESP_NO_MORE_MESSAGES);

    const size_t heard = mesh_test_event_count(&record, MESH_SESSION_EVENT_NODE_HEARD, 0U);
    MESH_TEST_FAIL_IF(
        mesh_meshcore_send_text(&g_meshcore, MESH_MESSAGE_BROADCAST_ADDR, 0U, "hi", NULL) != 0,
        "the channel send failed");
    MESH_TEST_FAIL_IF(mesh_test_event_count(&record, MESH_SESSION_EVENT_MESSAGE, 0U) != 2U ||
                          record.events[record.count - 1U].direction != MESH_MESSAGE_OUTBOUND,
                      "a send is announced going out");
    MESH_TEST_FAIL_IF(mesh_test_event_count(&record, MESH_SESSION_EVENT_NODE_HEARD, 0U) != heard,
                      "and hears nobody");
    record_success(test_name);
}

/* A favourite is bit 0 of a contact's flags, and an update replaces the whole record: the
       radio's record is read first and written back with that bit alone changed, route and all. */
MESH_TEST_CASE(meshcore_favorite_rewrites_the_radios_record, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const uint32_t alice = 0x40414243U;
    MESH_TEST_FAIL_IF(model_node(alice) == NULL || model_node(alice)->is_favorite,
                      "Alice is a contact, and no favourite");
    size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_set_favorite(&g_meshcore, alice, true) != 1 ||
                          wire.count != before + 1U ||
                          wire.frames[before][0] != MESH_MESHCORE_CMD_GET_CONTACT_BY_KEY ||
                          wire.frames[before][1] != 0x40,
                      "pinning reads the radio's record first");
    uint8_t record[160];
    const size_t record_len = mesh_test_meshcore_contact(
        record, MESH_MESHCORE_RESP_CONTACT, 0x40, "Alice", MESH_MESHCORE_ADV_CHAT, 2U, 1700000300U);
    record[34] = 0x04U; /* a permission bit above the favourite, left as it is */
    record[36] = 0xAAU;
    record[37] = 0xBBU;
    before = wire.count;
    feed(&protocol, record, record_len);
    const uint8_t *frame = wire.frames[before];
    MESH_TEST_FAIL_IF(
        wire.count != before + 1U || frame[0] != MESH_MESHCORE_CMD_ADD_UPDATE_CONTACT ||
            frame[34] != (0x04U | MESH_MESHCORE_CONTACT_FAVORITE) || frame[35] != 2U ||
            frame[36] != 0xAAU || frame[37] != 0xBBU || memcmp(frame + 100, "Alice", 6U) != 0,
        "and writes it back whole with the favourite bit set");
    MESH_TEST_FAIL_IF(model_node(alice)->is_favorite, "not pinned until the radio agrees");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(!model_node(alice)->is_favorite || !model_node(alice)->in_nodedb,
                      "the OK pins it, and it stays a contact");
    MESH_TEST_FAIL_IF(mesh_meshcore_set_favorite(&g_meshcore, alice, true) != 0,
                      "pinning a pinned contact asks nothing");

    MESH_TEST_FAIL_IF(mesh_meshcore_set_favorite(&g_meshcore, alice, false) != 1,
                      "unpinning is asked");
    before = wire.count;
    feed_code(&protocol, MESH_MESHCORE_RESP_ERR);
    MESH_TEST_FAIL_IF(!model_node(alice)->is_favorite || wire.count != before,
                      "a radio that no longer has the contact leaves the flag, and writes nothing");

    uint8_t advert[160];
    const size_t advert_len =
        mesh_test_meshcore_contact(advert, MESH_MESHCORE_PUSH_NEW_ADVERT, 0x60, "Bob",
                                   MESH_MESHCORE_ADV_CHAT, 0xffU, 1700000100U);
    feed(&protocol, advert, advert_len);
    const uint32_t bob = 0x60616263U;
    MESH_TEST_FAIL_IF(mesh_meshcore_set_favorite(&g_meshcore, bob, true) != -ENOENT,
                      "a heard node is no contact to pin");

    /* Two in flight: each lookup carries its own answer, and both are written. */
    struct mesh_node_summary *bob_node = mesh_session_model_node(g_meshcore.model, bob, false);
    bob_node->in_nodedb = true;
    before = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_set_favorite(&g_meshcore, alice, false) != 1 ||
                          mesh_meshcore_set_favorite(&g_meshcore, bob, true) != 1,
                      "unpinning one and pinning another are both asked");
    /* Each write goes out where its lookup stood, ahead of what was asked after it. */
    feed(&protocol, record, record_len); /* Alice's record, still 0x04 | favourite on the radio */
    MESH_TEST_FAIL_IF(wire.frames[wire.count - 1U][0] != MESH_MESHCORE_CMD_ADD_UPDATE_CONTACT ||
                          wire.frames[wire.count - 1U][1] != 0x40,
                      "Alice's write goes ahead of Bob's lookup");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    uint8_t bob_record[160];
    const size_t bob_len =
        mesh_test_meshcore_contact(bob_record, MESH_MESHCORE_RESP_CONTACT, 0x60, "Bob",
                                   MESH_MESHCORE_ADV_CHAT, 0xffU, 1700000100U);
    feed(&protocol, bob_record, bob_len);
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    bool alice_written = false;
    bool bob_written = false;
    for (size_t i = before; i < wire.count; ++i) {
        const uint8_t *f = wire.frames[i];
        if (f[0] != MESH_MESHCORE_CMD_ADD_UPDATE_CONTACT) {
            continue;
        }
        alice_written |= f[1] == 0x40 && (f[34] & MESH_MESHCORE_CONTACT_FAVORITE) == 0U;
        bob_written |= f[1] == 0x60 && (f[34] & MESH_MESHCORE_CONTACT_FAVORITE) != 0U;
    }
    MESH_TEST_FAIL_IF(!alice_written || !bob_written,
                      "each lookup writes its own contact back with its own answer");
    MESH_TEST_FAIL_IF(model_node(alice)->is_favorite || !model_node(bob)->is_favorite,
                      "and both land on the OKs");

    /* A full queue: the write takes the slot its lookup leaves. */
    MESH_TEST_FAIL_IF(mesh_meshcore_set_favorite(&g_meshcore, alice, true) != 1, "pin again");
    while (g_meshcore.queue_count < MESH_MESHCORE_QUEUE_LEN) {
        MESH_TEST_FAIL_IF(mesh_meshcore_send_advert(&g_meshcore, false) != 0, "fill the queue");
    }
    record[34] = 0x04U;
    feed(&protocol, record, record_len);
    MESH_TEST_FAIL_IF(g_meshcore.queue_count != MESH_MESHCORE_QUEUE_LEN,
                      "the write is queued in the slot the lookup left");
    before = wire.count;
    while (g_meshcore.queue_count > 0U) {
        feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    }
    MESH_TEST_FAIL_IF(!model_node(alice)->is_favorite, "and lands once the queue drains");

    /* Unpin, then remove: the write goes ahead of the removal, which is the last word. */
    wire.count = 0U; /* the fake link keeps 32 frames, and the full queue used them */
    MESH_TEST_FAIL_IF(mesh_meshcore_set_favorite(&g_meshcore, alice, false) != 1 ||
                          mesh_meshcore_remove_contact(&g_meshcore, alice) != 1,
                      "an unpin and then a removal are asked");
    feed(&protocol, record, record_len);
    MESH_TEST_FAIL_IF(wire.frames[wire.count - 1U][0] != MESH_MESHCORE_CMD_ADD_UPDATE_CONTACT,
                      "the unpin's write goes first");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(wire.frames[wire.count - 1U][0] != MESH_MESHCORE_CMD_REMOVE_CONTACT,
                      "and the removal after it, so the contact stays removed");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(model_node(alice) != NULL, "and it is gone");

    /* A contact dropped elsewhere takes its favourite with it. */
    struct mesh_node_summary *pinned = mesh_session_model_node(g_meshcore.model, bob, false);
    pinned->is_favorite = true;
    uint8_t deleted[1U + MESH_MESHCORE_PUBKEY_LEN];
    deleted[0] = MESH_MESHCORE_PUSH_CONTACT_DELETED;
    for (size_t i = 0; i < MESH_MESHCORE_PUBKEY_LEN; ++i) {
        deleted[1U + i] = (uint8_t)(0x60U + i);
    }
    feed(&protocol, deleted, sizeof deleted);
    MESH_TEST_FAIL_IF(model_node(bob)->in_nodedb || model_node(bob)->is_favorite,
                      "a contact the radio dropped is no longer pinned");
    record_success(test_name);
}

/* A contact link names a node by key alone: it is added with no route and no stamp, and joins
   the roster - never heard - on the radio's OK. */
MESH_TEST_CASE(meshcore_import_contact_from_a_link, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    uint8_t key[MESH_MESHCORE_PUBKEY_LEN];
    for (size_t i = 0; i < sizeof key; ++i) {
        key[i] = (uint8_t)(0xC0U + i);
    }
    MESH_TEST_FAIL_IF(mesh_meshcore_import_contact(&g_meshcore, key, "Dave", 9U) != -EINVAL,
                      "a kind outside the four is refused");
    MESH_TEST_FAIL_IF(mesh_meshcore_import_contact(&g_meshcore, g_meshcore.self.public_key, "Me",
                                                   MESH_MESHCORE_ADV_CHAT) != -EINVAL,
                      "this radio is not a contact of its own");
    uint8_t alice[MESH_MESHCORE_PUBKEY_LEN];
    for (size_t i = 0; i < sizeof alice; ++i) {
        alice[i] = (uint8_t)(0x40U + i);
    }
    MESH_TEST_FAIL_IF(mesh_meshcore_import_contact(&g_meshcore, alice, "Alice",
                                                   MESH_MESHCORE_ADV_CHAT) != -EEXIST,
                      "a contact's record, route and all, is not written over from a link");
    uint8_t twin[MESH_MESHCORE_PUBKEY_LEN];
    memcpy(twin, alice, sizeof twin);
    twin[31] ^= 0xFFU;
    MESH_TEST_FAIL_IF(mesh_meshcore_import_contact(&g_meshcore, twin, "Mallory",
                                                   MESH_MESHCORE_ADV_CHAT) != -EADDRINUSE,
                      "a key that starts as a roster node's would be stored over it");
    memcpy(twin, g_meshcore.self.public_key, sizeof twin);
    twin[31] ^= 0xFFU;
    MESH_TEST_FAIL_IF(mesh_meshcore_import_contact(&g_meshcore, twin, "Mallory",
                                                   MESH_MESHCORE_ADV_CHAT) != -EADDRINUSE,
                      "and one that starts as this radio's over its own row");
    const size_t before = wire.count;
    MESH_TEST_FAIL_IF(
        mesh_meshcore_import_contact(&g_meshcore, key, "Dave", MESH_MESHCORE_ADV_ROOM) != 1,
        "a stranger's link is asked");
    const size_t queued = g_meshcore.queue_count;
    MESH_TEST_FAIL_IF(
        mesh_meshcore_import_contact(&g_meshcore, key, "Dave", MESH_MESHCORE_ADV_ROOM) != 1 ||
            g_meshcore.queue_count != queued,
        "the same link again is already asked");
    uint8_t cousin[MESH_MESHCORE_PUBKEY_LEN];
    memcpy(cousin, key, sizeof cousin);
    cousin[31] ^= 0xFFU;
    MESH_TEST_FAIL_IF(mesh_meshcore_import_contact(&g_meshcore, cousin, "Eve",
                                                   MESH_MESHCORE_ADV_CHAT) != -EADDRINUSE,
                      "and a key starting the same, while the first waits, would land on it");
    const uint8_t *frame = wire.frames[before];
    MESH_TEST_FAIL_IF(wire.count != before + 1U ||
                          frame[0] != MESH_MESHCORE_CMD_ADD_UPDATE_CONTACT || frame[1] != 0xC0U ||
                          frame[33] != MESH_MESHCORE_ADV_ROOM ||
                          frame[35] != MESH_MESHCORE_PATH_NONE ||
                          memcmp(frame + 100, "Dave", 5U) != 0 || frame_u32(frame + 132) != 0U,
                      "by key, kind and name, with no route and no stamp");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    const struct mesh_node_summary *dave = model_node(0xC0C1C2C3U);
    MESH_TEST_FAIL_IF(dave == NULL || !dave->in_nodedb || strcmp(dave->long_name, "Dave") != 0,
                      "the OK puts it on the roster, a contact");
    MESH_TEST_FAIL_IF(dave->last_heard != 0U, "and never heard: a link is not a transmission");
    MESH_TEST_FAIL_IF(dave->discovered != 0U,
                      "nor a discovery: the user typed it in, and a notice would say so back");

    /* A heard node added with no advert kept - one from before this connection - carries no
       stamp either, and is still heard as of the OK if the roster let it go meanwhile. */
    uint8_t advert[160];
    const size_t advert_len =
        mesh_test_meshcore_contact(advert, MESH_MESHCORE_PUSH_NEW_ADVERT, 0xD0, "Erin",
                                   MESH_MESHCORE_ADV_CHAT, 0xffU, 1700000700U);
    feed(&protocol, advert, advert_len);
    memset(g_meshcore.heard_age, 0, sizeof g_meshcore.heard_age);
    g_meshcore.radio_clock = 1700000800U;
    g_meshcore.radio_clock_at_ms = inkwell_time_monotonic_ms();
    MESH_TEST_FAIL_IF(mesh_meshcore_add_contact(&g_meshcore, 0xD0D1D2D3U) != 1 ||
                          frame_u32(wire.frames[wire.count - 1U] + 132) != 0U,
                      "Erin is added with no stamp");
    MESH_TEST_FAIL_IF(mesh_session_model_drop_node(g_meshcore.model, 0xD0D1D2D3U) != 0,
                      "and dropped from the roster while the add waits");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(model_node(0xD0D1D2D3U) == NULL || model_node(0xD0D1D2D3U)->last_heard == 0U,
                      "the OK brings her back heard, since she was");

    /* And a link for a node already heard keeps it heard, whatever the roster did meanwhile. */
    const size_t frank_len =
        mesh_test_meshcore_contact(advert, MESH_MESHCORE_PUSH_NEW_ADVERT, 0xE0, "Frank",
                                   MESH_MESHCORE_ADV_CHAT, 0xffU, 1700000900U);
    feed(&protocol, advert, frank_len);
    uint8_t frank[MESH_MESHCORE_PUBKEY_LEN];
    for (size_t i = 0; i < sizeof frank; ++i) {
        frank[i] = (uint8_t)(0xE0U + i);
    }
    MESH_TEST_FAIL_IF(
        mesh_meshcore_import_contact(&g_meshcore, frank, "Frank", MESH_MESHCORE_ADV_CHAT) != 1,
        "Frank's link is asked");
    MESH_TEST_FAIL_IF(mesh_session_model_drop_node(g_meshcore.model, 0xE0E1E2E3U) != 0,
                      "and he is dropped while it waits");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(model_node(0xE0E1E2E3U) == NULL || model_node(0xE0E1E2E3U)->last_heard == 0U,
                      "the OK brings him back heard, since he was");

    /* An advert under the same four bytes, heard while a link's add waits, keeps its row. */
    uint8_t gina[MESH_MESHCORE_PUBKEY_LEN];
    for (size_t i = 0; i < sizeof gina; ++i) {
        gina[i] = (uint8_t)(0xA0U + i);
    }
    MESH_TEST_FAIL_IF(
        mesh_meshcore_import_contact(&g_meshcore, gina, "Gina", MESH_MESHCORE_ADV_CHAT) != 1,
        "Gina's link is asked");
    const size_t henry_len =
        mesh_test_meshcore_contact(advert, MESH_MESHCORE_PUSH_NEW_ADVERT, 0xA0, "Henry",
                                   MESH_MESHCORE_ADV_CHAT, 0xffU, 1700001100U);
    advert[32] ^= 0xFFU; /* the same first four bytes, a different key */
    feed(&protocol, advert, henry_len);
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(model_node(0xA0A1A2A3U) == NULL ||
                          strcmp(model_node(0xA0A1A2A3U)->long_name, "Henry") != 0 ||
                          model_node(0xA0A1A2A3U)->public_key[31] !=
                              (uint8_t)((0xA0U + 31U) ^ 0xFFU),
                      "Henry's row is not written over by Gina's OK");

    /* And the other way round: Henry, heard while Gina's add waits, is not asked under her
       number - by link or as a heard node. */
    uint8_t ivy[MESH_MESHCORE_PUBKEY_LEN];
    for (size_t i = 0; i < sizeof ivy; ++i) {
        ivy[i] = (uint8_t)(0xB8U + i);
    }
    MESH_TEST_FAIL_IF(
        mesh_meshcore_import_contact(&g_meshcore, ivy, "Ivy", MESH_MESHCORE_ADV_CHAT) != 1,
        "Ivy's link is asked");
    const size_t jack_len =
        mesh_test_meshcore_contact(advert, MESH_MESHCORE_PUSH_NEW_ADVERT, 0xB8, "Jack",
                                   MESH_MESHCORE_ADV_CHAT, 0xffU, 1700001200U);
    advert[32] ^= 0xFFU;
    feed(&protocol, advert, jack_len);
    uint8_t jack[MESH_MESHCORE_PUBKEY_LEN];
    memcpy(jack, ivy, sizeof jack);
    jack[31] ^= 0xFFU;
    MESH_TEST_FAIL_IF(mesh_meshcore_import_contact(&g_meshcore, jack, "Jack",
                                                   MESH_MESHCORE_ADV_CHAT) != -EADDRINUSE ||
                          mesh_meshcore_add_contact(&g_meshcore, 0xB8B9BABBU) != -EADDRINUSE,
                      "a heard key under a number an add is waiting on is not asked again");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    record_success(test_name);
}

/* Cayenne LPP as MeshCore writes it: the battery on channel 1, sensors after, a sensor's
   temperature over the MCU's, and a type it does not know stops the read. */
MESH_TEST_CASE(meshcore_lpp_reads_what_a_node_reports, unit) {
    static const uint8_t k_lpp[] = {
        0x01, 116,  0x01, 0x9A,       /* ch1 voltage 4.10 V */
        0x01, 103,  0x00, 0xFA,       /* ch1 MCU temperature 25.0 C */
        0x02, 103,  0xFF, 0x9C,       /* ch2 sensor temperature -10.0 C */
        0x02, 104,  0x5A,             /* ch2 humidity 45 % */
        0x02, 115,  0x27, 0x9B,       /* ch2 pressure 1013.9 hPa */
        0x03, 136,  0x05, 0xC3, 0x8C, /* ch3 GPS lat 37.7740 */
        0xED, 0x51, 0xFC,             /*         lon -122.4196 */
        0x00, 0x03, 0xE8,             /*         alt 10.00 m */
        0x04, 200,  0x01,             /* a type this reader does not know */
        0x02, 116,  0x01, 0x00,       /* never reached */
    };
    struct mesh_meshcore_telemetry t;
    MESH_TEST_FAIL_IF(mesh_meshcore_decode_lpp(k_lpp, sizeof k_lpp, &t) != 0, "decodes");
    MESH_TEST_FAIL_IF(!t.has_battery || t.battery_v < 4.09f || t.battery_v > 4.11f,
                      "the battery is channel 1's voltage");
    MESH_TEST_FAIL_IF(!t.has_temperature || t.temperature_c > -9.9f || t.temperature_c < -10.1f,
                      "the sensor's temperature wins over the MCU's");
    MESH_TEST_FAIL_IF(!t.has_humidity || t.humidity_pct != 45.0f, "humidity in half percent");
    MESH_TEST_FAIL_IF(!t.has_pressure || t.pressure_hpa < 1013.8f || t.pressure_hpa > 1014.0f,
                      "pressure in tenths of a hPa");
    MESH_TEST_FAIL_IF(!t.has_position || t.latitude_e7 != 377740000 ||
                          t.longitude_e7 != -1224196000 || t.altitude_m != 10,
                      "GPS in three signed bytes each");
    MESH_TEST_FAIL_IF(t.has_voltage, "nothing after an unknown type is read");
    MESH_TEST_FAIL_IF(mesh_meshcore_decode_lpp(k_lpp, 3U, &t) != 0 || t.has_battery,
                      "a truncated value is not read");
    static const uint8_t k_offworld[] = {0x03, 136, 0x7F, 0xFF, 0xFF, 0x80, 0x00, 0x00, 0, 0, 0};
    MESH_TEST_FAIL_IF(mesh_meshcore_decode_lpp(k_offworld, sizeof k_offworld, &t) != 0 ||
                          t.has_position,
                      "a fix off the globe is not a position");
    static const uint8_t k_current[] = {0x02, 117, 0xFF, 0x06}; /* -0.250 A */
    MESH_TEST_FAIL_IF(mesh_meshcore_decode_lpp(k_current, sizeof k_current, &t) != 0 ||
                          !t.has_current || t.current_a > -0.249f || t.current_a < -0.251f,
                      "current in signed thousandths of an amp");
    /* What a Heltec V3 on v1.17.1 answered about itself: battery and MCU temperature only. */
    static const uint8_t k_heltec[] = {0x01, 0x74, 0x01, 0x9d, 0x01, 0x67, 0x01, 0x86};
    MESH_TEST_FAIL_IF(mesh_meshcore_decode_lpp(k_heltec, sizeof k_heltec, &t) != 0 ||
                          !t.has_battery || t.battery_v < 4.12f || t.battery_v > 4.14f ||
                          !t.has_temperature || t.temperature_c != 39.0f || t.has_position,
                      "a real radio's answer reads as 4.13 V and 39.0 C");
    record_success(test_name);
}

/* A telemetry request names a contact by its whole key; the answer lands on its record. */
MESH_TEST_CASE(meshcore_telemetry_request_fills_the_node, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const uint32_t alice = 0x40414243U;
    MESH_TEST_FAIL_IF(mesh_meshcore_request_telemetry(&g_meshcore, g_meshcore.self_node) != -EINVAL,
                      "this radio is not asked");
    MESH_TEST_FAIL_IF(mesh_meshcore_request_telemetry(&g_meshcore, 0x12345678U) != -ENOENT,
                      "a node the radio does not carry is not asked");
    const size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_request_telemetry(&g_meshcore, alice) != 0, "Alice is asked");
    const uint8_t *frame = wire.frames[before];
    MESH_TEST_FAIL_IF(wire.lens[before] != 36U ||
                          frame[0] != MESH_MESHCORE_CMD_SEND_TELEMETRY_REQ || frame[1] != 0U ||
                          frame[4] != 0x40 || frame[35] != 0x40 + 31,
                      "by three reserved bytes and her whole key");
    static const uint8_t k_sent[10] = {MESH_MESHCORE_RESP_SENT, 0, 1, 2, 3, 4, 0x88, 0x13, 0, 0};
    feed(&protocol, k_sent, sizeof k_sent);
    static const uint8_t k_push[] = {
        MESH_MESHCORE_PUSH_TELEMETRY_RESPONSE,
        0x00,
        0x40,
        0x41,
        0x42,
        0x43,
        0x44,
        0x45,
        0x01,
        116,
        0x01,
        0x72, /* 3.70 V */
        0x02,
        103,
        0x00,
        0xC8, /* 20.0 C */
    };
    const uint32_t heard_before = model_node(alice)->last_heard;
    g_meshcore.radio_clock = heard_before + 600U;
    g_meshcore.radio_clock_at_ms = inkwell_time_monotonic_ms();
    feed(&protocol, k_push, sizeof k_push);
    const struct mesh_node_summary *node = model_node(alice);
    MESH_TEST_FAIL_IF(node->last_heard <= heard_before, "an answer is the node heard from, now");
    MESH_TEST_FAIL_IF(!node->metrics.valid || !node->metrics.has_voltage ||
                          node->metrics.voltage < 3.69f || node->metrics.voltage > 3.71f,
                      "the battery lands in the node's metrics");
    MESH_TEST_FAIL_IF(!node->environment.valid || !node->environment.has_temperature ||
                          node->environment.temperature != 20.0f || node->environment.has_humidity,
                      "the temperature in its environment, and nothing it did not send");
    MESH_TEST_FAIL_IF(g_meshcore.request_until_ms != 0U,
                      "the answer frees the radio for the next request");
    static const uint8_t k_amps[] = {
        MESH_MESHCORE_PUSH_TELEMETRY_RESPONSE,
        0x00,
        0x40,
        0x41,
        0x42,
        0x43,
        0x44,
        0x45,
        0x02,
        117,
        0x00,
        0xFA, /* 0.250 A */
    };
    feed(&protocol, k_amps, sizeof k_amps);
    MESH_TEST_FAIL_IF(!node->environment.has_current || node->environment.current < 249.9f ||
                          node->environment.current > 250.1f,
                      "current lands in milliamps, as the record keeps it");
    MESH_TEST_FAIL_IF(node->environment.has_temperature,
                      "and a reading the node no longer sends is not kept as fresh");
    static const uint8_t k_null_island[] = {
        MESH_MESHCORE_PUSH_TELEMETRY_RESPONSE,
        0x00,
        0x40,
        0x41,
        0x42,
        0x43,
        0x44,
        0x45,
        0x03,
        136,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    };
    feed(&protocol, k_null_island, sizeof k_null_island);
    MESH_TEST_FAIL_IF(!node->position.valid || node->position.latitude_i != 0 ||
                          node->position.longitude_i != 0,
                      "a fix at 0, 0 reported as one is a position");

    /* One outstanding: a second before the first's answer or deadline would orphan it. */
    MESH_TEST_FAIL_IF(mesh_meshcore_request_telemetry(&g_meshcore, alice) != 0, "asked again");
    MESH_TEST_FAIL_IF(mesh_meshcore_request_telemetry(&g_meshcore, alice) != -EBUSY,
                      "a second waits while the first is still on its way to the radio");
    feed(&protocol, k_sent, sizeof k_sent);
    MESH_TEST_FAIL_IF(mesh_meshcore_request_telemetry(&g_meshcore, alice) != -EBUSY,
                      "and a third waits for the second's answer");
    /* A late answer from someone else leaves the current request its deadline. */
    static const uint8_t k_stranger[] = {
        MESH_MESHCORE_PUSH_TELEMETRY_RESPONSE,
        0x00,
        0x99,
        0x98,
        0x97,
        0x96,
        0x95,
        0x94,
        0x01,
        116,
        0x01,
        0x72,
    };
    feed(&protocol, k_stranger, sizeof k_stranger);
    MESH_TEST_FAIL_IF(mesh_meshcore_request_telemetry(&g_meshcore, alice) != -EBUSY,
                      "only the asked node's answer frees the radio");
    g_meshcore.request_until_ms = 1U; /* the deadline long past */
    MESH_TEST_FAIL_IF(mesh_meshcore_request_telemetry(&g_meshcore, alice) != 0,
                      "an answer that never came frees it at its deadline");
    /* An answer ahead of the radio's SENT leaves nothing to wait for once the SENT comes. */
    feed(&protocol, k_push, sizeof k_push);
    feed(&protocol, k_sent, sizeof k_sent);
    MESH_TEST_FAIL_IF(g_meshcore.request_until_ms != 0U,
                      "a SENT after its own answer does not lock the radio again");

    /* A retry still queued behind another command: an answer then is the last request's, and
       the retry's own SENT still arms its deadline. */
    MESH_TEST_FAIL_IF(mesh_meshcore_send_advert(&g_meshcore, false) != 0, "something ahead");
    MESH_TEST_FAIL_IF(mesh_meshcore_request_telemetry(&g_meshcore, alice) != 0, "the retry");
    feed(&protocol, k_push, sizeof k_push);
    feed_code(&protocol, MESH_MESHCORE_RESP_OK); /* the advert's; the retry goes out now */
    feed(&protocol, k_sent, sizeof k_sent);
    MESH_TEST_FAIL_IF(g_meshcore.request_until_ms == 0U,
                      "an earlier answer does not stand in for a request not yet written");
    record_success(test_name);
}

/* A login is the key and the password, answered by the node asked - and it is the same one
   request as a telemetry request, since the radio keeps one pending and a new one orphans it. */
MESH_TEST_CASE(meshcore_login_is_answered_and_shares_the_lock, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const uint32_t alice = 0x40414243U;
    MESH_TEST_FAIL_IF(mesh_meshcore_login(&g_meshcore, alice, "0123456789abcdef") != -EINVAL,
                      "a password longer than the node's prefs hold is refused");
    MESH_TEST_FAIL_IF(mesh_meshcore_login(&g_meshcore, 0x12345678U, "") != -ENOENT,
                      "a node the radio does not carry is not logged in to");
    wire.count = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_login(&g_meshcore, alice, "hunter2") != 0, "Alice is asked");
    MESH_TEST_FAIL_IF(wire.lens[0] != 1U + 32U + 7U ||
                          wire.frames[0][0] != MESH_MESHCORE_CMD_SEND_LOGIN ||
                          wire.frames[0][1] != 0x40 || wire.frames[0][32] != 0x40 + 31 ||
                          memcmp(wire.frames[0] + 33, "hunter2", 7U) != 0,
                      "by her whole key and the password, unterminated");
    MESH_TEST_FAIL_IF(mesh_meshcore_request_telemetry(&g_meshcore, alice) != -EBUSY,
                      "a telemetry request waits behind a login");
    static const uint8_t k_sent[10] = {MESH_MESHCORE_RESP_SENT, 0, 1, 2, 3, 4, 0x88, 0x13, 0, 0};
    feed(&protocol, k_sent, sizeof k_sent);
    MESH_TEST_FAIL_IF(g_meshcore.request_until_ms == 0U, "its answer is due by the deadline");

    const uint32_t notices = g_meshcore.notices;
    static const uint8_t k_stranger[] = {
        MESH_MESHCORE_PUSH_LOGIN_SUCCESS, 1, 0x99, 0x98, 0x97, 0x96, 0x95, 0x94};
    feed(&protocol, k_stranger, sizeof k_stranger);
    MESH_TEST_FAIL_IF(g_meshcore.notices != notices || g_meshcore.request_until_ms == 0U,
                      "someone else's answer is not hers");
    static const uint8_t k_admin[] = {
        MESH_MESHCORE_PUSH_LOGIN_SUCCESS, 1, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0, 0, 0, 0, 3, 1};
    feed(&protocol, k_admin, sizeof k_admin);
    MESH_TEST_FAIL_IF(g_meshcore.notices != notices + 1U || g_meshcore.notice.node_id != alice ||
                          g_meshcore.notice.cmd != MESH_MESHCORE_CMD_SEND_LOGIN ||
                          g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_ADMIN,
                      "hers says she took us as her admin");
    MESH_TEST_FAIL_IF(g_meshcore.request_until_ms != 0U, "and frees the radio");
    feed(&protocol, k_admin, sizeof k_admin);
    MESH_TEST_FAIL_IF(g_meshcore.notices != notices + 1U, "a request ends once");

    /* A guest, then a refusal, then silence. */
    MESH_TEST_FAIL_IF(mesh_meshcore_login(&g_meshcore, alice, "") != 0, "a guest login");
    MESH_TEST_FAIL_IF(wire.lens[wire.count - 1U] != 33U, "is the key alone");
    feed(&protocol, k_sent, sizeof k_sent);
    static const uint8_t k_guest[] = {
        MESH_MESHCORE_PUSH_LOGIN_SUCCESS, 0, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45};
    feed(&protocol, k_guest, sizeof k_guest);
    MESH_TEST_FAIL_IF(g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_GUEST, "is a guest");
    MESH_TEST_FAIL_IF(mesh_meshcore_login(&g_meshcore, alice, "wrong") != 0, "a wrong password");
    feed(&protocol, k_sent, sizeof k_sent);
    static const uint8_t k_fail[] = {
        MESH_MESHCORE_PUSH_LOGIN_FAIL, 0, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45};
    feed(&protocol, k_fail, sizeof k_fail);
    MESH_TEST_FAIL_IF(g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_REFUSED, "is refused");
    MESH_TEST_FAIL_IF(mesh_meshcore_request_telemetry(&g_meshcore, alice) != 0, "readings");
    feed(&protocol, k_sent, sizeof k_sent);
    const uint32_t before_silence = g_meshcore.notices;
    mesh_protocol_tick(&protocol, g_meshcore.request_until_ms);
    MESH_TEST_FAIL_IF(g_meshcore.notices != before_silence + 1U ||
                          g_meshcore.notice.cmd != MESH_MESHCORE_CMD_SEND_TELEMETRY_REQ ||
                          g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_SILENT ||
                          g_meshcore.request_until_ms != 0U,
                      "nothing by the deadline is said so, and frees the radio");

    /* The radio refusing to send it ends it too. */
    MESH_TEST_FAIL_IF(mesh_meshcore_login(&g_meshcore, alice, "") != 0, "asked once more");
    static const uint8_t k_err[] = {MESH_MESHCORE_RESP_ERR, 3};
    feed(&protocol, k_err, sizeof k_err);
    MESH_TEST_FAIL_IF(g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_UNSENT ||
                          mesh_meshcore_login(&g_meshcore, alice, "") != 0,
                      "a refusal to send is said so, and frees the radio");
    feed(&protocol, k_err, sizeof k_err);

    /* A SENT too short to read names no deadline, so it ends the request rather than leave it
       open with nothing to expire it. */
    MESH_TEST_FAIL_IF(mesh_meshcore_login(&g_meshcore, alice, "") != 0, "asked again");
    static const uint8_t k_short_sent[] = {MESH_MESHCORE_RESP_SENT, 0, 1};
    feed(&protocol, k_short_sent, sizeof k_short_sent);
    MESH_TEST_FAIL_IF(g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_UNSENT ||
                          g_meshcore.request_cmd != 0U,
                      "an unreadable SENT ends the request as not sent");

    /* A password does not outlive its frame in the queue. */
    MESH_TEST_FAIL_IF(mesh_meshcore_login(&g_meshcore, alice, "hunter2") != 0, "a password");
    feed(&protocol, k_sent, sizeof k_sent);
    for (size_t i = 0; i < MESH_MESHCORE_QUEUE_LEN; ++i) {
        const struct mesh_meshcore_request *slot = &g_meshcore.queue[i];
        for (size_t at = 0; at + 7U <= sizeof slot->frame; ++at) {
            MESH_TEST_FAIL_IF(memcmp(slot->frame + at, "hunter2", 7U) == 0,
                              "the queue keeps no copy of a password once it is answered");
        }
    }
    feed(&protocol, k_guest, sizeof k_guest);

    /* Queued behind another command, a login the link then refuses to take was never asked. */
    MESH_TEST_FAIL_IF(mesh_meshcore_send_advert(&g_meshcore, false) != 0, "something ahead");
    MESH_TEST_FAIL_IF(mesh_meshcore_login(&g_meshcore, alice, "") != 0, "queued behind it");
    wire.refuse = true;
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    wire.refuse = false;
    MESH_TEST_FAIL_IF(g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_UNSENT ||
                          mesh_meshcore_login(&g_meshcore, alice, "") != 0,
                      "a write the link refused ends the request, and frees the radio");

    /* And the link dropping while one is answered ends it too, rather than leaving it open. */
    feed(&protocol, k_sent, sizeof k_sent);
    const uint32_t before_drop = g_meshcore.notices;
    mesh_protocol_detach(&protocol);
    MESH_TEST_FAIL_IF(g_meshcore.notices != before_drop + 1U ||
                          g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_SILENT ||
                          g_meshcore.request_cmd != 0U,
                      "a request the link took with it is said to have gone unanswered");
    record_success(test_name);
}

/* A status push: 0x87, a reserved byte, the node's key prefix, then its stats. */
static size_t status_push(uint8_t *out, size_t tail) {
    memset(out, 0, 8U + MESH_MESHCORE_STATUS_LEN + 8U);
    out[0] = MESH_MESHCORE_PUSH_STATUS_RESPONSE;
    static const uint8_t k_alice[6] = {0x40, 0x41, 0x42, 0x43, 0x44, 0x45};
    memcpy(out + 2, k_alice, sizeof k_alice);
    uint8_t *stats = out + 8;
    stats[0] = 0x10; /* 4112 mV */
    stats[1] = 0x10;
    stats[2] = 2;    /* queue */
    stats[4] = 0x8A; /* noise floor -118 */
    stats[5] = 0xFF;
    stats[6] = 0xA6; /* last RSSI -90 */
    stats[7] = 0xFF;
    stats[8] = 0x70; /* 70000 received */
    stats[9] = 0x11;
    stats[10] = 0x01;
    stats[20] = 0x80; /* up 3200 s... */
    stats[21] = 0x0C;
    stats[42] = 0xE7; /* last SNR -25/4 */
    stats[43] = 0xFF;
    stats[46] = 5; /* flood dups */
    for (size_t i = 0; i < tail; ++i) {
        stats[MESH_MESHCORE_STATUS_LEN + i] = (uint8_t)(i + 1U);
    }
    return 8U + MESH_MESHCORE_STATUS_LEN + tail;
}

/* A repeater's or room server's status is SEND_STATUS_REQ by key, answered by a push whose
   counters land on the node - its kind choosing which tail to read - under the one lock every
   request to another node shares. A repeater answers only a client logged in to it, so a
   status request that meets silence is the one said to need a login first. */
MESH_TEST_CASE(meshcore_status_lands_on_the_node, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const uint32_t alice = 0x40414243U;
    struct mesh_node_summary *node = mesh_session_model_node(g_meshcore.model, alice, false);
    MESH_TEST_FAIL_IF(node == NULL, "Alice is on the roster");
    node->role = 4U; /* a repeater's advert */
    MESH_TEST_FAIL_IF(mesh_meshcore_request_status(&g_meshcore, 0x12345678U) != -ENOENT,
                      "a node the radio does not carry is not asked");
    wire.count = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_request_status(&g_meshcore, alice) != 0, "Alice is asked");
    MESH_TEST_FAIL_IF(wire.lens[0] != 33U ||
                          wire.frames[0][0] != MESH_MESHCORE_CMD_SEND_STATUS_REQ ||
                          wire.frames[0][1] != 0x40 || wire.frames[0][32] != 0x40 + 31,
                      "by her whole key");
    MESH_TEST_FAIL_IF(mesh_meshcore_login(&g_meshcore, alice, "") != -EBUSY,
                      "a login waits behind a status request");
    static const uint8_t k_sent[10] = {MESH_MESHCORE_RESP_SENT, 0, 1, 2, 3, 4, 0x88, 0x13, 0, 0};
    feed(&protocol, k_sent, sizeof k_sent);

    static uint8_t push[8U + MESH_MESHCORE_STATUS_LEN + 8U];
    const uint32_t notices = g_meshcore.notices;
    feed(&protocol, push, status_push(push, 0U) - 1U);
    MESH_TEST_FAIL_IF(node->relay.valid || g_meshcore.notices != notices + 1U,
                      "a short one is not read, but is still her answer");
    MESH_TEST_FAIL_IF(mesh_meshcore_request_status(&g_meshcore, alice) != 0, "asked again");
    feed(&protocol, k_sent, sizeof k_sent);
    feed(&protocol, push, status_push(push, 8U));
    MESH_TEST_FAIL_IF(g_meshcore.notice.cmd != MESH_MESHCORE_CMD_SEND_STATUS_REQ ||
                          g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_STATUS ||
                          g_meshcore.request_cmd != 0U,
                      "her answer ends the request");
    const struct mesh_node_relay *relay = &node->relay;
    MESH_TEST_FAIL_IF(!relay->valid || relay->noise_floor != -118 || relay->last_rssi != -90 ||
                          relay->last_snr != -6.25f || relay->packets_recv != 70000U ||
                          relay->uptime_seconds != 3200U || relay->tx_queue_len != 2U ||
                          relay->flood_dups != 5U,
                      "and her counters land on her record");
    MESH_TEST_FAIL_IF(!relay->has_rx_air_time || relay->rx_air_time_secs != 0x04030201U ||
                          !relay->has_recv_errors || relay->recv_errors != 0x08070605U ||
                          relay->has_posts,
                      "a repeater's tail is its receive airtime and errors");
    MESH_TEST_FAIL_IF(!node->metrics.has_voltage || node->metrics.voltage < 4.111f ||
                          node->metrics.voltage > 4.113f,
                      "and her battery lands where every node's does");

    /* The same bytes from a room server are its posts. */
    node->role = 12U; /* a room server's */
    MESH_TEST_FAIL_IF(mesh_meshcore_request_status(&g_meshcore, alice) != 0, "a room server");
    feed(&protocol, k_sent, sizeof k_sent);
    feed(&protocol, push, status_push(push, 4U));
    MESH_TEST_FAIL_IF(!relay->has_posts || relay->posted != 0x0201U ||
                          relay->post_pushes != 0x0403U || relay->has_rx_air_time,
                      "a room server's tail is its posts, and the last answer's tail is gone");

    /* Silence, which is what a repeater that has not logged us in says. */
    MESH_TEST_FAIL_IF(mesh_meshcore_request_status(&g_meshcore, alice) != 0, "asked unheard");
    feed(&protocol, k_sent, sizeof k_sent);
    mesh_protocol_tick(&protocol, g_meshcore.request_until_ms);
    MESH_TEST_FAIL_IF(g_meshcore.notice.cmd != MESH_MESHCORE_CMD_SEND_STATUS_REQ ||
                          g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_SILENT,
                      "nothing by the deadline is said so");
    record_success(test_name);
}

/* MeshCore's traceroute is a path discovery: PATH_DISCOVERY_REQ by key, flooded, answered with
   the path out and the path back as repeater key prefixes - each named from the roster where
   exactly one node answers to it and kept as its bytes where none or several do - into the
   model's traceroute, under the one lock every request to another node shares. */
MESH_TEST_CASE(meshcore_path_discovery_is_the_traceroute, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const uint32_t alice = 0x40414243U;
    /* A second node under Alice's first byte, so that byte alone names nobody. */
    struct mesh_node_summary *twin = mesh_session_model_node(g_meshcore.model, 0x40999999U, false);
    MESH_TEST_FAIL_IF(twin == NULL, "a node shares Alice's first byte");
    twin->public_key_len = 32U;
    memset(twin->public_key, 0x99, 32U);
    twin->public_key[0] = 0x40;

    MESH_TEST_FAIL_IF(mesh_meshcore_discover_path(&g_meshcore, 0x12345678U) != -ENOENT,
                      "a node the radio does not carry is not traced");
    wire.count = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_discover_path(&g_meshcore, alice) != 0, "Alice is traced");
    MESH_TEST_FAIL_IF(
        wire.lens[0] != 34U || wire.frames[0][0] != MESH_MESHCORE_CMD_SEND_PATH_DISCOVERY_REQ ||
            wire.frames[0][1] != 0U || wire.frames[0][2] != 0x40 || wire.frames[0][33] != 0x40 + 31,
        "by a reserved zero and her whole key");
    const struct mesh_traceroute *trace = &g_meshcore.model->traceroute;
    MESH_TEST_FAIL_IF(trace->state != MESH_TRACEROUTE_PENDING || trace->target != alice,
                      "the trace is running");
    MESH_TEST_FAIL_IF(mesh_meshcore_request_status(&g_meshcore, alice) != -EBUSY,
                      "and holds the lock");
    static const uint8_t k_sent[10] = {MESH_MESHCORE_RESP_SENT, 1, 1, 2, 3, 4, 0x30, 0x75, 0, 0};
    feed(&protocol, k_sent, sizeof k_sent);

    /* Out: two one-byte hops, one nobody's and one both Alice's and her twin's. Back: two
       two-byte hops, one only Alice's and one nobody's. */
    static const uint8_t k_route[] = {MESH_MESHCORE_PUSH_PATH_DISCOVERY_RESPONSE,
                                      0,
                                      0x40,
                                      0x41,
                                      0x42,
                                      0x43,
                                      0x44,
                                      0x45,
                                      0x02,
                                      0xEE,
                                      0x40,
                                      0x42,
                                      0x40,
                                      0x41,
                                      0xAB,
                                      0xCD};
    feed(&protocol, k_route, sizeof k_route);
    MESH_TEST_FAIL_IF(g_meshcore.notice.cmd != MESH_MESHCORE_CMD_SEND_PATH_DISCOVERY_REQ ||
                          g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_ROUTE ||
                          g_meshcore.request_cmd != 0U,
                      "her answer ends the request");
    MESH_TEST_FAIL_IF(trace->state != MESH_TRACEROUTE_DONE || trace->target != alice ||
                          trace->completed == 0U,
                      "and finishes the trace");
    MESH_TEST_FAIL_IF(trace->route_count != 2U || trace->hash_size != 1U || trace->route[0] != 0U ||
                          trace->route_hash[0][0] != 0xEE || trace->route[1] != 0U ||
                          trace->route_hash[1][0] != 0x40,
                      "a byte nobody or several answer to is kept as its byte");
    MESH_TEST_FAIL_IF(trace->back_count != 2U || trace->back_hash_size != 2U ||
                          trace->route_back[0] != alice || trace->route_back[1] != 0U ||
                          trace->back_hash[1][0] != 0xAB || trace->back_hash[1][1] != 0xCD,
                      "and the way back names its hops at its own width");

    /* A width the firmware reserves is unreadable, and ends the trace rather than guessing. */
    MESH_TEST_FAIL_IF(mesh_meshcore_discover_path(&g_meshcore, alice) != 0, "traced again");
    feed(&protocol, k_sent, sizeof k_sent);
    static const uint8_t k_reserved[] = {MESH_MESHCORE_PUSH_PATH_DISCOVERY_RESPONSE,
                                         0,
                                         0x40,
                                         0x41,
                                         0x42,
                                         0x43,
                                         0x44,
                                         0x45,
                                         0xC1,
                                         1,
                                         2,
                                         3,
                                         4,
                                         0};
    feed(&protocol, k_reserved, sizeof k_reserved);
    MESH_TEST_FAIL_IF(trace->state != MESH_TRACEROUTE_TIMEOUT || g_meshcore.request_cmd != 0U,
                      "a reserved width is not read");

    /* Silence is a trace that timed out. */
    MESH_TEST_FAIL_IF(mesh_meshcore_discover_path(&g_meshcore, alice) != 0, "traced unheard");
    feed(&protocol, k_sent, sizeof k_sent);
    mesh_protocol_tick(&protocol, g_meshcore.request_until_ms);
    MESH_TEST_FAIL_IF(g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_SILENT ||
                          trace->state != MESH_TRACEROUTE_TIMEOUT,
                      "nothing by the deadline times the trace out");
    record_success(test_name);
}

/* A message to a repeater is a command: CLI_DATA rather than text, with no ack to wait for.
   The repeater's reply is the answer - it settles the command and lands in the conversation -
   and a command nobody answered is failed without being sent again, since it may have run. */
MESH_TEST_CASE(meshcore_a_repeater_is_sent_commands, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const uint32_t alice = 0x40414243U;
    struct mesh_node_summary *node = mesh_session_model_node(g_meshcore.model, alice, false);
    MESH_TEST_FAIL_IF(node == NULL, "Alice is on the roster");

    uint32_t packet_id = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_is_command_peer(&g_meshcore, alice) ||
                          mesh_meshcore_send_text(&g_meshcore, alice, 0U, "hi", &packet_id) != 0 ||
                          wire.frames[wire.count - 1U][1] != MESH_MESHCORE_TXT_PLAIN,
                      "a companion is sent text");
    node->role = 4U; /* a repeater's advert */
    MESH_TEST_FAIL_IF(!mesh_meshcore_is_command_peer(&g_meshcore, alice) ||
                          mesh_meshcore_is_command_peer(&g_meshcore, MESH_MESSAGE_BROADCAST_ADDR),
                      "a repeater is sent commands; a channel never is");
    feed_code(&protocol, MESH_MESHCORE_RESP_ERR); /* the message above, out of the way */

    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, alice, 0U, "get radio", &packet_id) != 0,
                      "a command goes out");
    const uint8_t *frame = wire.frames[wire.count - 1U];
    MESH_TEST_FAIL_IF(frame[0] != MESH_MESHCORE_CMD_SEND_TXT_MSG ||
                          frame[1] != MESH_MESHCORE_TXT_CLI_DATA || frame[2] != 0U ||
                          frame[7] != 0x40 || memcmp(frame + 13, "get radio", 9U) != 0,
                      "as CLI data, to Alice's prefix");
    uint8_t sent[10] = {MESH_MESHCORE_RESP_SENT, 0};
    mesh_test_put_u32(sent + 6, 1000U); /* no ack: the four bytes stay zero */
    feed(&protocol, sent, sizeof sent);
    uint8_t confirmed[5] = {MESH_MESHCORE_PUSH_SEND_CONFIRMED};
    feed(&protocol, confirmed, sizeof confirmed);
    MESH_TEST_FAIL_IF(mesh_message_log_find(&g_model.messages, packet_id)->ack !=
                          MESH_MESSAGE_ACK_PENDING,
                      "the zero a command's SENT named is not an ack to match");

    const uint8_t reply[] = {16, 20, 0,   0,   0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0xFF, 1,   0,  0,
                             0,  0,  '>', ' ', '8',  '6',  '9',  '.',  '5',  ',',  '2',  '5', '0'};
    feed_code(&protocol, MESH_MESHCORE_PUSH_MSG_WAITING); /* the reply waits in the radio */
    feed(&protocol, reply, sizeof reply);
    MESH_TEST_FAIL_IF(mesh_message_log_find(&g_model.messages, packet_id)->ack !=
                          MESH_MESSAGE_ACK_DELIVERED,
                      "the repeater's reply settles the command");
    const struct mesh_message *answer = newest_message();
    MESH_TEST_FAIL_IF(answer == NULL || answer->from != alice ||
                          answer->direction != MESH_MESSAGE_INBOUND ||
                          strcmp(answer->text, "> 869.5,250") != 0,
                      "and is the answer in her conversation");

    feed_code(&protocol, MESH_MESHCORE_RESP_NO_MORE_MESSAGES);

    /* A command queued behind a sync is not answered by an older reply the sync brings out of
       the radio's queue: it has not been sent yet. */
    feed_code(&protocol, MESH_MESHCORE_PUSH_MSG_WAITING);
    uint32_t queued_id = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, alice, 0U, "clock", &queued_id) != 0,
                      "a command queues behind the sync");
    feed(&protocol, reply, sizeof reply);
    MESH_TEST_FAIL_IF(mesh_message_log_find(&g_model.messages, queued_id)->ack !=
                          MESH_MESSAGE_ACK_PENDING,
                      "an older reply does not settle a command not yet sent");
    feed(&protocol, sent, sizeof sent); /* the command goes, then the sync asked after it */
    feed_code(&protocol, MESH_MESHCORE_RESP_NO_MORE_MESSAGES);
    feed_code(&protocol, MESH_MESHCORE_PUSH_MSG_WAITING);
    feed(&protocol, reply, sizeof reply);
    MESH_TEST_FAIL_IF(mesh_message_log_find(&g_model.messages, queued_id)->ack !=
                          MESH_MESSAGE_ACK_DELIVERED,
                      "its own reply, once it was sent, does");
    feed_code(&protocol, MESH_MESHCORE_RESP_NO_MORE_MESSAGES);

    /* Two waiting on one repeater: its first reply answers the first sent. Packet ids are a
       xorshift, so the generator is seeded where the first id is the larger - the order the
       ids alone would get backwards. */
    uint32_t seed = 1U;
    for (;; ++seed) {
        g_model.next_packet_id = seed;
        const uint32_t a = mesh_session_next_packet_id(&g_model);
        const uint32_t b = mesh_session_next_packet_id(&g_model);
        if (a > b) {
            break;
        }
    }
    g_model.next_packet_id = seed;
    uint32_t first_id = 0U;
    uint32_t second_id = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, alice, 0U, "clock", &first_id) != 0 ||
                          mesh_meshcore_send_text(&g_meshcore, alice, 0U, "ver", &second_id) != 0 ||
                          first_id <= second_id,
                      "two commands go out, the first under the larger id");
    feed(&protocol, sent, sizeof sent);
    feed(&protocol, sent, sizeof sent);
    feed_code(&protocol, MESH_MESHCORE_PUSH_MSG_WAITING);
    feed(&protocol, reply, sizeof reply);
    MESH_TEST_FAIL_IF(
        mesh_message_log_find(&g_model.messages, first_id)->ack != MESH_MESSAGE_ACK_DELIVERED ||
            mesh_message_log_find(&g_model.messages, second_id)->ack != MESH_MESSAGE_ACK_PENDING,
        "the first reply settles the first command sent, not the lower id");
    feed(&protocol, reply, sizeof reply);
    MESH_TEST_FAIL_IF(mesh_message_log_find(&g_model.messages, second_id)->ack !=
                          MESH_MESSAGE_ACK_DELIVERED,
                      "and the second, the second");
    feed_code(&protocol, MESH_MESHCORE_RESP_NO_MORE_MESSAGES);

    /* One nobody answers: failed at its deadline, said, and not written again. */
    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, alice, 0U, "reboot", &packet_id) != 0,
                      "a second command goes out");
    feed(&protocol, sent, sizeof sent);
    const size_t written = wire.count;
    const uint32_t notices = g_meshcore.notices;
    mesh_protocol_tick(&protocol, g_meshcore.now_ms + 60000U);
    MESH_TEST_FAIL_IF(mesh_message_log_find(&g_model.messages, packet_id)->ack !=
                          MESH_MESSAGE_ACK_FAILED,
                      "unanswered, it failed");
    MESH_TEST_FAIL_IF(wire.count != written, "and was not sent again");
    MESH_TEST_FAIL_IF(g_meshcore.notices != notices + 1U ||
                          g_meshcore.notice.cmd != MESH_MESHCORE_CMD_SEND_TXT_MSG ||
                          g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_SILENT ||
                          g_meshcore.notice.node_id != alice,
                      "and the silence is said, naming her");

    /* Its reply, late, while a newer command waits: it is the late one's answer - delivered
       after all - and the newer keeps its own place rather than being settled by it. */
    const uint32_t late_id = packet_id;
    uint32_t newer_id = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, alice, 0U, "ver", &newer_id) != 0,
                      "a newer command goes out");
    feed(&protocol, sent, sizeof sent);
    feed_code(&protocol, MESH_MESHCORE_PUSH_MSG_WAITING);
    feed(&protocol, reply, sizeof reply);
    MESH_TEST_FAIL_IF(
        mesh_message_log_find(&g_model.messages, late_id)->ack != MESH_MESSAGE_ACK_DELIVERED ||
            mesh_message_log_find(&g_model.messages, newer_id)->ack != MESH_MESSAGE_ACK_PENDING,
        "a late reply settles the command it answers, not the next one");
    feed_code(&protocol, MESH_MESHCORE_RESP_NO_MORE_MESSAGES);

    /* Past the grace, an unanswered command gives its place up: the next reply is the newer's. */
    mesh_protocol_tick(&protocol, g_meshcore.now_ms + 60000U);
    for (size_t i = 0; i < MESH_MESHCORE_PENDING_SENDS; ++i) {
        MESH_TEST_FAIL_IF(g_meshcore.pending[i].packet_id != 0U,
                          "a command given up on frees its send slot at once");
    }
    mesh_protocol_tick(&protocol, g_meshcore.now_ms + MESH_MESHCORE_COMMAND_LATE_MS + 1U);
    for (size_t i = 0; i < MESH_MESHCORE_PENDING_SENDS; ++i) {
        MESH_TEST_FAIL_IF(g_meshcore.late[i].packet_id != 0U, "and its place in line after");
    }
    record_success(test_name);
}

/* A contact on the roster by a key starting at `lead`, of the kind `role` reads as. */
static struct mesh_node_summary *add_contact(uint32_t id, uint8_t lead, uint32_t role) {
    struct mesh_node_summary *node = mesh_session_model_node(&g_model, id, true);
    if (node != NULL) {
        node->in_nodedb = true;
        node->role = role;
        node->public_key_len = MESH_MESHCORE_PUBKEY_LEN;
        for (size_t k = 0; k < MESH_MESHCORE_PUBKEY_LEN; ++k) {
            node->public_key[k] = (uint8_t)(lead + k);
        }
    }
    return node;
}

/* Every command waiting on two quiet repeaters times out in one tick. Each is said - the
   publish reads them all, not just the last - and none of them keeps a send slot, so a message
   to somebody else still goes while they wait out their grace for a late reply. */
MESH_TEST_CASE(meshcore_quiet_repeaters_neither_hold_slots_nor_lose_notices, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const uint32_t alice = 0x40414243U;
    const uint32_t carol = 0x60616263U;
    const uint32_t bob = 0x50515253U;
    mesh_session_model_node(&g_model, alice, false)->role = 4U;
    MESH_TEST_FAIL_IF(add_contact(carol, 0x60, 4U) == NULL || add_contact(bob, 0x50, 0U) == NULL,
                      "Carol, a repeater, and Bob, a companion, are contacts");

    uint8_t sent[10] = {MESH_MESHCORE_RESP_SENT, 0};
    mesh_test_put_u32(sent + 6, 1000U);
    uint32_t id = 0U;
    for (unsigned i = 0; i < MESH_MESHCORE_PENDING_SENDS; ++i) {
        MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, i % 2U == 0U ? alice : carol, 0U,
                                                  "clock", &id) != 0,
                          "every slot takes a command");
        feed(&protocol, sent, sizeof sent);
    }
    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, bob, 0U, "hi", &id) != -ENOBUFS,
                      "while they wait, the slots are full");

    const uint32_t notices = g_meshcore.notices;
    const uint64_t timed_out = g_meshcore.now_ms + 60000U;
    mesh_protocol_tick(&protocol, timed_out);
    MESH_TEST_FAIL_IF(g_meshcore.notices != notices + MESH_MESHCORE_PENDING_SENDS,
                      "each one that timed out is a notice");
    unsigned said_alice = 0U;
    unsigned said_carol = 0U;
    for (uint32_t n = notices; n != g_meshcore.notices; ++n) {
        const struct mesh_meshcore_notice *notice =
            &g_meshcore.notice_log[n % MESH_MESHCORE_NOTICES_KEPT];
        MESH_TEST_FAIL_IF(notice->cmd != MESH_MESHCORE_CMD_SEND_TXT_MSG ||
                              notice->answer != MESH_MESHCORE_ANSWER_SILENT,
                          "each says a command went unanswered");
        said_alice += notice->node_id == alice ? 1U : 0U;
        said_carol += notice->node_id == carol ? 1U : 0U;
    }
    MESH_TEST_FAIL_IF(said_alice != MESH_MESHCORE_PENDING_SENDS / 2U ||
                          said_carol != MESH_MESHCORE_PENDING_SENDS / 2U,
                      "and every one is kept for the publish, both repeaters' alike");

    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, bob, 0U, "hi", &id) != 0,
                      "a message to Bob goes while theirs wait out the grace");

    /* But while those eight are still in line for a reply, a ninth command is refused rather
       than one of them losing its place - a late reply would otherwise settle the ninth. */
    const size_t logged = g_model.messages.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, alice, 0U, "ver", &id) != -EBUSY ||
                          g_model.messages.count != logged,
                      "a command past the line's length is refused, and not logged");
    /* Timed from the tick that gave them up: a write since reads the real clock afresh. */
    mesh_protocol_tick(&protocol, timed_out + MESH_MESHCORE_COMMAND_LATE_MS + 1U);
    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, alice, 0U, "ver", &id) != 0,
                      "and goes once they have waited out their grace");
    record_success(test_name);
}

/* A repeater's neighbours are SEND_BINARY_REQ by key under the one lock every request shares,
   answered by a push that names no node - only the tag the SENT carried. The list lands on the
   record Meshtastic's NeighborInfo fills, each neighbour by its key's first four bytes. */
MESH_TEST_CASE(meshcore_neighbours_land_on_the_repeater, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const uint32_t alice = 0x40414243U;
    struct mesh_node_summary *node = mesh_session_model_node(g_meshcore.model, alice, false);
    MESH_TEST_FAIL_IF(node == NULL, "Alice is on the roster");
    MESH_TEST_FAIL_IF(mesh_meshcore_request_neighbours(&g_meshcore, alice) != -EINVAL,
                      "a companion keeps no list to ask for");
    node->role = 4U;
    MESH_TEST_FAIL_IF(mesh_meshcore_request_neighbours(&g_meshcore, 0x12345678U) != -ENOENT,
                      "a node the radio does not carry is not asked");
    wire.count = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_request_neighbours(&g_meshcore, alice) != 0, "Alice is asked");
    const uint8_t *frame = wire.frames[0];
    MESH_TEST_FAIL_IF(
        wire.lens[0] != 44U || frame[0] != MESH_MESHCORE_CMD_SEND_BINARY_REQ || frame[1] != 0x40 ||
            frame[32] != 0x40 + 31 || frame[33] != MESH_MESHCORE_REQ_GET_NEIGHBOURS ||
            frame[34] != 0U || frame[35] != MESH_MESHCORE_NEIGHBOURS_MAX || frame[36] != 0U ||
            frame[37] != 0U || frame[38] != 0U || frame[39] != MESH_MESHCORE_NEIGHBOUR_PREFIX_LEN,
        "by her whole key: the first ten, newest first, four bytes of each key");
    MESH_TEST_FAIL_IF(mesh_meshcore_request_status(&g_meshcore, alice) != -EBUSY,
                      "a status waits behind it");

    uint8_t push[2U + 4U + 4U + 2U * 9U] = {MESH_MESHCORE_PUSH_BINARY_RESPONSE, 0};
    mesh_test_put_u32(push + 2, 0x11223344U);
    push[6] = 5; /* five held, two sent */
    push[8] = 2;
    const uint8_t k_entries[] = {0x0A, 0x0B, 0x0C, 0x0D, 30, 0, 0, 0, 20,
                                 0x50, 0x51, 0x52, 0x53, 90, 0, 0, 0, (uint8_t)-10};
    memcpy(push + 10, k_entries, sizeof k_entries);
    const uint32_t notices = g_meshcore.notices;
    feed(&protocol, push, sizeof push);
    MESH_TEST_FAIL_IF(g_meshcore.notices != notices || node->neighbors.valid,
                      "an answer before the SENT that tags it is nobody's");

    uint8_t sent[10] = {MESH_MESHCORE_RESP_SENT, 0};
    mesh_test_put_u32(sent + 2, 0x11223344U);
    mesh_test_put_u32(sent + 6, 3000U);
    feed(&protocol, sent, sizeof sent);
    mesh_test_put_u32(push + 2, 0x99999999U);
    feed(&protocol, push, sizeof push);
    MESH_TEST_FAIL_IF(g_meshcore.notices != notices, "another tag is another request's");

    mesh_test_put_u32(push + 2, 0x11223344U);
    feed(&protocol, push, sizeof push);
    MESH_TEST_FAIL_IF(g_meshcore.notices != notices + 1U ||
                          g_meshcore.notice.cmd != MESH_MESHCORE_CMD_SEND_BINARY_REQ ||
                          g_meshcore.notice.answer != MESH_MESHCORE_ANSWER_NEIGHBOURS ||
                          g_meshcore.request_cmd != 0U,
                      "its own tag ends the request");
    const struct mesh_node_neighbors *neighbors = &node->neighbors;
    MESH_TEST_FAIL_IF(
        !neighbors->valid || neighbors->count != 2U ||
            neighbors->entries[0].node_id != 0x0A0B0C0DU || neighbors->entries[0].snr != 5.0f ||
            neighbors->entries[1].node_id != 0x50515253U || neighbors->entries[1].snr != -2.5f,
        "and the two it sent land on her record, by the roster's numbers");

    /* An answer shorter than the entries it counts is not read, and is still the answer. */
    MESH_TEST_FAIL_IF(mesh_meshcore_request_neighbours(&g_meshcore, alice) != 0, "asked again");
    feed(&protocol, sent, sizeof sent);
    feed(&protocol, push, sizeof push - 1U);
    MESH_TEST_FAIL_IF(g_meshcore.notices != notices + 2U || neighbors->count != 2U,
                      "a short one ends the request and leaves the last list");
    record_success(test_name);
}

/* A MeshCore channel link joins one channel: into the first slot the sync read as unused, as a
   SET_CHANNEL save, and not at all when a slot already holds that name and secret or when every
   slot read is in use. */
MESH_TEST_CASE(meshcore_channel_link_joins_a_free_slot, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    static const uint8_t k_public[16] = {0x8b}; /* the fixture's: 0x8b, then zeros */
    static const uint8_t k_owls[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t slot = 0xFFU;
    const struct mesh_radio_settings *settings = mesh_session_settings(&g_model);
    MESH_TEST_FAIL_IF(settings == NULL || settings->channels[0].settings.psk.size != 16U ||
                          memcmp(settings->channels[0].settings.psk.bytes, k_public, 16U) != 0,
                      "the fixture's Public is slot 0");
    MESH_TEST_FAIL_IF(mesh_meshcore_import_channel(&g_meshcore, "Public", k_public, &slot) !=
                              -EEXIST ||
                          slot != 0U,
                      "the channel it is already on is refused, and named");
    MESH_TEST_FAIL_IF(mesh_meshcore_import_channel(&g_meshcore, "Owls", k_owls, &slot) != -ENOSPC,
                      "its one slot is in use");
    MESH_TEST_FAIL_IF(mesh_meshcore_import_channel(&g_meshcore, "", k_owls, &slot) != -EINVAL,
                      "a link with no name names nothing a slot could hold");

    /* A second slot the sync read as unused. */
    g_meshcore.device.max_channels = 2U;
    struct mesh_channel_summary unused;
    memset(&unused, 0, sizeof unused);
    unused.index = 1U;
    MESH_TEST_FAIL_IF(mesh_session_model_set_channel(&g_model, &unused) < 0, "slot 1 is read");
    mesh_session_model_settings(&g_model)->has_channel[1] = true;
    wire.count = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_import_channel(&g_meshcore, "Owls", k_owls, &slot) <= 0 ||
                          slot != 1U,
                      "a new channel goes into the free slot");
    MESH_TEST_FAIL_IF(wire.count == 0U || wire.frames[0][0] != MESH_MESHCORE_CMD_SET_CHANNEL ||
                          wire.frames[0][1] != 1U || memcmp(wire.frames[0] + 2, "Owls", 5U) != 0 ||
                          memcmp(wire.frames[0] + 34, k_owls, 16U) != 0,
                      "as SET_CHANNEL: the slot, the name and the secret");
    record_success(test_name);
}

/* A signed card goes to the radio whole, after IMPORT_CONTACT, for the radio to check as an
   advert heard on the air - never this radio's own, and never one that would land on another
   node's roster entry. */
MESH_TEST_CASE(meshcore_card_is_handed_to_the_radio_whole, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    uint8_t packet[1U + 1U + 32U + 4U + 64U + 5U];
    for (size_t i = 0; i < sizeof packet; ++i) {
        packet[i] = (uint8_t)(0xA0U + i);
    }
    uint8_t key[32];
    memset(key, 0x77, sizeof key);
    MESH_TEST_FAIL_IF(mesh_meshcore_import_card(&g_meshcore, packet, 1U + 32U + 64U, key) !=
                          -EINVAL,
                      "a card too short for a key and a signature is refused");
    MESH_TEST_FAIL_IF(mesh_meshcore_import_card(&g_meshcore, packet, sizeof packet,
                                                g_meshcore.self.public_key) != -EINVAL,
                      "this radio's own card is refused");
    uint8_t twin[32];
    memset(twin, 0x99, sizeof twin);
    twin[0] = 0x40;
    twin[1] = 0x41;
    twin[2] = 0x42;
    twin[3] = 0x43; /* Alice's first four bytes, not her key */
    MESH_TEST_FAIL_IF(mesh_meshcore_import_card(&g_meshcore, packet, sizeof packet, twin) !=
                          -EADDRINUSE,
                      "a key that would land on Alice's entry is refused");
    wire.count = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_import_card(&g_meshcore, packet, sizeof packet, key) != 1,
                      "a stranger's card is asked");
    MESH_TEST_FAIL_IF(wire.count != 1U || wire.lens[0] != 1U + sizeof packet ||
                          wire.frames[0][0] != MESH_MESHCORE_CMD_IMPORT_CONTACT ||
                          memcmp(wire.frames[0] + 1, packet, sizeof packet) != 0,
                      "as IMPORT_CONTACT and every byte of it");
    record_success(test_name);
}

/* A login queued behind a reboot is still the open request once the handshake starts over:
   it is written first, and its SENT arms the deadline its answer is held to. */
MESH_TEST_CASE(meshcore_request_outlives_a_reboot_restart, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    MESH_TEST_FAIL_IF(mesh_meshcore_reboot(&g_meshcore) != 1, "the reboot is queued");
    MESH_TEST_FAIL_IF(mesh_meshcore_login(&g_meshcore, 0x40414243U, "") != 0, "a login behind it");
    mesh_protocol_tick(&protocol, g_meshcore.awaiting_since_ms + MESH_MESHCORE_REPLY_TIMEOUT_MS);
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_SEND_LOGIN ||
                          g_meshcore.request_cmd != MESH_MESHCORE_CMD_SEND_LOGIN,
                      "the login goes out first, still the open request");
    static const uint8_t k_sent[10] = {MESH_MESHCORE_RESP_SENT, 0, 1, 2, 3, 4, 0x88, 0x13, 0, 0};
    feed(&protocol, k_sent, sizeof k_sent);
    MESH_TEST_FAIL_IF(g_meshcore.request_until_ms == 0U, "and its SENT arms its deadline");
    record_success(test_name);
}

/* An advert is SEND_SELF_ADVERT with 1 to flood it and 0 for the nodes in earshot. */
MESH_TEST_CASE(meshcore_advert_is_flooded_or_not, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    MESH_TEST_FAIL_IF(mesh_meshcore_send_advert(&g_meshcore, false) != 0, "a nearby advert");
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_SEND_SELF_ADVERT ||
                          wire.frames[wire.count - 1U][1] != 0U,
                      "goes out zero-hop");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(mesh_meshcore_send_advert(&g_meshcore, true) != 0 ||
                          wire.frames[wire.count - 1U][1] != 1U,
                      "and a flooded one says so");
    record_success(test_name);
}

/* A reboot is never answered, and over a USB bridge the port stays open while the ESP32 behind
   it resets - so once the answer is overdue the conversation starts over by itself. */
MESH_TEST_CASE(meshcore_reboot_syncs_again, unit) {
    struct mesh_protocol protocol;
    static struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    MESH_TEST_FAIL_IF(mesh_meshcore_reboot(&g_meshcore) != 1, "the reboot is queued");
    struct mesh_meshcore_settings_write write;
    memset(&write, 0, sizeof write);
    write.set_tx_power = true;
    write.tx_power_dbm = 10;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != -EBUSY,
                      "no save is queued behind it");
    MESH_TEST_FAIL_IF(mesh_meshcore_reboot(&g_meshcore) != 0, "nor a second reboot");
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_REBOOT ||
                          wire.lens[wire.count - 1U] != 7U ||
                          memcmp(wire.frames[wire.count - 1U] + 1, "reboot", 6U) != 0,
                      "carrying the word the firmware checks for");
    mesh_protocol_tick(&protocol, g_meshcore.awaiting_since_ms + MESH_MESHCORE_REPLY_TIMEOUT_MS);
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_DEVICE_QUERY,
                      "the silence that follows starts the handshake over");
    MESH_TEST_FAIL_IF(mesh_protocol_silent(&protocol), "and is not counted against the link");
    /* But a handshake the link will not take leaves nothing to time out, so the link is
       called silent at once rather than left unsynced. */
    feed(&protocol, mesh_test_meshcore_device_info, sizeof mesh_test_meshcore_device_info);
    while (g_meshcore.queue_count > 0U) {
        feed_code(&protocol, MESH_MESHCORE_RESP_ERR);
    }
    MESH_TEST_FAIL_IF(mesh_meshcore_reboot(&g_meshcore) != 1, "a second reboot is queued");
    wire.refuse = true;
    mesh_protocol_tick(&protocol, g_meshcore.awaiting_since_ms + MESH_MESHCORE_REPLY_TIMEOUT_MS);
    MESH_TEST_FAIL_IF(!mesh_protocol_silent(&protocol), "a refused handshake drops the link");
    wire.refuse = false;
    g_meshcore.timeouts = 0U;

    /* A reboot the link refuses outright was never asked for. */
    feed(&protocol, mesh_test_meshcore_device_info, sizeof mesh_test_meshcore_device_info);
    wire.refuse = true;
    while (g_meshcore.queue_count > 0U) {
        feed_code(&protocol, MESH_MESHCORE_RESP_ERR);
    }
    MESH_TEST_FAIL_IF(mesh_meshcore_reboot(&g_meshcore) != -EAGAIN,
                      "a reboot the link refuses is the call's answer");
    MESH_TEST_FAIL_IF(mesh_meshcore_refresh_settings(&g_meshcore) != -EAGAIN,
                      "and so is a refresh");
    record_success(test_name);
}

/* ------------------------------------------------------------------ backups */

static struct mesh_radio_backup g_backup;
static struct mesh_radio_backup g_backup_read;
static struct mesh_meshcore_backup_contents g_contents;

MESH_TEST_CASE(meshcore_backup_waits_for_the_whole_sync, unit) {
    mesh_session_init(&g_model);
    mesh_meshcore_init(&g_meshcore, &g_model);
    struct mesh_protocol protocol = mesh_meshcore_protocol(&g_meshcore);
    struct mesh_test_meshcore_wire wire;
    memset(&wire, 0, sizeof wire);
    mesh_protocol_attach(&protocol, mesh_test_meshcore_wire_send, &wire);
    (void)mesh_protocol_begin(&protocol);
    feed(&protocol, mesh_test_meshcore_device_info, sizeof mesh_test_meshcore_device_info);
    feed(&protocol, mesh_test_meshcore_self_info, sizeof mesh_test_meshcore_self_info);
    /* SELF_INFO is in, the contact list is not: a backup now would be a radio with none. */
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_capture(&g_meshcore, &g_backup) != -EAGAIN,
                      "a radio still reading its contacts was captured");
    MESH_TEST_FAIL_IF(g_backup.section_count != 0U, "a refused capture left sections behind");
    record_success(test_name);
}

MESH_TEST_CASE(meshcore_backup_keeps_settings_channels_and_contacts_whole, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_capture(&g_meshcore, &g_backup) != 0,
                      "a synced radio was not captured");
    g_backup.header.reason = MESH_RADIO_BACKUP_MANUAL;

    const char *path = "/tmp/meshcore_backup_test.backup";
    const int written = mesh_radio_backup_write_file(&g_backup, path);
    const int read = mesh_radio_backup_read_file(&g_backup_read, path);
    remove(path);
    MESH_TEST_FAIL_IF(written != 0 || read != 0, "the file did not round-trip");
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_read(&g_backup_read, &g_contents) != 0,
                      "the sections did not decode");

    const struct mesh_radio_backup_header *header = &g_backup_read.header;
    MESH_TEST_FAIL_IF(header->protocol != MESH_RADIO_BACKUP_MESHCORE ||
                          header->node_id != g_meshcore.self_node,
                      "the header does not name this radio");
    MESH_TEST_FAIL_IF(strcmp(header->model, "Heltec V3") != 0 ||
                          strncmp(header->firmware, "v1.17.1", 7U) != 0,
                      "the header lost the board or the firmware");
    MESH_TEST_FAIL_IF(!header->has_contacts || header->contacts != 1U || header->has_nodes_heard,
                      "a MeshCore radio counts contacts it keeps, not nodes it heard");
    MESH_TEST_FAIL_IF(!header->has_radio ||
                          header->frequency_khz != g_meshcore.self.frequency_khz ||
                          header->spreading_factor != g_meshcore.self.spreading_factor,
                      "the header's radio numbers are not SELF_INFO's");
    MESH_TEST_FAIL_IF(header->channel_count != 1U ||
                          strcmp(header->channel_names[0], "Public") != 0,
                      "the header's channel list is wrong");

    MESH_TEST_FAIL_IF(!g_contents.has_self ||
                          memcmp(g_contents.self.public_key, g_meshcore.self.public_key,
                                 MESH_MESHCORE_PUBKEY_LEN) != 0 ||
                          strcmp(g_contents.self.name, g_meshcore.self.name) != 0 ||
                          g_contents.self.bandwidth_hz != g_meshcore.self.bandwidth_hz ||
                          g_contents.self.tx_power_dbm != g_meshcore.self.tx_power_dbm,
                      "the radio's settings did not come back");
    MESH_TEST_FAIL_IF(!g_contents.has_pin || g_contents.ble_pin != g_meshcore.device.ble_pin,
                      "the Bluetooth PIN did not come back");
    MESH_TEST_FAIL_IF(!g_contents.has_channel[0] ||
                          strcmp(g_contents.channels[0].name, "Public") != 0 ||
                          g_contents.channels[0].secret[0] != 0x8b,
                      "the channel and its secret did not come back");
    /* The route is the part the roster never kept, and the reason the book exists. */
    MESH_TEST_FAIL_IF(g_contents.contact_count != 1U ||
                          strcmp(g_contents.contacts[0].name, "Alice") != 0 ||
                          g_contents.contacts[0].out_path_len != 2U ||
                          g_contents.contacts[0].lastmod != 1700000000U ||
                          g_contents.contacts[0].public_key[0] != 0x40,
                      "the contact did not come back whole");
    record_success(test_name);
}

MESH_TEST_CASE(meshcore_contact_book_follows_the_radio, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    MESH_TEST_FAIL_IF(mesh_meshcore_contact_count(&g_meshcore) != 1U,
                      "the synced contact is not in the book");

    /* A route changed: the radio says so by key, and the record it is asked for replaces the
       one kept rather than joining it. */
    uint8_t push[1U + MESH_MESHCORE_PUBKEY_LEN];
    push[0] = MESH_MESHCORE_PUSH_PATH_UPDATED;
    for (size_t k = 0; k < MESH_MESHCORE_PUBKEY_LEN; ++k) {
        push[1U + k] = (uint8_t)(0x40U + k);
    }
    feed(&protocol, push, sizeof push);
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_GET_CONTACT_BY_KEY,
                      "a path update did not ask for the record");
    uint8_t frame[160];
    feed(&protocol, frame,
         mesh_test_meshcore_contact(frame, MESH_MESHCORE_RESP_CONTACT, 0x40, "Alice",
                                    MESH_MESHCORE_ADV_CHAT, 4U, 1700000500U));
    const struct mesh_meshcore_contact *kept = mesh_meshcore_contact_at(&g_meshcore, 0U);
    MESH_TEST_FAIL_IF(mesh_meshcore_contact_count(&g_meshcore) != 1U || kept == NULL ||
                          kept->out_path_len != 4U,
                      "an updated record was not kept in place");

    /* A heard advert the radio did not add is not on its list. */
    feed(&protocol, frame,
         mesh_test_meshcore_contact(frame, MESH_MESHCORE_PUSH_NEW_ADVERT, 0x60, "Bob",
                                    MESH_MESHCORE_ADV_CHAT, MESH_MESHCORE_PATH_NONE, 1700000600U));
    MESH_TEST_FAIL_IF(mesh_meshcore_contact_count(&g_meshcore) != 1U,
                      "an advert the radio did not add went into the book");

    /* And one the radio deleted leaves it. */
    push[0] = MESH_MESHCORE_PUSH_CONTACT_DELETED;
    feed(&protocol, push, sizeof push);
    MESH_TEST_FAIL_IF(mesh_meshcore_contact_count(&g_meshcore) != 0U,
                      "a contact the radio deleted stayed in the book");
    record_success(test_name);
}

MESH_TEST_CASE(meshcore_contact_book_starts_again_on_a_new_connection, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    MESH_TEST_FAIL_IF(mesh_meshcore_contact_count(&g_meshcore) != 1U, "fixture: no contact");
    /* Another radio on the next link: its list is read from the start, and until it is, this
       radio's contacts must not be taken for that one's. */
    mesh_protocol_detach(&protocol);
    mesh_protocol_attach(&protocol, mesh_test_meshcore_wire_send, &wire);
    (void)mesh_protocol_begin(&protocol);
    MESH_TEST_FAIL_IF(mesh_meshcore_contact_count(&g_meshcore) != 0U,
                      "the last connection's contacts survived into this one");
    MESH_TEST_FAIL_IF(g_meshcore.has_channel[0], "the last connection's channels survived");
    record_success(test_name);
}

MESH_TEST_CASE(meshcore_backup_refuses_a_book_that_dropped_a_contact, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    /* A radio that reports the protocol's most - 255 * 2 - fits the book whole. */
    MESH_TEST_FAIL_IF(MESH_MESHCORE_CONTACTS_MAX < 255U * 2U,
                      "the book is smaller than a limit DEVICE_INFO can report");
    /* And one past it anyway: the backup would say complete about a list with holes. */
    g_meshcore.contacts_unkept = 1U;
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_capture(&g_meshcore, &g_backup) != -EOVERFLOW,
                      "a backup was written with contacts missing from it");
    MESH_TEST_FAIL_IF(g_backup.section_count != 0U, "a refused capture left sections behind");
    record_success(test_name);
}

static struct mesh_radio_backup_diff g_diff;

MESH_TEST_CASE(meshcore_backup_diff_of_the_same_radio_is_empty, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup_read);
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_diff(&g_backup, &g_backup_read, &g_diff) != 0 ||
                          g_diff.count != 0U || g_diff.total != 0U,
                      "an unchanged radio was reported as changed");
    record_success(test_name);
}

MESH_TEST_CASE(meshcore_backup_diff_names_the_one_setting_changed, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);
    const uint32_t was = g_meshcore.self.frequency_khz;
    g_meshcore.self.frequency_khz = was + 125U;
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup_read);
    mesh_meshcore_backup_diff(&g_backup, &g_backup_read, &g_diff);
    MESH_TEST_FAIL_IF(g_diff.count != 1U, "not exactly one change");
    const struct mesh_radio_backup_change *change = &g_diff.changes[0];
    MESH_TEST_FAIL_IF(change->topic != MESH_RADIO_BACKUP_TOPIC_LORA ||
                          change->field != MESH_MESHCORE_BACKUP_FIELD_FREQUENCY ||
                          change->before.number != (int64_t)was ||
                          change->after.number != (int64_t)was + 125,
                      "the change is not the frequency, backup to radio");

    /* And what the radio advertises itself as, which is a setting like any other. */
    g_meshcore.self.frequency_khz = was;
    g_meshcore.self.adv_type = (uint8_t)(g_meshcore.self.adv_type + 1U);
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup_read);
    mesh_meshcore_backup_diff(&g_backup, &g_backup_read, &g_diff);
    MESH_TEST_FAIL_IF(g_diff.count != 1U ||
                          g_diff.changes[0].field != MESH_MESHCORE_BACKUP_FIELD_ADV_TYPE,
                      "a changed advert type was not one change");
    record_success(test_name);
}

MESH_TEST_CASE(meshcore_backup_diff_lists_contacts_by_key_not_by_route, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);

    /* Alice's route and stamps moved, which is the radio hearing her again: not a change. */
    struct mesh_meshcore_contact *alice = &g_meshcore.contacts[0];
    alice->out_path_len = 1U;
    alice->lastmod += 600U;
    alice->last_advert += 600U;
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup_read);
    mesh_meshcore_backup_diff(&g_backup, &g_backup_read, &g_diff);
    MESH_TEST_FAIL_IF(g_diff.count != 0U, "a contact heard again was called changed");

    /* Her name changing is, and a contact added since is an addition. */
    snprintf(alice->name, sizeof alice->name, "%s", "Alicia");
    struct mesh_meshcore_contact *bob = &g_meshcore.contacts[g_meshcore.contact_count++];
    *bob = *alice;
    bob->public_key[0] = 0x60;
    snprintf(bob->name, sizeof bob->name, "%s", "Bob");
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup_read);
    mesh_meshcore_backup_diff(&g_backup, &g_backup_read, &g_diff);
    MESH_TEST_FAIL_IF(g_diff.count != 2U, "not a rename and an addition");
    MESH_TEST_FAIL_IF(g_diff.changes[0].topic != MESH_RADIO_BACKUP_TOPIC_CONTACT ||
                          g_diff.changes[0].kind != MESH_RADIO_BACKUP_CHANGED ||
                          strcmp(g_diff.changes[0].before.text, "Alice") != 0 ||
                          strcmp(g_diff.changes[0].after.text, "Alicia") != 0,
                      "the rename is not Alice to Alicia");
    MESH_TEST_FAIL_IF(g_diff.changes[1].kind != MESH_RADIO_BACKUP_ADDED ||
                          strcmp(g_diff.changes[1].subject, "Bob") != 0,
                      "Bob was not listed as added");

    /* And the other way round, Bob is gone. */
    mesh_meshcore_backup_diff(&g_backup_read, &g_backup, &g_diff);
    MESH_TEST_FAIL_IF(g_diff.count != 2U || g_diff.changes[1].kind != MESH_RADIO_BACKUP_REMOVED,
                      "a contact the radio no longer has was not a removal");
    record_success(test_name);
}

static struct mesh_meshcore_settings_write g_plan[MESH_MESHCORE_BACKUP_PLAN_MAX];
static struct mesh_meshcore_contact g_contact_plan[MESH_MESHCORE_CONTACTS_MAX];

MESH_TEST_CASE(meshcore_backup_plan_of_an_unchanged_radio_sends_nothing, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);
    size_t unwritable = 99U;
    const int planned = mesh_meshcore_backup_plan(&g_backup, &g_meshcore, g_plan,
                                                  MESH_MESHCORE_BACKUP_PLAN_MAX, &unwritable);
    MESH_TEST_FAIL_IF(planned != 0 || unwritable != 0U, "a radio matching its backup had a plan");
    record_success(test_name);
}

/* One slot changed on the radio: the plan is that slot, whole, and nothing else - and on the
   wire it is one SET_CHANNEL and the read-back of that slot. */
MESH_TEST_CASE(meshcore_backup_plan_writes_one_changed_channel_and_reads_it_back, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);
    snprintf(g_meshcore.channels[0].name, sizeof g_meshcore.channels[0].name, "%s", "Renamed");
    g_meshcore.channels[0].secret[0] = 0x11;
    size_t unwritable = 0U;
    const int planned = mesh_meshcore_backup_plan(&g_backup, &g_meshcore, g_plan,
                                                  MESH_MESHCORE_BACKUP_PLAN_MAX, &unwritable);
    MESH_TEST_FAIL_IF(planned != 1 || unwritable != 0U, "not exactly one save");
    const struct mesh_meshcore_settings_write *write = &g_plan[0];
    MESH_TEST_FAIL_IF(!write->set_channel || write->channel_index != 0U || write->set_name ||
                          write->set_radio || write->set_tx_power || write->set_position ||
                          write->set_other || write->set_pin,
                      "the save is not the slot alone");
    MESH_TEST_FAIL_IF(strncmp(write->channel_name, "Public", MESH_MESHCORE_NAME_LEN) != 0 ||
                          write->channel_secret[0] != 0x8b,
                      "the slot is not the backup's name and secret");

    const size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, write) <= 0, "the save refused");
    MESH_TEST_FAIL_IF(wire.count != before + 1U ||
                          mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_SET_CHANNEL,
                      "the slot did not go out as one SET_CHANNEL");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(wire.count != before + 2U ||
                          mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_GET_CHANNEL,
                      "the slot was not read back");
    MESH_TEST_FAIL_IF(g_model.settings.writes_acked == 0U, "the OK was not counted");
    record_success(test_name);
}

/*
 * The radio parameters and the power as a group each: a frequency changed is the four numbers
 * written whole, and a power past what this radio reports it can do is counted unwritable and
 * not sent - never quietly written as something else. And a group the radio refuses is a save
 * that failed.
 */
MESH_TEST_CASE(meshcore_backup_plan_writes_groups_whole_and_counts_what_it_cannot, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    const uint32_t frequency = g_meshcore.self.frequency_khz;
    g_meshcore.self.tx_power_dbm = (uint8_t)(g_meshcore.self.max_tx_power_dbm + 5U);
    g_meshcore.self.adv_type = (uint8_t)(g_meshcore.self.adv_type + 1U);
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);
    g_meshcore.self.tx_power_dbm = 10U;
    g_meshcore.self.adv_type = (uint8_t)(g_meshcore.self.adv_type - 1U);
    g_meshcore.self.frequency_khz = frequency + 125U;
    size_t unwritable = 0U;
    const int planned = mesh_meshcore_backup_plan(&g_backup, &g_meshcore, g_plan,
                                                  MESH_MESHCORE_BACKUP_PLAN_MAX, &unwritable);
    MESH_TEST_FAIL_IF(planned != 1, "not one save");
    MESH_TEST_FAIL_IF(!g_plan[0].set_radio || g_plan[0].frequency_khz != frequency ||
                          g_plan[0].bandwidth_hz != g_meshcore.self.bandwidth_hz ||
                          g_plan[0].spreading_factor != g_meshcore.self.spreading_factor,
                      "the radio parameters were not the backup's, whole");
    MESH_TEST_FAIL_IF(g_plan[0].set_tx_power, "a power past the radio's ceiling was planned");
    MESH_TEST_FAIL_IF(unwritable != 2U, "the power and the advert type were not both counted");

    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &g_plan[0]) <= 0,
                      "the save refused");
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_SET_RADIO_PARAMS,
                      "not the radio");
    const uint32_t failed = g_model.settings.writes_failed;
    feed_code(&protocol, MESH_MESHCORE_RESP_ERR);
    MESH_TEST_FAIL_IF(g_model.settings.writes_failed != failed + 1U,
                      "a group the radio refused was not a failed save");
    record_success(test_name);
}

MESH_TEST_CASE(meshcore_backup_plan_refuses_another_radios_backup, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    g_meshcore.self.public_key[5] ^= 0xffU;
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);
    g_meshcore.self.public_key[5] ^= 0xffU;
    size_t unwritable = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_plan(&g_backup, &g_meshcore, g_plan,
                                                MESH_MESHCORE_BACKUP_PLAN_MAX,
                                                &unwritable) != -ENODEV,
                      "a backup under another key was planned onto this radio");
    /* Nor one that does not say whose it is: channels and a PIN with no settings section. */
    mesh_radio_backup_reset(&g_backup);
    g_backup.header.protocol = MESH_RADIO_BACKUP_MESHCORE;
    g_backup.header.node_id = g_meshcore.self_node;
    const uint8_t pin[4] = {0x40, 0xe2, 0x01, 0x00};
    mesh_radio_backup_add(&g_backup, MESH_MESHCORE_BACKUP_PIN, pin, sizeof pin);
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_plan(&g_backup, &g_meshcore, g_plan,
                                                MESH_MESHCORE_BACKUP_PLAN_MAX,
                                                &unwritable) != -ENODEV,
                      "a backup with no key to check was planned onto this radio");
    /* And the contact plan refuses both the same way. */
    struct mesh_meshcore_contact_options options = {0};
    struct mesh_meshcore_contact_plan_notes notes;
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_plan_contacts(&g_backup, &g_meshcore, &options,
                                                         g_contact_plan, MESH_MESHCORE_CONTACTS_MAX,
                                                         &notes) != -ENODEV,
                      "contacts from a backup with no key to check were planned");
    /* Contacts are planned apart: one gone from the radio is not the settings plan's. */
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);
    g_meshcore.contact_count = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_plan(&g_backup, &g_meshcore, g_plan,
                                                MESH_MESHCORE_BACKUP_PLAN_MAX, &unwritable) != 0 ||
                          unwritable != 0U,
                      "a missing contact was counted by the settings plan");
    record_success(test_name);
}

/* Values the radio can report but its commands cannot set back - a channel name filling all 32
   bytes, an empty advert name - are counted unwritable, and the rest of the plan still goes. */
MESH_TEST_CASE(meshcore_backup_plan_counts_names_its_commands_cannot_carry, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    memset(g_meshcore.channels[0].name, 'x', MESH_MESHCORE_NAME_LEN);
    g_meshcore.channels[0].name[MESH_MESHCORE_NAME_LEN] = '\0';
    g_meshcore.self.name[0] = '\0';
    const uint8_t power = g_meshcore.self.tx_power_dbm;
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);
    snprintf(g_meshcore.channels[0].name, sizeof g_meshcore.channels[0].name, "%s", "Public");
    snprintf(g_meshcore.self.name, sizeof g_meshcore.self.name, "%s", "MPBC");
    g_meshcore.self.tx_power_dbm = 10U;
    size_t unwritable = 0U;
    const int planned = mesh_meshcore_backup_plan(&g_backup, &g_meshcore, g_plan,
                                                  MESH_MESHCORE_BACKUP_PLAN_MAX, &unwritable);
    MESH_TEST_FAIL_IF(planned != 1 || g_plan[0].set_channel || g_plan[0].set_name,
                      "a name the commands cannot carry was planned");
    MESH_TEST_FAIL_IF(!g_plan[0].set_tx_power || g_plan[0].tx_power_dbm != (int8_t)power,
                      "the power that can be written was dropped with them");
    MESH_TEST_FAIL_IF(unwritable != 2U, "the two names were not counted unwritable");
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &g_plan[0]) <= 0,
                      "what was planned did not encode");
    record_success(test_name);
}

/* ------------------------------------------------------------------ restoring contacts */

/* A contact in the radio's book, straight into it: key `lead`..., a two-hop route. */
static struct mesh_meshcore_contact *book_contact(uint8_t lead, const char *name, uint8_t flags,
                                                  uint32_t lastmod) {
    struct mesh_meshcore_contact *contact = &g_meshcore.contacts[g_meshcore.contact_count++];
    memset(contact, 0, sizeof *contact);
    memset(contact->public_key, lead, MESH_MESHCORE_PUBKEY_LEN);
    contact->type = MESH_MESHCORE_ADV_CHAT;
    contact->flags = flags;
    contact->out_path_len = 2U;
    contact->out_path[0] = 0x11U;
    contact->out_path[1] = 0x22U;
    snprintf(contact->name, sizeof contact->name, "%s", name);
    contact->lastmod = lastmod;
    return contact;
}

static const struct mesh_meshcore_contact *planned_contact(size_t count, uint8_t lead) {
    for (size_t i = 0; i < count; ++i) {
        if (g_contact_plan[i].public_key[0] == lead) {
            return &g_contact_plan[i];
        }
    }
    return NULL;
}

/* A contact gone from the radio and one renamed on it are put back; one only on the radio is
   left there. Each goes with no route unless the routes are kept. */
MESH_TEST_CASE(meshcore_backup_plan_contacts_puts_back_what_is_missing_or_changed, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    g_meshcore.contact_count = 0U;
    book_contact(0x51U, "Bob", 0U, 100U);
    book_contact(0x52U, "Carol", 0U, 100U);
    book_contact(0x53U, "Dave", 0U, 100U);
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_capture(&g_meshcore, &g_backup) != 0, "no backup");
    /* Since: Bob is gone, Carol renamed before the backup's stamp, Erin new. */
    g_meshcore.contacts[0] = g_meshcore.contacts[2];
    g_meshcore.contact_count = 2U;
    snprintf(g_meshcore.contacts[1].name, sizeof g_meshcore.contacts[1].name, "%s", "Caz");
    g_meshcore.contacts[1].lastmod = 90U;
    book_contact(0x54U, "Erin", 0U, 200U);

    struct mesh_meshcore_contact_options options = {0};
    struct mesh_meshcore_contact_plan_notes notes;
    int planned = mesh_meshcore_backup_plan_contacts(
        &g_backup, &g_meshcore, &options, g_contact_plan, MESH_MESHCORE_CONTACTS_MAX, &notes);
    MESH_TEST_FAIL_IF(planned != 2, "not Bob and Carol");
    const struct mesh_meshcore_contact *bob = planned_contact(2U, 0x51U);
    const struct mesh_meshcore_contact *carol = planned_contact(2U, 0x52U);
    MESH_TEST_FAIL_IF(bob == NULL || carol == NULL || strcmp(carol->name, "Carol") != 0,
                      "the backup's records were not the plan");
    MESH_TEST_FAIL_IF(planned_contact(2U, 0x54U) != NULL,
                      "a contact only on the radio was planned");
    MESH_TEST_FAIL_IF(bob->out_path_len != MESH_MESHCORE_PATH_NONE || bob->out_path[0] != 0U,
                      "a route was restored by default");
    MESH_TEST_FAIL_IF(notes.newer != 0U || notes.left_out != 0U, "something was left alone");

    options.keep_routes = true;
    planned = mesh_meshcore_backup_plan_contacts(&g_backup, &g_meshcore, &options, g_contact_plan,
                                                 MESH_MESHCORE_CONTACTS_MAX, &notes);
    bob = planned_contact((size_t)planned, 0x51U);
    MESH_TEST_FAIL_IF(bob == NULL || bob->out_path_len != 2U || bob->out_path[1] != 0x22U,
                      "the route kept was not the backup's");
    record_success(test_name);
}

/* A contact the radio changed after the backup was taken is the radio's, unless replaced. */
MESH_TEST_CASE(meshcore_backup_plan_contacts_keeps_what_the_radio_changed_since, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    g_meshcore.contact_count = 0U;
    book_contact(0x52U, "Carol", 0U, 100U);
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);
    g_meshcore.contacts[0].flags = MESH_MESHCORE_CONTACT_FAVORITE;
    g_meshcore.contacts[0].lastmod = 150U;

    struct mesh_meshcore_contact_options options = {0};
    struct mesh_meshcore_contact_plan_notes notes;
    int planned = mesh_meshcore_backup_plan_contacts(
        &g_backup, &g_meshcore, &options, g_contact_plan, MESH_MESHCORE_CONTACTS_MAX, &notes);
    MESH_TEST_FAIL_IF(planned != 0 || notes.newer != 1U,
                      "a contact pinned since the backup was written over");
    options.replace_newer = true;
    planned = mesh_meshcore_backup_plan_contacts(&g_backup, &g_meshcore, &options, g_contact_plan,
                                                 MESH_MESHCORE_CONTACTS_MAX, &notes);
    MESH_TEST_FAIL_IF(planned != 1 || notes.newer != 0U || g_contact_plan[0].flags != 0U,
                      "replacing did not put the backup's record back");
    record_success(test_name);
}

/* More to add than the radio has room for: favourites first, then the most recently changed,
   and the rest counted. An update takes no room. */
MESH_TEST_CASE(meshcore_backup_plan_contacts_fills_the_room_favourites_first, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    g_meshcore.contact_count = 0U;
    book_contact(0x51U, "Old", 0U, 10U);
    book_contact(0x52U, "Pinned", MESH_MESHCORE_CONTACT_FAVORITE, 5U);
    book_contact(0x53U, "Newest", 0U, 300U);
    book_contact(0x54U, "Newer", 0U, 200U);
    book_contact(0x55U, "Kept", 0U, 50U);
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);
    /* The radio keeps only "Kept", renamed, and has room for three. */
    g_meshcore.contacts[0] = g_meshcore.contacts[4];
    g_meshcore.contact_count = 1U;
    snprintf(g_meshcore.contacts[0].name, sizeof g_meshcore.contacts[0].name, "%s", "K");
    g_meshcore.device.max_contacts = 3U;

    struct mesh_meshcore_contact_options options = {0};
    struct mesh_meshcore_contact_plan_notes notes;
    const int planned = mesh_meshcore_backup_plan_contacts(
        &g_backup, &g_meshcore, &options, g_contact_plan, MESH_MESHCORE_CONTACTS_MAX, &notes);
    MESH_TEST_FAIL_IF(planned != 3, "not two adds and the update");
    MESH_TEST_FAIL_IF(g_contact_plan[0].public_key[0] != 0x52U, "the favourite was not first");
    MESH_TEST_FAIL_IF(g_contact_plan[1].public_key[0] != 0x53U,
                      "the most recently changed was not next");
    MESH_TEST_FAIL_IF(g_contact_plan[2].public_key[0] != 0x55U, "the update was not planned");
    MESH_TEST_FAIL_IF(planned_contact(3U, 0x54U) != NULL || planned_contact(3U, 0x51U) != NULL ||
                          notes.left_out != 2U,
                      "what did not fit was not counted");
    record_success(test_name);
}

/* One restore write at a time, each settled by its answer: OK lands it in the book, a refusal
   is counted, and the next is refused until the last is answered. */
MESH_TEST_CASE(meshcore_restore_contact_waits_for_its_answer, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    const size_t before = g_meshcore.contact_count;
    struct mesh_meshcore_contact contact;
    memset(&contact, 0, sizeof contact);
    memset(contact.public_key, 0x61, MESH_MESHCORE_PUBKEY_LEN);
    contact.type = MESH_MESHCORE_ADV_CHAT;
    contact.out_path_len = MESH_MESHCORE_PATH_NONE;
    snprintf(contact.name, sizeof contact.name, "%s", "Frank");

    MESH_TEST_FAIL_IF(mesh_meshcore_restore_contact(&g_meshcore, &contact) != 1, "not asked");
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_ADD_UPDATE_CONTACT,
                      "not an ADD_UPDATE_CONTACT");
    MESH_TEST_FAIL_IF(mesh_meshcore_restore_contact(&g_meshcore, &contact) != -EBUSY,
                      "a second was taken before the first was answered");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(g_meshcore.contact_restore_outstanding, "the OK did not settle it");
    MESH_TEST_FAIL_IF(g_meshcore.contact_count != before + 1U, "the contact is not in the book");
    MESH_TEST_FAIL_IF(g_meshcore.contact_restores_refused != 0U, "an OK counted as refused");

    contact.public_key[0] = 0x62U;
    MESH_TEST_FAIL_IF(mesh_meshcore_restore_contact(&g_meshcore, &contact) != 1, "not asked");
    feed_code(&protocol, MESH_MESHCORE_RESP_ERR);
    MESH_TEST_FAIL_IF(g_meshcore.contact_restore_outstanding ||
                          g_meshcore.contact_restores_refused != 1U,
                      "a refusal was not counted");

    /* Its own key is never written as a contact. */
    memcpy(contact.public_key, g_meshcore.self.public_key, MESH_MESHCORE_PUBKEY_LEN);
    MESH_TEST_FAIL_IF(mesh_meshcore_restore_contact(&g_meshcore, &contact) != -EINVAL,
                      "the radio's own key was written as a contact");

    /* A write the link takes with it is counted too, and does not hold the next connection. */
    contact.public_key[0] = 0x63U;
    MESH_TEST_FAIL_IF(mesh_meshcore_restore_contact(&g_meshcore, &contact) != 1, "not asked");
    mesh_protocol_detach(&protocol);
    MESH_TEST_FAIL_IF(g_meshcore.contact_restore_outstanding ||
                          g_meshcore.contact_restores_refused != 2U,
                      "a write lost with the link was not settled");
    record_success(test_name);
}

/* ------------------------------------------------------------------ profiles */

static struct mesh_radio_backup g_profile;
static struct mesh_radio_backup_diff g_profile_diff;

static struct mesh_radio_backup_parts profile_parts(uint8_t a, uint8_t b) {
    struct mesh_radio_backup_parts parts = {0};
    mesh_radio_profile_parts_set(&parts, a, 0U, true);
    mesh_radio_profile_parts_set(&parts, b, 0U, true);
    return parts;
}

/* Contacts, a name, a key and a place are the radio; none of them is in a profile, however
   much of it was asked for. */
MESH_TEST_CASE(meshcore_profile_from_a_backup_has_no_identity, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    g_meshcore.self.latitude_e6 = 47397700;
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);
    MESH_TEST_FAIL_IF(mesh_radio_backup_count_tag(&g_backup, MESH_MESHCORE_BACKUP_CONTACT) == 0U,
                      "fixture: the backup has no contact to leave out");
    const struct mesh_radio_backup_parts every = {.topics = UINT32_MAX, .modules = UINT32_MAX};
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &every, "Kit", &g_profile) != 0,
                      "the profile was not made");
    MESH_TEST_FAIL_IF(mesh_radio_backup_count_tag(&g_profile, MESH_MESHCORE_BACKUP_CONTACT) != 0U,
                      "a contact went into a profile");
    static struct mesh_meshcore_backup_contents contents;
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_read(&g_profile, &contents) != 0 || !contents.has_self,
                      "the profile's settings did not read");
    static const uint8_t zero[MESH_MESHCORE_PUBKEY_LEN] = {0};
    MESH_TEST_FAIL_IF(contents.self.name[0] != '\0' ||
                          memcmp(contents.self.public_key, zero, sizeof zero) != 0 ||
                          contents.self.latitude_e6 != 0,
                      "the radio's name, key or position is in the profile");
    MESH_TEST_FAIL_IF(contents.self.frequency_khz != g_meshcore.self.frequency_khz ||
                          !contents.has_pin || !contents.has_channel[0],
                      "the radio numbers, the PIN or a channel was left out");
    MESH_TEST_FAIL_IF(g_profile.header.node_id != 0U || g_profile.header.has_contacts,
                      "the header still names the radio");
    record_success(test_name);
}

/*
 * A profile of the channels, put on another radio - another key, another name, other radio
 * numbers: the comparison lists nothing outside the channels, and the plan is the one slot that
 * differs, planned for a radio that is not the profile's.
 */
MESH_TEST_CASE(meshcore_profile_applied_touches_only_its_parts, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);
    const struct mesh_radio_backup_parts channels =
        profile_parts(MESH_RADIO_BACKUP_TOPIC_CHANNEL, MESH_RADIO_BACKUP_TOPIC_CHANNEL);
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &channels, "Channels", &g_profile) != 0,
                      "the profile was not made");

    g_meshcore.self.public_key[0] ^= 0xffU;
    snprintf(g_meshcore.self.name, sizeof g_meshcore.self.name, "%s", "Other");
    g_meshcore.self.frequency_khz += 125U;
    g_meshcore.contact_count = 0U;
    snprintf(g_meshcore.channels[0].name, sizeof g_meshcore.channels[0].name, "%s", "Renamed");
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup_read);
    MESH_TEST_FAIL_IF(mesh_radio_profile_diff(&g_profile, &g_backup_read, &g_profile_diff) != 0,
                      "the comparison failed");
    for (size_t i = 0; i < g_profile_diff.count; ++i) {
        MESH_TEST_FAIL_IF(g_profile_diff.changes[i].topic != MESH_RADIO_BACKUP_TOPIC_CHANNEL,
                          "a difference outside the channels was listed");
    }
    MESH_TEST_FAIL_IF(g_profile_diff.total != 1U, "the renamed slot was not the one difference");

    size_t unwritable = 99U;
    const int planned = mesh_meshcore_backup_plan_profile(
        &g_profile, &g_meshcore, g_plan, MESH_MESHCORE_BACKUP_PLAN_MAX, &unwritable);
    MESH_TEST_FAIL_IF(planned != 1 || unwritable != 0U, "not exactly one save");
    MESH_TEST_FAIL_IF(!g_plan[0].set_channel || g_plan[0].channel_index != 0U ||
                          g_plan[0].set_name || g_plan[0].set_radio || g_plan[0].set_position ||
                          g_plan[0].set_pin || g_plan[0].set_other || g_plan[0].set_tx_power,
                      "the save reached past the channels");

    /* A profile of the radio numbers, onto the same radio: the numbers, and not its name. */
    const struct mesh_radio_backup_parts lora =
        profile_parts(MESH_RADIO_BACKUP_TOPIC_LORA, MESH_RADIO_BACKUP_TOPIC_LORA);
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &lora, "LoRa", &g_profile) != 0,
                      "the LoRa profile was not made");
    MESH_TEST_FAIL_IF(
        mesh_meshcore_backup_plan_profile(&g_profile, &g_meshcore, g_plan,
                                          MESH_MESHCORE_BACKUP_PLAN_MAX, &unwritable) != 1 ||
            !g_plan[0].set_radio || g_plan[0].set_name || g_plan[0].set_channel || unwritable != 0U,
        "the LoRa profile did not plan the radio numbers alone");
    record_success(test_name);
}

MESH_TEST_CASE(meshcore_profile_plan_refuses_what_is_not_a_meshcore_profile, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    mesh_meshcore_backup_capture(&g_meshcore, &g_backup);
    size_t unwritable = 0U;
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_plan_profile(&g_backup, &g_meshcore, g_plan,
                                                        MESH_MESHCORE_BACKUP_PLAN_MAX,
                                                        &unwritable) != -EINVAL,
                      "a backup was applied as a profile, to a radio that may not be its own");
    const struct mesh_radio_backup_parts lora =
        profile_parts(MESH_RADIO_BACKUP_TOPIC_LORA, MESH_RADIO_BACKUP_TOPIC_LORA);
    (void)mesh_radio_profile_make(&g_backup, &lora, "LoRa", &g_profile);
    g_profile.header.protocol = MESH_RADIO_BACKUP_MESHTASTIC;
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_plan_profile(&g_profile, &g_meshcore, g_plan,
                                                        MESH_MESHCORE_BACKUP_PLAN_MAX,
                                                        &unwritable) != -EPROTO,
                      "a Meshtastic profile was planned onto a MeshCore radio");
    record_success(test_name);
}

/* ------------------------------------------------------------------ the identity key */

/* A radio's private key in RESP_PRIVATE_KEY, every byte `fill`. */
static void feed_private_key(const struct mesh_protocol *protocol, uint8_t fill) {
    uint8_t frame[1U + MESH_MESHCORE_PRVKEY_LEN];
    frame[0] = MESH_MESHCORE_RESP_PRIVATE_KEY;
    memset(frame + 1, fill, MESH_MESHCORE_PRVKEY_LEN);
    feed(protocol, frame, sizeof frame);
}

/* Whether `fill` runs for eight bytes anywhere in the conversation's command queue. */
static bool queue_holds(uint8_t fill) {
    const uint8_t *bytes = (const uint8_t *)g_meshcore.queue;
    uint8_t run[8];
    memset(run, fill, sizeof run);
    for (size_t at = 0; at + sizeof run <= sizeof g_meshcore.queue; ++at) {
        if (memcmp(bytes + at, run, sizeof run) == 0) {
            return true;
        }
    }
    return false;
}

MESH_TEST_CASE(meshcore_identity_export_is_asked_once_and_wiped_once_taken, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    MESH_TEST_FAIL_IF(mesh_meshcore_export_identity(&g_meshcore) != 1, "the export was not sent");
    MESH_TEST_FAIL_IF(mesh_test_meshcore_wire_last(&wire) != MESH_MESHCORE_CMD_EXPORT_PRIVATE_KEY ||
                          wire.lens[wire.count - 1U] != 1U,
                      "the export is not the one code byte");
    MESH_TEST_FAIL_IF(mesh_meshcore_export_identity(&g_meshcore) != -EBUSY,
                      "a second export went out while the first was unanswered");

    feed_private_key(&protocol, 0x11);
    MESH_TEST_FAIL_IF(g_meshcore.identity_state != MESH_MESHCORE_IDENTITY_EXPORTED,
                      "the key that arrived was not kept for the backup");
    MESH_TEST_FAIL_IF(mesh_meshcore_export_identity(&g_meshcore) != -EBUSY,
                      "an export went out over a key nobody had taken");
    uint8_t key[MESH_MESHCORE_PRVKEY_LEN];
    MESH_TEST_FAIL_IF(!mesh_meshcore_take_identity(&g_meshcore, key) || key[0] != 0x11 ||
                          key[MESH_MESHCORE_PRVKEY_LEN - 1U] != 0x11,
                      "the key was not handed over");
    static const uint8_t zero[MESH_MESHCORE_PRVKEY_LEN] = {0};
    MESH_TEST_FAIL_IF(memcmp(g_meshcore.identity_key, zero, sizeof zero) != 0 ||
                          g_meshcore.identity_state != MESH_MESHCORE_IDENTITY_IDLE,
                      "the conversation kept a copy of the key it handed over");
    MESH_TEST_FAIL_IF(mesh_meshcore_take_identity(&g_meshcore, key),
                      "the key was handed over twice");
    record_success(test_name);
}

/* A build without the command and a radio that refused are two answers, and say so. */
MESH_TEST_CASE(meshcore_identity_disabled_refused_and_lost_are_told_apart, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    (void)mesh_meshcore_export_identity(&g_meshcore);
    feed_code(&protocol, MESH_MESHCORE_RESP_DISABLED);
    const bool disabled = g_meshcore.identity_state == MESH_MESHCORE_IDENTITY_DISABLED;
    mesh_meshcore_identity_clear(&g_meshcore);

    uint8_t key[MESH_MESHCORE_PRVKEY_LEN];
    memset(key, 0x22, sizeof key);
    (void)mesh_meshcore_import_identity(&g_meshcore, key);
    const uint8_t refusal[] = {MESH_MESHCORE_RESP_ERR, MESH_MESHCORE_ERR_ILLEGAL_ARG};
    feed(&protocol, refusal, sizeof refusal);
    const bool refused = g_meshcore.identity_state == MESH_MESHCORE_IDENTITY_REFUSED &&
                         g_meshcore.identity_error == MESH_MESHCORE_ERR_ILLEGAL_ARG;
    mesh_meshcore_identity_clear(&g_meshcore);

    (void)mesh_meshcore_export_identity(&g_meshcore);
    mesh_protocol_tick(&protocol, g_meshcore.awaiting_since_ms + MESH_MESHCORE_REPLY_TIMEOUT_MS);
    const bool lost = g_meshcore.identity_state == MESH_MESHCORE_IDENTITY_LOST;
    MESH_TEST_FAIL_IF(!disabled, "a build without the command was not said to be one");
    MESH_TEST_FAIL_IF(!refused, "a refused key did not keep the radio's reason");
    MESH_TEST_FAIL_IF(!lost, "an unanswered export was left waiting");
    record_success(test_name);
}

/* The import's frame is the key: gone from the queue once answered, and from the call's own
   buffer. What is left of it is the radio's OK. */
MESH_TEST_CASE(meshcore_identity_import_is_wiped_from_the_queue_once_answered, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    uint8_t key[MESH_MESHCORE_PRVKEY_LEN];
    memset(key, 0x33, sizeof key);
    MESH_TEST_FAIL_IF(mesh_meshcore_import_identity(&g_meshcore, key) != 1,
                      "the import was not sent");
    const size_t last = wire.count - 1U;
    MESH_TEST_FAIL_IF(wire.frames[last][0] != MESH_MESHCORE_CMD_IMPORT_PRIVATE_KEY ||
                          wire.lens[last] != 1U + MESH_MESHCORE_PRVKEY_LEN ||
                          wire.frames[last][1] != 0x33 ||
                          wire.frames[last][MESH_MESHCORE_PRVKEY_LEN] != 0x33,
                      "the import is not the code and the 64 bytes");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(g_meshcore.identity_state != MESH_MESHCORE_IDENTITY_IMPORTED,
                      "the radio's OK did not settle the import");
    MESH_TEST_FAIL_IF(queue_holds(0x33), "the key stayed in the command queue");
    record_success(test_name);
}

/* In a backup, the key is a section only the two identity calls read: the backup reads, compares
   and makes a profile exactly as it did without it. */
MESH_TEST_CASE(meshcore_backup_identity_rides_along_unseen, unit) {
    static struct mesh_radio_backup plain;
    static struct mesh_radio_backup profile;
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_capture(&g_meshcore, &plain) != 0, "capture failed");
    MESH_TEST_FAIL_IF(plain.header.has_identity, "a capture carried a key");
    g_backup = plain;
    uint8_t key[MESH_MESHCORE_PRVKEY_LEN];
    memset(key, 0x44, sizeof key);
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_add_identity(&g_backup, key) != 0 ||
                          !g_backup.header.has_identity,
                      "the key was not added");
    uint8_t back[MESH_MESHCORE_PRVKEY_LEN];
    MESH_TEST_FAIL_IF(!mesh_meshcore_backup_identity(&g_backup, back) ||
                          memcmp(back, key, sizeof key) != 0,
                      "the key did not come back out");
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_read(&g_backup, &g_contents) != 0,
                      "a keyed backup no longer reads");
    MESH_TEST_FAIL_IF(mesh_meshcore_backup_diff(&g_backup, &plain, &g_diff) != 0 ||
                          g_diff.total != 0U,
                      "the key showed up as a difference");
    const struct mesh_radio_backup_parts every = {.topics = UINT32_MAX, .modules = UINT32_MAX};
    MESH_TEST_FAIL_IF(mesh_radio_profile_make(&g_backup, &every, "Kit", &profile) != 0,
                      "the profile was not made");
    MESH_TEST_FAIL_IF(profile.header.has_identity || mesh_radio_backup_identity(&profile, NULL),
                      "the key went into a profile");
    record_success(test_name);
}

/* A key that arrives after its export was given up on answers nothing outstanding now: the
   save written meanwhile stays the head of the queue, and the key is not kept. */
MESH_TEST_CASE(meshcore_identity_late_key_leaves_the_next_command_alone, unit) {
    struct mesh_protocol protocol;
    struct mesh_test_meshcore_wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "handshake did not reach READY");
    (void)mesh_meshcore_export_identity(&g_meshcore);
    mesh_protocol_tick(&protocol, g_meshcore.awaiting_since_ms + MESH_MESHCORE_REPLY_TIMEOUT_MS);
    struct mesh_meshcore_settings_write write;
    memset(&write, 0, sizeof write);
    write.set_tx_power = true;
    write.tx_power_dbm = 10;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) <= 0,
                      "the save was not sent");
    const size_t queued = g_meshcore.queue_count;
    const uint8_t head = g_meshcore.queue[g_meshcore.queue_head].frame[0];
    feed_private_key(&protocol, 0x66);
    MESH_TEST_FAIL_IF(!g_meshcore.awaiting || g_meshcore.queue_count != queued ||
                          g_meshcore.queue[g_meshcore.queue_head].frame[0] != head ||
                          g_meshcore.writes_outstanding == 0U,
                      "a late key popped the save outstanding in its place");
    MESH_TEST_FAIL_IF(g_meshcore.identity_state == MESH_MESHCORE_IDENTITY_EXPORTED,
                      "a key nobody was waiting for was kept");
    record_success(test_name);
}
