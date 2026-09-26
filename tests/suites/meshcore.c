/*
 * MeshCore's companion protocol: the framing a serial link wraps it in, the codec, and the
 * conversation that fills the session's model.
 *
 * The SELF_INFO and DEVICE_INFO frames below are the ones a Heltec V3 on companion-v1.17.1
 * sent over BLE, byte for byte; the rest are built to the firmware's layouts
 * (examples/companion_radio/MyMesh.cpp).
 */

#include "framework/mesh_test.h"

#include "inkwell/base/time.h"
#include "mesh/core/meshcore.h"
#include "mesh/core/message.h"
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"
#include "mesh/proto/ble_profile.h"
#include "mesh/proto/stream_framing.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ captured from a radio */

static const uint8_t k_device_info[] = {
    0x0d, 0x0d, 0xaf, 0x28, 0x1a, 0xa5, 0x09, 0x00, 0x31, 0x34, 0x2d, 0x41, 0x75, 0x67,
    0x2d, 0x32, 0x30, 0x32, 0x36, 0x00, 0x48, 0x65, 0x6c, 0x74, 0x65, 0x63, 0x20, 0x56,
    0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x76, 0x31, 0x2e, 0x31, 0x37, 0x2e, 0x31, 0x2d, 0x64, 0x39,
    0x32, 0x39, 0x36, 0x34, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t k_self_info[] = {
    0x05, 0x01, 0x16, 0x16, 0xb8, 0xda, 0x09, 0xb0, 0x98, 0xf4, 0xdd, 0x8c, 0x5d, 0xe6, 0x21, 0xf1,
    0xfc, 0x92, 0x3b, 0x02, 0x84, 0xb3, 0xf6, 0xc7, 0x72, 0x7d, 0x41, 0x29, 0xeb, 0x10, 0xa2, 0xb2,
    0xe3, 0xd4, 0xa4, 0x58, 0x2a, 0xd7, 0x17, 0x01, 0xdf, 0x18, 0xfe, 0xfb, 0x00, 0x00, 0x00, 0x00,
    0xbd, 0xe4, 0x0d, 0x00, 0x24, 0xf4, 0x00, 0x00, 0x07, 0x05, 0x4d, 0x50, 0x42, 0x43,
};

/* ------------------------------------------------------------------ frames built to layout */

static void put_u32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
}

/* A 148-byte contact record for a key starting with `lead`. */
static size_t build_contact(uint8_t *out, uint8_t code, uint8_t lead, const char *name,
                            uint8_t type, uint8_t path_len, uint32_t lastmod) {
    memset(out, 0, 148U);
    size_t i = 0U;
    out[i++] = code;
    for (size_t k = 0; k < 32U; ++k) {
        out[i + k] = (uint8_t)(lead + k);
    }
    i += 32U;
    out[i++] = type;
    out[i++] = 0U;
    out[i++] = path_len;
    i += 64U;
    memcpy(out + i, name, strlen(name));
    i += 32U;
    put_u32(out + i, lastmod - 60U); /* the sender's own advert stamp */
    i += 4U;
    put_u32(out + i, (uint32_t)(int32_t)37774900);
    i += 4U;
    put_u32(out + i, (uint32_t)(int32_t)-122419400);
    i += 4U;
    put_u32(out + i, lastmod);
    i += 4U;
    return i;
}

/* ------------------------------------------------------------------ a fake link */

struct wire {
    uint8_t frames[32][MESH_MESHCORE_MAX_FRAME];
    size_t lens[32];
    uint32_t ids[32];
    size_t count;
};

static int wire_send(void *ctx, const uint8_t *frame, size_t len, uint32_t frame_id) {
    struct wire *wire = ctx;
    if (wire->count >= 32U || len > MESH_MESHCORE_MAX_FRAME) {
        return -ENOBUFS;
    }
    memcpy(wire->frames[wire->count], frame, len);
    wire->lens[wire->count] = len;
    wire->ids[wire->count] = frame_id;
    wire->count += 1U;
    return 0;
}

static uint8_t wire_last(const struct wire *wire) {
    return wire->count > 0U ? wire->frames[wire->count - 1U][0] : 0U;
}

static struct mesh_session g_model;
static struct mesh_meshcore g_meshcore;

static void feed(const struct mesh_protocol *protocol, const uint8_t *frame, size_t len) {
    mesh_protocol_receive(protocol, frame, len);
}

static void feed_code(const struct mesh_protocol *protocol, uint8_t code) {
    feed(protocol, &code, 1U);
}

/* Drives a handshake to READY: one known contact ("Alice", key 0x40...), one channel. */
static bool handshake(struct mesh_protocol *protocol, struct wire *wire) {
    mesh_session_init(&g_model);
    mesh_meshcore_init(&g_meshcore, &g_model);
    *protocol = mesh_meshcore_protocol(&g_meshcore);
    memset(wire, 0, sizeof *wire);
    mesh_protocol_attach(protocol, wire_send, wire);
    if (mesh_protocol_begin(protocol) != 0 || wire_last(wire) != MESH_MESHCORE_CMD_DEVICE_QUERY ||
        wire->count != 1U) {
        return false;
    }
    uint8_t device[sizeof k_device_info];
    memcpy(device, k_device_info, sizeof device);
    device[3] = 1U; /* one channel slot, so the walk is short */
    feed(protocol, device, sizeof device);
    if (wire_last(wire) != MESH_MESHCORE_CMD_APP_START) {
        return false;
    }
    feed(protocol, k_self_info, sizeof k_self_info);
    if (wire_last(wire) == MESH_MESHCORE_CMD_SET_DEVICE_TIME) {
        feed_code(protocol, MESH_MESHCORE_RESP_OK);
    }
    if (wire_last(wire) != MESH_MESHCORE_CMD_GET_CONTACTS) {
        return false;
    }
    uint8_t frame[160];
    uint8_t start[5] = {MESH_MESHCORE_RESP_CONTACTS_START, 1, 0, 0, 0};
    feed(protocol, start, sizeof start);
    feed(protocol, frame,
         build_contact(frame, MESH_MESHCORE_RESP_CONTACT, 0x40, "Alice", MESH_MESHCORE_ADV_CHAT, 2U,
                       1700000000U));
    uint8_t end[5] = {MESH_MESHCORE_RESP_END_OF_CONTACTS};
    put_u32(end + 1, 1700000000U);
    feed(protocol, end, sizeof end);
    if (wire_last(wire) != MESH_MESHCORE_CMD_GET_CHANNEL) {
        return false;
    }
    uint8_t channel[2 + 32 + 16];
    memset(channel, 0, sizeof channel);
    channel[0] = MESH_MESHCORE_RESP_CHANNEL_INFO;
    memcpy(channel + 2, "Public", 6U);
    channel[34] = 0x8b;
    feed(protocol, channel, sizeof channel);
    /* Ready: the message queue is drained, then the battery asked for. */
    if (wire_last(wire) != MESH_MESHCORE_CMD_SYNC_NEXT_MESSAGE) {
        return false;
    }
    feed_code(protocol, MESH_MESHCORE_RESP_NO_MORE_MESSAGES);
    if (wire_last(wire) != MESH_MESHCORE_CMD_GET_BATT_AND_STORAGE) {
        return false;
    }
    uint8_t battery[11] = {MESH_MESHCORE_RESP_BATT_AND_STORAGE, 0x10, 0x0f};
    feed(protocol, battery, sizeof battery);
    return mesh_meshcore_ready(&g_meshcore);
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
    MESH_TEST_FAIL_IF(
        mesh_meshcore_decode_device_info(k_device_info, sizeof k_device_info, &device) != 0,
        "DEVICE_INFO decodes");
    MESH_TEST_FAIL_IF(device.firmware_version != 13U || device.max_contacts != 350U ||
                          device.max_channels != 40U || device.ble_pin != 632090U,
                      "version 13, 350 contacts, 40 channels, the fixed PIN");
    MESH_TEST_FAIL_IF(strcmp(device.model, "Heltec V3") != 0 ||
                          strcmp(device.version, "v1.17.1-d929643") != 0 ||
                          strcmp(device.build_date, "14-Aug-2026") != 0,
                      "the fixed-width strings stop at their padding");

    struct mesh_meshcore_self_info self;
    MESH_TEST_FAIL_IF(mesh_meshcore_decode_self_info(k_self_info, sizeof k_self_info, &self) != 0,
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
    MESH_TEST_FAIL_IF(mesh_meshcore_decode_self_info(k_self_info, 20U, &self) != -EBADMSG,
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
    const int len = mesh_meshcore_encode_text(prefix, 1U, 0x01020304U, "hey", out, sizeof out);
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
    static struct wire wire;
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
 * A push that a message is waiting starts the drain, and the drain runs until the radio says
 * there is no more - a direct message resolves to the contact whose key it starts with, and a
 * channel message's "Name: " becomes its sender when a node goes by that name.
 */
MESH_TEST_CASE(meshcore_drains_messages_into_the_log, unit) {
    struct mesh_protocol protocol;
    static struct wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");

    feed_code(&protocol, MESH_MESHCORE_PUSH_MSG_WAITING);
    MESH_TEST_FAIL_IF(wire_last(&wire) != MESH_MESHCORE_CMD_SYNC_NEXT_MESSAGE,
                      "a waiting message is asked for");
    const uint8_t direct[] = {16,   0x14, 0, 0, 0x40, 0x41, 0x42, 0x43, 0x44,
                              0x45, 0xFF, 0, 0, 0,    0,    0,    'y',  'o'};
    const size_t before = wire.count;
    feed(&protocol, direct, sizeof direct);
    MESH_TEST_FAIL_IF(wire.count != before + 1U ||
                          wire_last(&wire) != MESH_MESHCORE_CMD_SYNC_NEXT_MESSAGE,
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
    static struct wire wire;
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
    put_u32(sent + 2, 0xDEADBEEFU);
    put_u32(sent + 6, 1000U);
    feed(&protocol, sent, sizeof sent);

    /* The wrong ack changes nothing; the right one delivers. */
    uint8_t confirmed[9] = {MESH_MESHCORE_PUSH_SEND_CONFIRMED};
    put_u32(confirmed + 1, 0x0BADF00DU);
    feed(&protocol, confirmed, sizeof confirmed);
    MESH_TEST_FAIL_IF(newest_message()->ack != MESH_MESSAGE_ACK_PENDING, "another ack is ignored");
    put_u32(confirmed + 1, 0xDEADBEEFU);
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
        put_u32(sent + 2, 0x1000U + attempt);
        feed(&protocol, sent, sizeof sent);
        now = g_meshcore.now_ms + 60000U;
        mesh_protocol_tick(&protocol, now);
        if (attempt + 2U == MESH_MESHCORE_SEND_ATTEMPTS) {
            MESH_TEST_FAIL_IF(wire_last(&wire) != MESH_MESHCORE_CMD_RESET_PATH,
                              "the last attempt resets the route first");
            feed_code(&protocol, MESH_MESHCORE_RESP_OK);
        }
    }
    const struct mesh_message *failed = mesh_message_log_find(&g_model.messages, packet_id);
    MESH_TEST_FAIL_IF(failed == NULL || failed->ack != MESH_MESSAGE_ACK_FAILED,
                      "unacknowledged after every attempt, it failed");

    MESH_TEST_FAIL_IF(mesh_meshcore_send_text(&g_meshcore, MESH_MESSAGE_BROADCAST_ADDR, 0U, "all",
                                              &packet_id) != 0 ||
                          wire_last(&wire) != MESH_MESHCORE_CMD_SEND_CHANNEL_TXT_MSG,
                      "a channel message goes out");
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);
    MESH_TEST_FAIL_IF(newest_message()->ack != MESH_MESSAGE_ACK_NONE,
                      "OK is all a channel message gets, and it is not left pending");
    record_success(test_name);
}

/* A command the radio never answers is given up on, and two in a row is a dead link. */
MESH_TEST_CASE(meshcore_unanswered_commands_end_the_link, unit) {
    struct mesh_protocol protocol;
    static struct wire wire;
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
    static struct wire wire;
    memset(&wire, 0, sizeof wire);
    mesh_session_init(&g_model);
    mesh_meshcore_init(&g_meshcore, &g_model);
    const struct mesh_protocol protocol = mesh_meshcore_protocol(&g_meshcore);
    mesh_protocol_attach(&protocol, wire_send, &wire);

    uint32_t packet_id = 0U;
    const bool undated = mesh_meshcore_send_text(&g_meshcore, MESH_MESSAGE_BROADCAST_ADDR, 0U, "a",
                                                 &packet_id) == 0 &&
                         frame_u32(wire.frames[wire.count - 1U] + 3) == 0U;
    feed_code(&protocol, MESH_MESHCORE_RESP_OK);

    (void)mesh_protocol_begin(&protocol);
    feed(&protocol, k_device_info, sizeof k_device_info);
    feed(&protocol, k_self_info, sizeof k_self_info);
    const bool asked = wire_last(&wire) == MESH_MESHCORE_CMD_GET_DEVICE_TIME;
    uint8_t clock[5] = {MESH_MESHCORE_RESP_CURR_TIME};
    put_u32(clock + 1, 1750000000U);
    feed(&protocol, clock, sizeof clock);
    const bool walked_on = wire_last(&wire) == MESH_MESHCORE_CMD_GET_CONTACTS;
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
    static struct wire wire;
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
    static struct wire wire;
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
    static struct wire wire;
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
    static struct wire wire;
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
    static struct wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    const struct mesh_node_summary *self =
        mesh_session_model_node(&g_model, g_meshcore.self_node, false);
    MESH_TEST_FAIL_IF(self == NULL || !self->position.valid, "the captured radio has a fix");

    MESH_TEST_FAIL_IF(mesh_meshcore_refresh_settings(&g_meshcore) != 1, "a refresh is asked");
    uint8_t cleared[sizeof k_self_info];
    memcpy(cleared, k_self_info, sizeof cleared);
    memset(cleared + 36, 0, 8U); /* lat_e6, lon_e6 */
    feed(&protocol, cleared, sizeof cleared);
    self = mesh_session_model_node(&g_model, g_meshcore.self_node, false);
    MESH_TEST_FAIL_IF(self == NULL || self->position.valid, "and the fix goes with the location");
    record_success(test_name);
}

/* A save is each group's command and then APP_START, whose SELF_INFO is the read-back; the
   answers settle into the write counters the app's save toast watches. */
MESH_TEST_CASE(meshcore_settings_write_is_commands_then_a_read_back, unit) {
    struct mesh_protocol protocol;
    static struct wire wire;
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
    MESH_TEST_FAIL_IF(wire_last(&wire) != MESH_MESHCORE_CMD_APP_START, "and the read-back follows");

    /* A refusal fails the save with the radio's own code. */
    const uint32_t failed = settings->writes_failed;
    feed(&protocol, k_self_info, sizeof k_self_info);
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

MESH_TEST_CASE(meshcore_settings_write_refuses_what_it_cannot_send_whole, unit) {
    struct mesh_protocol protocol;
    static struct wire wire;
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
    write.set_name = true;
    write.name[0] = '\0';
    const size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_meshcore_write_settings(&g_meshcore, &write) != -EINVAL ||
                          g_meshcore.writes_outstanding != 0U,
                      "an empty name is refused before anything is queued");
    MESH_TEST_FAIL_IF(wire.count != before, "and nothing reached the radio");
    record_success(test_name);
}

/* A reboot is never answered, and over a USB bridge the port stays open while the ESP32 behind
   it resets - so once the answer is overdue the conversation starts over by itself. */
MESH_TEST_CASE(meshcore_reboot_syncs_again, unit) {
    struct mesh_protocol protocol;
    static struct wire wire;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake walks to ready");
    MESH_TEST_FAIL_IF(mesh_meshcore_reboot(&g_meshcore) != 1, "the reboot is queued");
    MESH_TEST_FAIL_IF(wire_last(&wire) != MESH_MESHCORE_CMD_REBOOT ||
                          wire.lens[wire.count - 1U] != 7U ||
                          memcmp(wire.frames[wire.count - 1U] + 1, "reboot", 6U) != 0,
                      "carrying the word the firmware checks for");
    mesh_protocol_tick(&protocol, g_meshcore.awaiting_since_ms + MESH_MESHCORE_REPLY_TIMEOUT_MS);
    MESH_TEST_FAIL_IF(wire_last(&wire) != MESH_MESHCORE_CMD_DEVICE_QUERY,
                      "the silence that follows starts the handshake over");
    MESH_TEST_FAIL_IF(mesh_protocol_silent(&protocol), "and is not counted against the link");
    record_success(test_name);
}
