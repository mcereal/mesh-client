/*
 * Tern's conversation: the client half of draft/companion.md, driven through its struct
 * mesh_protocol as a link would drive it, with what it learns read back off the session model.
 * The first three cases replay the specification's own exchange, its older clients' connections
 * and its update over two connections (tests/data/tern_companion.json); the rest are the rules
 * around them - a lapsed connection, missed news and the sync after it, the version both ends
 * speak, silence and the ping, positions, cards, join codes and updates.
 */

#include "framework/mesh_test.h"
#include "support/data_fixture.h"

#include "inkwell/base/time.h"
#include "inkwell/codec/json.h"
#include "mesh/core/message.h"
#include "mesh/core/session.h"
#include "mesh/core/tern.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct mesh_session g_model;
static struct mesh_tern g_tern;
/* The image an UPDATE_BEGIN in a replay starts an update of. */
static uint8_t g_image[512];
static size_t g_image_len;

/* The exchange's node and its contact, as the vectors give them. */
static const uint8_t k_node[MESH_TERN_ADDRESS_LEN] = {
    0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
    0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
static const uint8_t k_bob[MESH_TERN_ADDRESS_LEN] = {
    0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a, 0x92, 0xb7, 0x0a, 0xa7, 0x4d, 0x1b, 0x7e, 0xbc,
    0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4, 0x96, 0x8c, 0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c};
#define EXCHANGE_TIME 1790000000U
#define NEIGHBOUR_ID 0x1d2e3f40U

struct wire {
    uint8_t frames[32][MESH_TERN_MAX_FRAME];
    size_t lens[32];
    size_t count;
};

static int wire_send(void *ctx, const uint8_t *frame, size_t len, uint32_t frame_id) {
    (void)frame_id;
    struct wire *wire = ctx;
    if (wire->count >= 32U || len > MESH_TERN_MAX_FRAME) {
        return -ENOBUFS;
    }
    memcpy(wire->frames[wire->count], frame, len);
    wire->lens[wire->count] = len;
    wire->count += 1U;
    return 0;
}

static const uint8_t *wire_last(const struct wire *wire) {
    return wire->count > 0U ? wire->frames[wire->count - 1U] : NULL;
}

static struct mesh_protocol start(struct wire *wire) {
    memset(wire, 0, sizeof *wire);
    inkwell_time_wall_set_fixed(EXCHANGE_TIME);
    mesh_session_init(&g_model);
    mesh_tern_init(&g_tern, &g_model);
    struct mesh_protocol protocol = mesh_tern_protocol(&g_tern);
    mesh_protocol_attach(&protocol, wire_send, wire);
    (void)mesh_protocol_begin(&protocol);
    return protocol;
}

static void feed(const struct mesh_protocol *protocol, const uint8_t *frame, size_t len) {
    mesh_protocol_receive(protocol, frame, len);
}

/* An answer, of `type`, to the request last on the wire. */
static void answer(const struct mesh_protocol *protocol, const struct wire *wire, uint8_t type) {
    const uint8_t frame[2] = {type, wire_last(wire)[1]};
    feed(protocol, frame, sizeof frame);
}

/* The version a node writes in: the one the two ends agreed, once they have. */
static uint8_t spoken(void) { return g_tern.has_agreed ? g_tern.agreed : MESH_TERN_VERSION; }

static void news(const struct mesh_protocol *protocol, const struct mesh_tern_frame *frame) {
    uint8_t out[MESH_TERN_MAX_FRAME];
    const int len = mesh_tern_encode(frame, spoken(), out, sizeof out);
    if (len > 0) {
        feed(protocol, out, (size_t)len);
    }
}

/* SYNCED to the request last on the wire, with `count` as the node's news count. */
static void synced_at(const struct mesh_protocol *protocol, const struct wire *wire,
                      uint8_t count) {
    const struct mesh_tern_frame frame = {
        .type = MESH_TERN_SYNCED, .seq = wire_last(wire)[1], .news = count};
    news(protocol, &frame);
}

/* SYNCED with the count the client expects: every news frame of the sync arrived. */
static void synced(const struct mesh_protocol *protocol, const struct wire *wire) {
    synced_at(protocol, wire, g_tern.news_expected);
}

/* The SYNC last on the wire's `after`, or UINT32_MAX when the last frame is not a SYNC. */
static uint32_t last_sync_after(const struct wire *wire) {
    struct mesh_tern_frame sync;
    if (wire->count == 0U || wire_last(wire)[0] != MESH_TERN_SYNC ||
        mesh_tern_decode(wire_last(wire), wire->lens[wire->count - 1U], MESH_TERN_VERSION, &sync) !=
            MESH_TERN_DECODE_OK) {
        return UINT32_MAX;
    }
    return sync.after;
}

static const struct mesh_node_summary *model_node(uint32_t id) {
    for (size_t i = 0; i < g_model.handshake.node_count; ++i) {
        if (g_model.handshake.nodes[i].node_id == id) {
            return &g_model.handshake.nodes[i];
        }
    }
    return NULL;
}

/* INFO from a node of `version`, SET_TIME, OK, SYNC, a sync of SELF alone: READY. */
static bool greet_as(struct mesh_protocol *protocol, struct wire *wire, uint8_t version,
                     const uint8_t address[MESH_TERN_ADDRESS_LEN]) {
    if (wire->count == 0U || wire_last(wire)[0] != MESH_TERN_HELLO) {
        return false;
    }
    struct mesh_tern_frame info = {
        .type = MESH_TERN_INFO, .seq = wire_last(wire)[1], .version = version};
    news(protocol, &info);
    answer(protocol, wire, MESH_TERN_OK); /* SET_TIME */
    if (wire_last(wire)[0] != MESH_TERN_SYNC) {
        return false;
    }
    struct mesh_tern_frame self = {.type = MESH_TERN_SELF, .seq = g_tern.news_expected};
    memcpy(self.address, address, MESH_TERN_ADDRESS_LEN);
    news(protocol, &self);
    synced(protocol, wire);
    return mesh_tern_ready(&g_tern);
}

static bool greet(struct mesh_protocol *protocol, struct wire *wire, uint8_t version) {
    return greet_as(protocol, wire, version, k_node);
}

/* HELLO and greet(): news counted from 0 again. */
static bool handshake_at(struct mesh_protocol *protocol, struct wire *wire, uint8_t version) {
    *protocol = start(wire);
    return greet(protocol, wire, version);
}

static bool handshake(struct mesh_protocol *protocol, struct wire *wire) {
    return handshake_at(protocol, wire, MESH_TERN_VERSION);
}

/* The link drops and comes back to the same node, which now speaks `version`. */
static bool reconnect_at(struct mesh_protocol *protocol, struct wire *wire, uint8_t version) {
    mesh_protocol_detach(protocol);
    memset(wire, 0, sizeof *wire);
    mesh_protocol_attach(protocol, wire_send, wire);
    (void)mesh_protocol_begin(protocol);
    return greet(protocol, wire, version);
}

/* An address in the vectors, as 64 hex digits. */
static void unhex(const char *hex, uint8_t *out, size_t out_len, size_t *len) {
    *len = 0U;
    for (const char *p = hex; p[0] != '\0' && p[1] != '\0' && *len < out_len; p += 2) {
        const char pair[3] = {p[0], p[1], '\0'};
        out[(*len)++] = (uint8_t)strtoul(pair, NULL, 16);
    }
}

/* What the user does, read off the frame the vectors show the client sending for it. HELLO,
   SET_TIME and SYNC are the conversation's own, and need nothing done. */
static int act(const struct mesh_tern_frame *want) {
    const uint32_t node = mesh_tern_routing_id(want->address);
    switch (want->type) {
    case MESH_TERN_READ:
        return mesh_tern_mark_read(&g_tern, mesh_tern_packet_id(&g_tern, want->through));
    case MESH_TERN_SEND:
        return mesh_tern_send_text(&g_tern, node, want->text, NULL);
    case MESH_TERN_SAVE_CONTACT:
        return mesh_tern_save_contact(&g_tern, want->address, want->text);
    case MESH_TERN_END_SESSION:
        return mesh_tern_end_session(&g_tern, node);
    case MESH_TERN_MAKE_GROUP:
        return mesh_tern_make_group(&g_tern, want->text);
    case MESH_TERN_LEAVE_GROUP:
        return mesh_tern_leave_group(&g_tern, want->group);
    case MESH_TERN_NAME_GROUP:
        return mesh_tern_name_group(&g_tern, want->group, want->text);
    case MESH_TERN_SEND_GROUP:
        return mesh_tern_send_group(&g_tern, want->group, want->text);
    case MESH_TERN_SEND_INVITE:
        return mesh_tern_send_invite(&g_tern, want->group, node);
    case MESH_TERN_JOIN:
        return mesh_tern_join(&g_tern, want->id);
    case MESH_TERN_SET:
        if (want->setting == MESH_TERN_SETTING_REGION ||
            want->setting == MESH_TERN_SETTING_CARD_NAME) {
            return mesh_tern_set_text(&g_tern, want->setting, want->text);
        }
        /* The receiver's 1 and 0 are mesh_tern_set_receiver()'s; the exchange's 2, which the node
           refuses, only a raw SET can say. */
        if (want->setting == MESH_TERN_SETTING_RECEIVER && want->receiver <= 1U) {
            return mesh_tern_set_receiver(&g_tern, want->receiver != 0U);
        }
        return mesh_tern_set_value(&g_tern, want->setting,
                                   want->setting == MESH_TERN_SETTING_ROLE    ? want->role
                                   : want->setting == MESH_TERN_SETTING_POWER ? (uint8_t)want->power
                                   : want->setting == MESH_TERN_SETTING_CARDS ? want->cards
                                   : want->setting == MESH_TERN_SETTING_RECEIVER ? want->receiver
                                                                                 : want->passkey);
    case MESH_TERN_OFF:
        return mesh_tern_turn_off(&g_tern);
    case MESH_TERN_SET_POSITION:
        return mesh_tern_set_position(&g_tern, want->lat, want->lon, want->altitude, want->accuracy,
                                      (uint16_t)want->age);
    case MESH_TERN_SHARE:
    case MESH_TERN_SHARE_GROUP: {
        const struct mesh_tern_share share = {.precision = want->precision,
                                              .fields = want->fields,
                                              .interval = want->interval,
                                              .minutes = want->minutes};
        return want->type == MESH_TERN_SHARE ? mesh_tern_share(&g_tern, node, &share)
                                             : mesh_tern_share_group(&g_tern, want->group, &share);
    }
    case MESH_TERN_GROUP_LINK:
        return mesh_tern_group_link(&g_tern, want->group);
    case MESH_TERN_JOIN_LINK:
        return mesh_tern_join_link(&g_tern, want->text);
    case MESH_TERN_UPDATE_BEGIN: {
        /* The user starts one; after a dropped link the conversation goes on with it itself. */
        const enum mesh_tern_update_phase phase = mesh_tern_update_status(&g_tern)->phase;
        return phase == MESH_TERN_UPDATE_IDLE || phase == MESH_TERN_UPDATE_FINISHED
                   ? mesh_tern_update_start(&g_tern, g_image, g_image_len)
                   : 0;
    }
    default:
        return 0;
    }
}

/*
 * One connection of the vectors, the array under the cursor, frame by frame as the client of
 * `version`: the node's frames are fed, and each of the client's is what the user's action makes
 * it send - but for a SEND's or SEND_GROUP's `ref`, which a client chooses at random. A request
 * the version spoken does not define is refused here and not sent; the node's answer to the
 * client that sent it anyway is then one to nothing asked.
 */
static void replay(struct inkwell_json *json, struct mesh_protocol *protocol, struct wire *wire,
                   size_t *steps, char *failure, size_t failure_len) {
    size_t sent = 0U; /* client frames matched so far, the HELLO on the wire already among them */
    while (failure[0] == '\0' && inkwell_json_next_element(json)) {
        char from[16] = "";
        char type[32] = "";
        char hex[512] = "";
        char key[16];
        bool ok = inkwell_json_enter_object(json);
        while (ok && inkwell_json_next_key(json, key, sizeof key)) {
            if (strcmp(key, "from") == 0) {
                ok = inkwell_json_read_string(json, from, sizeof from);
            } else if (strcmp(key, "type") == 0) {
                ok = inkwell_json_read_string(json, type, sizeof type);
            } else if (strcmp(key, "frame") == 0) {
                ok = inkwell_json_read_string(json, hex, sizeof hex);
            } else {
                ok = inkwell_json_skip_value(json);
            }
        }
        uint8_t frame[MESH_TERN_MAX_FRAME];
        size_t len = 0U;
        unhex(hex, frame, sizeof frame, &len);
        struct mesh_tern_frame want;
        if (!ok || len < 2U) {
            snprintf(failure, failure_len, "step %zu should read", *steps);
        } else if (strcmp(from, "node") == 0) {
            feed(protocol, frame, len);
        } else if (mesh_tern_decode(frame, len, MESH_TERN_VERSION, &want) != MESH_TERN_DECODE_OK) {
            snprintf(failure, failure_len, "step %zu: the client's %s should read", *steps, type);
        } else if (mesh_tern_frame_since(frame, len) > g_tern.agreed) {
            if (act(&want) != -EOPNOTSUPP || wire->count != sent) {
                snprintf(failure, failure_len, "step %zu: %s goes at version %u", *steps, type,
                         (unsigned)g_tern.agreed);
            }
        } else {
            const int acted = act(&want);
            const size_t skip =
                want.type == MESH_TERN_SEND || want.type == MESH_TERN_SEND_GROUP ? 6U : 2U;
            if (acted != 0 || wire->count != sent + 1U || wire->lens[sent] != len ||
                memcmp(wire->frames[sent], frame, 2U) != 0 ||
                memcmp(wire->frames[sent] + skip, frame + skip, len - skip) != 0) {
                snprintf(failure, failure_len, "step %zu: the client's %s differs (%d)", *steps,
                         type, acted);
            }
            sent += 1U;
        }
        *steps += 1U;
    }
}

/* A conversation as the client of `version`, with the wall clock one not worth giving a node. */
static struct mesh_protocol start_as(struct wire *wire, uint8_t version) {
    memset(wire, 0, sizeof *wire);
    inkwell_time_wall_set_fixed(1000U);
    mesh_session_init(&g_model);
    mesh_tern_init(&g_tern, &g_model);
    g_tern.version = version;
    struct mesh_protocol protocol = mesh_tern_protocol(&g_tern);
    mesh_protocol_attach(&protocol, wire_send, wire);
    return protocol;
}

static const uint8_t k_hut[MESH_TERN_GROUP_LEN] = {0xc0, 0x03, 0x7d, 0xcd, 0x77, 0xf6, 0xea, 0xc6};

static size_t group_item_at(uint32_t id) {
    for (size_t i = 0; i < g_tern.group_item_count; ++i) {
        if (g_tern.group_items[i].id == id) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* The vectors' exchange, as the client of version 9 it is. */
MESH_TEST_CASE(tern_replays_the_specifications_exchange, unit) {
    size_t doc_len = 0U;
    char *document = mesh_test_data_read("tern_companion.json", &doc_len);
    MESH_TEST_FAIL_IF(document == NULL, "the vectors should be there");
    struct inkwell_json json;
    inkwell_json_init(&json, document, doc_len);
    const bool found =
        inkwell_json_object_find(&json, "exchange") && inkwell_json_enter_array(&json);
    MESH_TEST_FAIL_IF_CLEANUP(!found, free(document), "the exchange should be there");

    struct wire wire;
    struct mesh_protocol protocol = start(&wire);
    size_t steps = 0U;
    char failure[160] = "";
    replay(&json, &protocol, &wire, &steps, failure, sizeof failure);
    free(document);
    MESH_TEST_FAIL_IF(failure[0] != '\0', failure);
    MESH_TEST_FAIL_IF(steps != 102U, "all 102 steps");
    MESH_TEST_FAIL_IF(g_tern.agreed != 9U || g_tern.missed_news || g_tern.news_expected != 42U,
                      "version 9 throughout, every news frame counted and none missed");
    MESH_TEST_FAIL_IF(strcmp(g_tern.board, "heltec-v3") != 0 ||
                          strcmp(g_tern.release, "0.2.0") != 0,
                      "INFO names the board and the release, which an update is found by");

    /* And what it learned. */
    const uint32_t self_id = mesh_tern_routing_id(k_node);
    const uint32_t bob_id = mesh_tern_routing_id(k_bob);
    MESH_TEST_FAIL_IF(g_model.handshake.my_info.my_node_num != self_id,
                      "the node is the radio, known by its routing id");
    /* The exchange ends with OFF answered OK: the node is turning off. */
    const size_t sent = wire.count;
    mesh_protocol_tick(&protocol,
                       g_tern.answered_ms + MESH_TERN_IDLE_MS + MESH_TERN_ANSWER_WAIT_MS);
    MESH_TEST_FAIL_IF(mesh_tern_ready(&g_tern) || g_tern.phase != MESH_TERN_TURNED_OFF ||
                          !mesh_tern_turned_off(&g_tern) || wire.count != sent ||
                          mesh_protocol_silent(&protocol),
                      "OFF answered: nothing more is sent, not even a PING, and nothing is gone");
    MESH_TEST_FAIL_IF(mesh_tern_take_off(&g_tern, NULL) != MESH_TERN_OFF_DONE ||
                          mesh_tern_take_off(&g_tern, NULL) != MESH_TERN_OFF_NONE,
                      "and the app is told once");
    const struct mesh_node_summary *bob = model_node(bob_id);
    MESH_TEST_FAIL_IF(bob == NULL || strcmp(bob->long_name, "Bob") != 0 || !bob->in_nodedb ||
                          bob->public_key_len != 32U || memcmp(bob->public_key, k_bob, 32U) != 0,
                      "Bob is a contact, by name, with his address");
    MESH_TEST_FAIL_IF(g_tern.contact_count != 3U || g_tern.contacts[0].session != 0U,
                      "Carol and the card's sender are saved beside him; his session has ended");
    const struct mesh_node_summary *neighbour = model_node(NEIGHBOUR_ID);
    MESH_TEST_FAIL_IF(neighbour == NULL || neighbour->snr != -9.5f || !neighbour->in_nodedb ||
                          neighbour->last_heard != EXCHANGE_TIME - 42U ||
                          neighbour->hops_away != 0U,
                      "the neighbour, heard 42 s ago at -9.5 dB, directly");
    const struct mesh_node_summary *self = model_node(self_id);
    MESH_TEST_FAIL_IF(self == NULL || !self->metrics.has_voltage ||
                          self->metrics.voltage != 3.987f || self->metrics.battery_level != 81U,
                      "the node's battery");
    MESH_TEST_FAIL_IF(!g_model.stats.valid || g_model.stats.air_util_tx < 0.342f ||
                          g_model.stats.air_util_tx > 0.344f,
                      "12.345 s of an hour on the air");

    struct mesh_message *received =
        mesh_message_log_find(&g_model.messages, mesh_tern_packet_id(&g_tern, 17U));
    MESH_TEST_FAIL_IF(received == NULL || received->direction != MESH_MESSAGE_INBOUND ||
                          received->from != bob_id ||
                          strcmp(received->text, "Where are you?") != 0 || !received->replayed ||
                          received->rx_time != 1789999000U,
                      "Bob's message, under the node's id, handed back by the sync");
    size_t copies = 0U;
    for (size_t i = 0; i < g_model.messages.count; ++i) {
        copies += mesh_message_log_at(&g_model.messages, i)->packet_id ==
                          mesh_tern_packet_id(&g_tern, 17U)
                      ? 1U
                      : 0U;
    }
    MESH_TEST_FAIL_IF(copies != 1U, "marked read, the record lands on the entry it already has");
    MESH_TEST_FAIL_IF(
        g_model.messages.count != 2U,
        "group messages and invites are not direct messages, and stay out of the log");

    struct mesh_tern_queued queued;
    MESH_TEST_FAIL_IF(!mesh_tern_take_queued(&g_tern, &queued) ||
                          queued.id != mesh_tern_packet_id(&g_tern, 18U) || queued.ticket != 1U ||
                          mesh_tern_take_queued(&g_tern, &queued),
                      "QUEUED's id, once, under the send's ticket");
    const struct mesh_message *mine =
        mesh_message_log_find(&g_model.messages, mesh_tern_packet_id(&g_tern, 18U));
    MESH_TEST_FAIL_IF(mine == NULL || mine->direction != MESH_MESSAGE_OUTBOUND ||
                          mine->to != bob_id || mine->ack != MESH_MESSAGE_ACK_DELIVERED ||
                          mine->replayed,
                      "our message, waiting, then sent, then delivered");

    MESH_TEST_FAIL_IF(!g_tern.has_asked || g_tern.asked_why != MESH_TERN_ASKED_NOT_CONTACT,
                      "the node said whom it refused, and why");
    static const uint8_t k_ridge[MESH_TERN_GROUP_LEN] = {0xc8, 0xea, 0xfa, 0xdc,
                                                         0x08, 0x57, 0xa6, 0x96};
    MESH_TEST_FAIL_IF(g_tern.group_count != 2U ||
                          memcmp(g_tern.groups[0].id, k_ridge, sizeof k_ridge) != 0 ||
                          strcmp(g_tern.groups[0].name, "Ridge walkers") != 0 ||
                          memcmp(g_tern.groups[1].id, k_hut, sizeof k_hut) != 0,
                      "the group joined and renamed is held, and the one left is back from its "
                      "join code");
    const size_t invite = group_item_at(19U);
    const size_t written = group_item_at(20U);
    const size_t heard = group_item_at(21U);
    const size_t asked_in = group_item_at(22U);
    MESH_TEST_FAIL_IF(invite == SIZE_MAX || written == SIZE_MAX || heard == SIZE_MAX ||
                          asked_in == SIZE_MAX,
                      "both group messages and both invites are held");
    MESH_TEST_FAIL_IF(g_tern.group_items[invite].state != MESH_TERN_STATE_DELIVERED ||
                          g_tern.group_items[written].state != MESH_TERN_STATE_SENT ||
                          g_tern.group_items[heard].from != 0xf031219dU ||
                          strcmp(g_tern.group_items[heard].text, "Two of us") != 0 ||
                          (g_tern.group_items[asked_in].flags & MESH_TERN_MESSAGE_READ) == 0U,
                      "each as its record and STATEs left it");
    MESH_TEST_FAIL_IF(
        g_tern.open_count != 0U,
        "and nothing is left that may change: all delivered, sent to a group, or read");

    MESH_TEST_FAIL_IF(bob->position.valid != true || bob->position.latitude_i != 603945922 ||
                          bob->position.longitude_i != 52871704 ||
                          bob->position.precision_bits != 16U || bob->position.has_altitude ||
                          bob->position.time != EXCHANGE_TIME - 40U,
                      "Bob's position, on his roster entry, a cell of 2^-16 turns, 40 s old");
    MESH_TEST_FAIL_IF(g_tern.sharing_count != 0U, "sharing with him was turned on, then off");
    MESH_TEST_FAIL_IF(g_tern.self.cards != 0U ||
                          strcmp(g_tern.self.card_name, "Ada \xc2\xb7 hut warden") != 0,
                      "cards on, then off; their name kept while they are off");
    MESH_TEST_FAIL_IF(g_tern.card_count != 0U, "the card held from the start was forgotten");
    char link[MESH_TERN_LINK_MAX + 1U];
    MESH_TEST_FAIL_IF(!mesh_tern_take_link(&g_tern, link, sizeof link) ||
                          strncmp(link, "HTTPS://TERNMESH.ORG/G#", 23U) != 0 ||
                          mesh_tern_take_link(&g_tern, link, sizeof link),
                      "the join code asked for, had once");
    /* The receiver turned off, on again, and its last fix still the node's, 40 s old. */
    MESH_TEST_FAIL_IF(!g_tern.has_fix || g_tern.fix.receiver != MESH_TERN_RECEIVER_ON ||
                          g_tern.fix.source != MESH_TERN_FIX_RECEIVER ||
                          g_tern.fix.satellites != 9U || g_tern.fix.accuracy != 3U,
                      "the node's own fix, its receiver's, from nine satellites");
    MESH_TEST_FAIL_IF(!self->position.valid || self->position.latitude_i != 458325412 ||
                          self->position.longitude_i != 68643871 || !self->position.has_altitude ||
                          self->position.altitude != 4809 || self->position.sats_in_view != 9U ||
                          self->position.time != EXCHANGE_TIME - 40U ||
                          self->position.precision_bits != 0U,
                      "on the radio's roster entry, which the map and Settings read");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

/*
 * Each older connection, as the client of its version, against the same node. The node's INFO
 * says 9 throughout; the client speaks its own. The client of version 0 has synced message 17
 * at version 0 before, which is why it asks after it.
 */
MESH_TEST_CASE(tern_replays_the_older_clients_connections, unit) {
    size_t doc_len = 0U;
    char *document = mesh_test_data_read("tern_companion.json", &doc_len);
    MESH_TEST_FAIL_IF(document == NULL, "the vectors should be there");
    struct inkwell_json json;
    inkwell_json_init(&json, document, doc_len);
    const bool found = inkwell_json_object_find(&json, "older") && inkwell_json_enter_array(&json);
    MESH_TEST_FAIL_IF_CLEANUP(!found, free(document), "the older connections should be there");

    size_t connections = 0U;
    size_t steps = 0U;
    char failure[160] = "";
    struct wire wire;
    while (failure[0] == '\0' && inkwell_json_next_element(&json)) {
        uint64_t version = 99U;
        char key[16];
        const bool read =
            inkwell_json_object_find(&json, "version") && inkwell_json_read_u64(&json, &version) &&
            inkwell_json_next_key(&json, key, sizeof key) && strcmp(key, "frames") == 0 &&
            inkwell_json_enter_array(&json) && version <= MESH_TERN_VERSION;
        if (!read) {
            snprintf(failure, sizeof failure, "older connection %zu should read", connections);
            break;
        }
        struct mesh_protocol protocol = start_as(&wire, (uint8_t)version);
        if (version == 0U) {
            g_tern.has_synced_version = true;
            g_tern.synced_version = 0U;
            g_tern.newest_id = 17U;
        }
        (void)mesh_protocol_begin(&protocol);
        replay(&json, &protocol, &wire, &steps, failure, sizeof failure);
        while (inkwell_json_next_key(&json, key, sizeof key)) {
            (void)inkwell_json_skip_value(&json);
        }
        if (failure[0] == '\0' && (g_tern.agreed != version || !mesh_tern_ready(&g_tern) ||
                                   g_tern.missed_news || g_tern.contact_count == 0U)) {
            snprintf(failure, sizeof failure,
                     "version %u: synced, by its own version, with nothing missed",
                     (unsigned)version);
        }
        connections += 1U;
    }
    free(document);
    MESH_TEST_FAIL_IF(failure[0] != '\0', failure);
    MESH_TEST_FAIL_IF(connections != 9U || steps != 115U, "nine connections, 115 steps");
    MESH_TEST_FAIL_IF(g_tern.synced_version != 8U || g_tern.has_missed_since ||
                          g_tern.card_count != 1U || !g_tern.has_fix ||
                          g_tern.off != MESH_TERN_OFF_NONE,
                      "the last, version 8's, synced with the card and the fix it is sent, and "
                      "never sent OFF");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

/*
 * The vectors' update: one image over two connections, the first lost after its last frame. The
 * client starts the update once synced, sends UPDATE_CHUNK at a time, and after the link drops
 * goes on by itself from the offset the node gives, once the next connection has synced.
 */
MESH_TEST_CASE(tern_replays_the_specifications_update_over_two_connections, unit) {
    size_t doc_len = 0U;
    char *document = mesh_test_data_read("tern_companion.json", &doc_len);
    MESH_TEST_FAIL_IF(document == NULL, "the vectors should be there");
    struct inkwell_json json;
    inkwell_json_init(&json, document, doc_len);
    char hex[2U * sizeof g_image + 1U];
    const bool image = inkwell_json_object_find(&json, "image") &&
                       inkwell_json_read_string(&json, hex, sizeof hex);
    unhex(hex, g_image, sizeof g_image, &g_image_len);
    inkwell_json_init(&json, document, doc_len);
    const bool found = image && g_image_len == 400U && inkwell_json_object_find(&json, "update") &&
                       inkwell_json_enter_array(&json);
    MESH_TEST_FAIL_IF_CLEANUP(!found, free(document), "the update and its image should be there");

    struct wire wire;
    struct mesh_protocol protocol = start(&wire);
    size_t connections = 0U;
    size_t steps = 0U;
    char failure[160] = "";
    while (failure[0] == '\0' && inkwell_json_next_element(&json)) {
        if (connections > 0U) {
            /* The link was lost: what the client sent after the last frame never arrived. */
            if (mesh_tern_update_status(&g_tern)->phase != MESH_TERN_UPDATE_SENDING ||
                wire_last(&wire)[0] != MESH_TERN_UPDATE_DATA) {
                snprintf(failure, sizeof failure, "the first connection ends mid-update");
                break;
            }
            mesh_protocol_detach(&protocol);
            if (mesh_tern_update_status(&g_tern)->phase != MESH_TERN_UPDATE_WAITING) {
                snprintf(failure, sizeof failure, "a dropped link leaves the update waiting");
                break;
            }
            memset(&wire, 0, sizeof wire);
            mesh_protocol_attach(&protocol, wire_send, &wire);
            /* The vectors number each connection's requests from 1. This client's count runs on
               across connections - an answer from the last can never be taken for one of this
               one's - so it is started where the vectors start. */
            g_tern.next_seq = 0U;
            (void)mesh_protocol_begin(&protocol);
        }
        if (!inkwell_json_enter_array(&json)) {
            snprintf(failure, sizeof failure, "connection %zu should read", connections);
            break;
        }
        replay(&json, &protocol, &wire, &steps, failure, sizeof failure);
        connections += 1U;
    }
    free(document);
    MESH_TEST_FAIL_IF(failure[0] != '\0', failure);
    MESH_TEST_FAIL_IF(connections != 2U || steps != 32U, "two connections, 32 steps");
    const struct mesh_tern_update *update = mesh_tern_update_status(&g_tern);
    MESH_TEST_FAIL_IF(update->phase != MESH_TERN_UPDATE_FINISHED ||
                          update->outcome != MESH_TERN_UPDATE_RESTARTING || update->held != 400U ||
                          update->image != NULL,
                      "UPDATE_END answered: the node restarts into the image, which is the "
                      "caller's again");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

/* The answer to the request last on the wire: ERROR `code`, or UPDATING at `offset`. */
static void refuse(const struct mesh_protocol *protocol, const struct wire *wire, uint8_t code) {
    const uint8_t frame[3] = {MESH_TERN_ERROR, wire_last(wire)[1], code};
    feed(protocol, frame, sizeof frame);
}

static void updating(const struct mesh_protocol *protocol, const struct wire *wire,
                     uint32_t offset) {
    const struct mesh_tern_frame frame = {
        .type = MESH_TERN_UPDATING, .seq = wire_last(wire)[1], .offset = offset};
    news(protocol, &frame);
}

MESH_TEST_CASE(tern_an_update_goes_on_from_where_the_node_says_and_ends_once, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    static uint8_t image[400];
    for (size_t i = 0; i < sizeof image; ++i) {
        image[i] = (uint8_t)i;
    }
    MESH_TEST_FAIL_IF(mesh_tern_update_start(&g_tern, image, 0U) != -EINVAL,
                      "an image is at least a byte");
    MESH_TEST_FAIL_IF(mesh_tern_update_start(&g_tern, image, sizeof image) != 0 ||
                          wire_last(&wire)[0] != MESH_TERN_UPDATE_BEGIN,
                      "UPDATE_BEGIN first, asking where to go on from");
    MESH_TEST_FAIL_IF(mesh_tern_update_start(&g_tern, image, sizeof image) != -EBUSY,
                      "one update at a time");
    updating(&protocol, &wire, 0U);
    struct mesh_tern_frame data;
    MESH_TEST_FAIL_IF(mesh_tern_decode(wire_last(&wire), wire.lens[wire.count - 1U],
                                       MESH_TERN_VERSION, &data) != MESH_TERN_DECODE_OK ||
                          data.type != MESH_TERN_UPDATE_DATA || data.offset != 0U ||
                          data.data_len != MESH_TERN_UPDATE_CHUNK,
                      "then UPDATE_CHUNK bytes from the offset");
    /* The node lost track: ERROR 10 asks where to go on from again. */
    refuse(&protocol, &wire, MESH_TERN_ERR_NOT_THERE);
    MESH_TEST_FAIL_IF(wire_last(&wire)[0] != MESH_TERN_UPDATE_BEGIN,
                      "ERROR 10 to UPDATE_DATA is an UPDATE_BEGIN again");
    updating(&protocol, &wire, 9999U);
    MESH_TEST_FAIL_IF(wire_last(&wire)[0] != MESH_TERN_UPDATE_END,
                      "a node that says it holds more than the image is asked to end it");
    refuse(&protocol, &wire, MESH_TERN_ERR_NOT_AN_IMAGE);
    const struct mesh_tern_update *update = mesh_tern_update_status(&g_tern);
    MESH_TEST_FAIL_IF(update->phase != MESH_TERN_UPDATE_FINISHED ||
                          update->outcome != MESH_TERN_UPDATE_REFUSED ||
                          update->code != MESH_TERN_ERR_NOT_AN_IMAGE,
                      "ERROR 11: not an image this node runs, and discarded");

    /* Again, with the link lost while UPDATE_END is out: never sent again. */
    MESH_TEST_FAIL_IF(mesh_tern_update_start(&g_tern, image, sizeof image) != 0,
                      "a finished update makes room for another");
    updating(&protocol, &wire, sizeof image);
    MESH_TEST_FAIL_IF(wire_last(&wire)[0] != MESH_TERN_UPDATE_END, "all held: UPDATE_END");
    MESH_TEST_FAIL_IF(mesh_tern_update_cancel(&g_tern), "too late to cancel");
    mesh_protocol_detach(&protocol);
    MESH_TEST_FAIL_IF(update->outcome != MESH_TERN_UPDATE_UNCONFIRMED,
                      "the node may be restarting into the image: wait, and read its release");
    MESH_TEST_FAIL_IF(!reconnect_at(&protocol, &wire, MESH_TERN_VERSION) ||
                          wire_last(&wire)[0] == MESH_TERN_UPDATE_END,
                      "and nothing of it is sent on the next connection");

    /* Cancelled while a chunk is out: the answer comes first, then nothing more. */
    MESH_TEST_FAIL_IF(mesh_tern_update_start(&g_tern, image, sizeof image) != 0, "a third");
    updating(&protocol, &wire, 0U);
    MESH_TEST_FAIL_IF(!mesh_tern_update_cancel(&g_tern), "cancelled before UPDATE_END");
    const size_t before = wire.count;
    answer(&protocol, &wire, MESH_TERN_OK);
    MESH_TEST_FAIL_IF(wire.count != before || update->outcome != MESH_TERN_UPDATE_CANCELLED,
                      "the chunk out is answered, and no other follows");

    /* The link drops mid-update and comes back to another radio: its image is not this one's. */
    MESH_TEST_FAIL_IF(mesh_tern_update_start(&g_tern, image, sizeof image) != 0, "a fourth");
    updating(&protocol, &wire, 0U);
    mesh_protocol_detach(&protocol);
    MESH_TEST_FAIL_IF(update->phase != MESH_TERN_UPDATE_WAITING, "it waits for the link");
    uint8_t other[MESH_TERN_ADDRESS_LEN];
    memcpy(other, k_node, sizeof other);
    other[0] ^= 0x01U;
    memset(&wire, 0, sizeof wire);
    mesh_protocol_attach(&protocol, wire_send, &wire);
    (void)mesh_protocol_begin(&protocol);
    /* Another node's SELF has its books forgotten and the sync asked again from 0. */
    if (!greet_as(&protocol, &wire, MESH_TERN_VERSION, other)) {
        synced(&protocol, &wire);
    }
    MESH_TEST_FAIL_IF(!mesh_tern_ready(&g_tern), "the other radio should reach READY");
    for (size_t i = 0; i < wire.count; ++i) {
        MESH_TEST_FAIL_IF(wire.frames[i][0] == MESH_TERN_UPDATE_BEGIN ||
                              wire.frames[i][0] == MESH_TERN_UPDATE_DATA,
                          "nothing of the update goes to another node");
    }
    MESH_TEST_FAIL_IF(update->phase != MESH_TERN_UPDATE_FINISHED ||
                          update->outcome != MESH_TERN_UPDATE_ANOTHER_NODE || update->image != NULL,
                      "it finishes, and the image is the caller's again");

    MESH_TEST_FAIL_IF(!handshake_at(&protocol, &wire, 3U) ||
                          mesh_tern_update_start(&g_tern, image, sizeof image) != -EOPNOTSUPP,
                      "a node of version 3 has no updates");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_positions_sharing_and_cards_are_whole_lists_from_their_version, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    const uint32_t bob_id = mesh_tern_routing_id(k_bob);
    struct mesh_tern_frame position = {.type = MESH_TERN_POSITION,
                                       .seq = 1U,
                                       .precision = 20U,
                                       .lat = 458325000,
                                       .lon = 68644000,
                                       .altitude = 4806,
                                       .accuracy = 4U,
                                       .age = 30U};
    memcpy(position.address, k_bob, sizeof k_bob);
    news(&protocol, &position);
    const struct mesh_node_summary *bob = model_node(bob_id);
    MESH_TEST_FAIL_IF(bob == NULL || !bob->position.valid || !bob->position.has_altitude ||
                          bob->position.altitude != 4806 || bob->position.precision_bits != 20U ||
                          bob->position.received != EXCHANGE_TIME ||
                          bob->position.time != EXCHANGE_TIME - 30U,
                      "a contact's position lands on his roster entry, received now");
    struct mesh_tern_frame from_group = {.type = MESH_TERN_GROUP_POSITION,
                                         .seq = 2U,
                                         .from = NEIGHBOUR_ID,
                                         .precision = 12U,
                                         .altitude = MESH_TERN_NO_ALTITUDE};
    memcpy(from_group.group, k_hut, sizeof k_hut);
    news(&protocol, &from_group);
    MESH_TEST_FAIL_IF(g_tern.group_position_count != 1U || model_node(NEIGHBOUR_ID) != NULL,
                      "a group member's is kept, and makes no roster entry of a claimed id");
    struct mesh_tern_frame sharing = {
        .type = MESH_TERN_SHARING, .seq = 3U, .precision = 16U, .interval = 900U, .minutes = 60U};
    memcpy(sharing.address, k_bob, sizeof k_bob);
    news(&protocol, &sharing);
    MESH_TEST_FAIL_IF(g_tern.sharing_count != 1U || g_tern.sharing[0].ends != EXCHANGE_TIME + 3600U,
                      "sharing with him, for an hour from now");
    struct mesh_tern_frame card = {
        .type = MESH_TERN_CARD, .seq = 4U, .heard = 60U, .text_len = 4U, .text = "Pine"};
    memcpy(card.address, k_node, sizeof k_node);
    card.address[0] ^= 0x01U; /* a stranger */
    news(&protocol, &card);
    const uint32_t stranger = mesh_tern_routing_id(card.address);
    const struct mesh_node_summary *about = model_node(stranger);
    MESH_TEST_FAIL_IF(about == NULL || !about->has_card || strcmp(about->card_name, "Pine") != 0 ||
                          about->card_heard != EXCHANGE_TIME - 60U || about->has_user ||
                          strstr(about->long_name, "Pine") != NULL,
                      "a card is about its sender, beside and never in the place of a name");

    /* Precision 0: the node no longer holds Bob's position. */
    position.seq = 5U;
    position.precision = 0U;
    news(&protocol, &position);
    MESH_TEST_FAIL_IF(model_node(bob_id)->position.valid, "a record of precision 0 forgets it");
    position.seq = 6U;
    position.precision = 20U;
    news(&protocol, &position);

    /* A sync that sends none of them: each is gone, and sharing is off. */
    const struct mesh_tern_frame power = {.type = MESH_TERN_POWER, .seq = 9U};
    news(&protocol, &power);
    struct mesh_tern_frame self = {.type = MESH_TERN_SELF, .seq = 10U};
    memcpy(self.address, k_node, sizeof k_node);
    news(&protocol, &self);
    synced(&protocol, &wire);
    MESH_TEST_FAIL_IF(!mesh_tern_ready(&g_tern) || model_node(bob_id)->position.valid ||
                          g_tern.position_count != 0U || g_tern.group_position_count != 0U ||
                          g_tern.sharing_count != 0U || g_tern.card_count != 0U ||
                          model_node(stranger)->has_card,
                      "what a version 7 sync did not send, the node does not hold");

    /* At version 5 the sync says nothing of cards, which version 6 added. */
    card.seq = 11U;
    news(&protocol, &card);
    MESH_TEST_FAIL_IF(!reconnect_at(&protocol, &wire, 5U) || g_tern.card_count != 1U,
                      "a version 5 sync sends no cards, which says nothing of them");
    MESH_TEST_FAIL_IF(!reconnect_at(&protocol, &wire, 6U) || g_tern.card_count != 0U,
                      "a version 6 one is the whole list");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_a_join_code_crosses_the_link_once_and_is_kept_by_nobody, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    static const char k_code[] = "HTTPS://TERNMESH.ORG/G#AAAQEAYEAUDAOCAJBIFQYDIOB7UBUSDVOQ";
    MESH_TEST_FAIL_IF(mesh_tern_join_link(&g_tern, k_code) != 0, "JOIN_LINK goes");
    MESH_TEST_FAIL_IF(memmem(g_tern.queue, sizeof g_tern.queue, "TERNMESH", 8U) == NULL,
                      "it is on the wire, from the queue's head");
    const struct mesh_tern_frame made = {.type = MESH_TERN_MADE, .seq = wire_last(&wire)[1]};
    news(&protocol, &made);
    MESH_TEST_FAIL_IF(memmem(g_tern.queue, sizeof g_tern.queue, "TERNMESH", 8U) != NULL,
                      "and once answered nothing of it is left in the queue");
    MESH_TEST_FAIL_IF(mesh_tern_group_link(&g_tern, k_hut) != 0, "GROUP_LINK goes");
    struct mesh_tern_frame link = {.type = MESH_TERN_LINK,
                                   .seq = wire_last(&wire)[1],
                                   .text_len = (uint8_t)(sizeof k_code - 1U)};
    memcpy(link.text, k_code, sizeof k_code);
    news(&protocol, &link);
    char out[MESH_TERN_LINK_MAX + 1U];
    MESH_TEST_FAIL_IF(!mesh_tern_take_link(&g_tern, out, sizeof out) || strcmp(out, k_code) != 0,
                      "the code, for the screen that shows it");
    MESH_TEST_FAIL_IF(mesh_tern_take_link(&g_tern, out, sizeof out) ||
                          memmem(&g_tern, sizeof g_tern, "TERNMESH", 8U) != NULL,
                      "once: then the conversation holds nothing of it");
    /* A code not taken is never the answer to the next request. */
    MESH_TEST_FAIL_IF(mesh_tern_group_link(&g_tern, k_hut) != 0, "GROUP_LINK again");
    link.seq = wire_last(&wire)[1];
    news(&protocol, &link);
    MESH_TEST_FAIL_IF(mesh_tern_group_link(&g_tern, k_hut) != 0 ||
                          mesh_tern_take_link(&g_tern, out, sizeof out) ||
                          memmem(&g_tern, sizeof g_tern, "TERNMESH", 8U) != NULL,
                      "asking again wipes the code the last request was answered with");
    MESH_TEST_FAIL_IF(!handshake_at(&protocol, &wire, 6U) ||
                          mesh_tern_join_link(&g_tern, k_code) != -EOPNOTSUPP ||
                          mesh_tern_group_link(&g_tern, k_hut) != -EOPNOTSUPP,
                      "a node of version 6 has no join codes");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_a_setting_the_version_lacks_is_never_sent, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake_at(&protocol, &wire, 5U), "the handshake should reach READY");
    const size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_tern_set_value(&g_tern, MESH_TERN_SETTING_CARDS, 1U) != -EOPNOTSUPP ||
                          mesh_tern_set_text(&g_tern, MESH_TERN_SETTING_CARD_NAME, "Ada") !=
                              -EOPNOTSUPP ||
                          wire.count != before,
                      "cards are version 6's: refused here, not sent");
    MESH_TEST_FAIL_IF(mesh_tern_set_value(&g_tern, MESH_TERN_SETTING_REGION, 1U) != -EINVAL ||
                          mesh_tern_set_text(&g_tern, MESH_TERN_SETTING_ROLE, "1") != -EINVAL,
                      "and a value of the wrong kind for its setting is no SET at all");
    MESH_TEST_FAIL_IF(mesh_tern_set_text(&g_tern, MESH_TERN_SETTING_REGION, "EU868") != 0 ||
                          wire_last(&wire)[0] != MESH_TERN_SET,
                      "the region is version 0's");
    answer(&protocol, &wire, MESH_TERN_OK);
    MESH_TEST_FAIL_IF(mesh_tern_set_position(&g_tern, 900000001, 0, 0, 0U, 0U) != -EINVAL,
                      "a latitude past the pole");
    MESH_TEST_FAIL_IF(mesh_tern_set_position(&g_tern, -336183000, -704517000, MESH_TERN_NO_ALTITUDE,
                                             0U, 600U) != 0 ||
                          wire_last(&wire)[0] != MESH_TERN_SET_POSITION,
                      "positions are version 5's");

    /* Asked before INFO said the node's version, and found to be one it lacks. */
    struct mesh_protocol early = start(&wire);
    MESH_TEST_FAIL_IF(mesh_tern_set_value(&g_tern, MESH_TERN_SETTING_CARDS, 1U) != 0,
                      "queued behind the HELLO");
    const struct mesh_tern_frame info = {
        .type = MESH_TERN_INFO, .seq = wire_last(&wire)[1], .version = 5U};
    news(&early, &info);
    for (size_t i = 0; i < wire.count; ++i) {
        MESH_TEST_FAIL_IF(wire.frames[i][0] == MESH_TERN_SET,
                          "a node of version 5 is never sent SET 5");
    }
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_message_states_follow_the_node, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    struct mesh_tern_frame message = {.type = MESH_TERN_MESSAGE,
                                      .seq = 1U,
                                      .id = 5U,
                                      .state = MESH_TERN_STATE_WAITING,
                                      .reason = MESH_TERN_WAIT_ROUTE,
                                      .text_len = 2U,
                                      .text = "hi"};
    memcpy(message.address, k_bob, sizeof k_bob);
    news(&protocol, &message);
    const struct mesh_message *logged =
        mesh_message_log_find(&g_model.messages, mesh_tern_packet_id(&g_tern, 5U));
    MESH_TEST_FAIL_IF(logged == NULL || logged->ack != MESH_MESSAGE_ACK_WAITING ||
                          logged->ack_error != MESH_TERN_WAIT_ROUTE || logged->replayed,
                      "waiting for a route, and live rather than replayed");
    struct mesh_tern_frame state = {
        .type = MESH_TERN_STATE, .seq = 2U, .id = 5U, .state = MESH_TERN_STATE_NOT_DELIVERED};
    news(&protocol, &state);
    MESH_TEST_FAIL_IF(logged->ack != MESH_MESSAGE_ACK_FAILED, "its source gave it up");
    MESH_TEST_FAIL_IF(g_tern.missed_news, "news counted 0, 1, 2 is none missed");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_missed_news_syncs_again_from_what_may_have_moved, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    struct mesh_tern_frame message = {.type = MESH_TERN_MESSAGE,
                                      .seq = 1U,
                                      .id = 7U,
                                      .state = MESH_TERN_STATE_SENT,
                                      .text_len = 1U,
                                      .text = "x"};
    memcpy(message.address, k_bob, sizeof k_bob);
    news(&protocol, &message);
    message.seq = 2U;
    message.id = 9U;
    message.state = MESH_TERN_STATE_DELIVERED;
    news(&protocol, &message);
    const size_t before = wire.count;
    /* Count 3 never came. */
    struct mesh_tern_frame power = {.type = MESH_TERN_POWER, .seq = 4U, .percent = 50U};
    news(&protocol, &power);
    MESH_TEST_FAIL_IF(wire.count != before + 1U || wire_last(&wire)[0] != MESH_TERN_SYNC,
                      "a gap in the count is a sync asked for");
    MESH_TEST_FAIL_IF(last_sync_after(&wire) != 6U,
                      "after one less than the least message still sent, whose state may have "
                      "moved unseen");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_a_sync_is_the_whole_contact_list, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    struct mesh_tern_frame contact = {
        .type = MESH_TERN_CONTACT, .seq = 1U, .text_len = 3U, .text = "Bob"};
    memcpy(contact.address, k_bob, sizeof k_bob);
    news(&protocol, &contact);
    const uint32_t bob_id = mesh_tern_routing_id(k_bob);
    MESH_TEST_FAIL_IF(model_node(bob_id) == NULL || !model_node(bob_id)->in_nodedb,
                      "a contact saved live is on the node");

    /* A sync that leaves Bob out: the node no longer holds him. */
    struct mesh_tern_frame power = {.type = MESH_TERN_POWER, .seq = 5U};
    news(&protocol, &power); /* out of count, so a sync is asked */
    MESH_TEST_FAIL_IF(wire_last(&wire)[0] != MESH_TERN_SYNC, "a sync is asked");
    synced(&protocol, &wire);
    MESH_TEST_FAIL_IF(model_node(bob_id) == NULL, "the roster outlives what the node holds");
    MESH_TEST_FAIL_IF(model_node(bob_id)->in_nodedb || model_node(bob_id)->has_user ||
                          g_tern.contact_count != 0U,
                      "but he is no longer the node's contact, nor named");
    MESH_TEST_FAIL_IF(mesh_tern_send_text(&g_tern, bob_id, "hello?", NULL) != 0,
                      "a node the roster has an address for can still be written to");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_a_lapsed_connection_says_hello_again, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    MESH_TEST_FAIL_IF(mesh_tern_mark_read(&g_tern, 3U) != 0, "a READ goes");
    const uint8_t refused[3] = {MESH_TERN_ERROR, wire_last(&wire)[1], MESH_TERN_ERR_HELLO_FIRST};
    feed(&protocol, refused, sizeof refused);
    MESH_TEST_FAIL_IF(wire_last(&wire)[0] != MESH_TERN_HELLO || mesh_tern_ready(&g_tern),
                      "ERROR 6 after HELLO was answered is a client taken for gone");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_pings_when_idle_and_calls_silence_gone, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    const uint64_t t0 = g_tern.answered_ms;
    const size_t before = wire.count;
    mesh_protocol_tick(&protocol, t0 + MESH_TERN_IDLE_MS - 1U);
    MESH_TEST_FAIL_IF(wire.count != before, "nothing before IDLE");
    mesh_protocol_tick(&protocol, t0 + MESH_TERN_IDLE_MS);
    MESH_TEST_FAIL_IF(wire.count != before + 1U || wire_last(&wire)[0] != MESH_TERN_PING,
                      "a PING once IDLE has passed since the last answer");
    MESH_TEST_FAIL_IF(mesh_protocol_silent(&protocol), "not yet gone");

    /* An answer to another request is not this one's. */
    const uint8_t stray[2] = {MESH_TERN_OK, (uint8_t)(wire_last(&wire)[1] + 7U)};
    feed(&protocol, stray, sizeof stray);
    mesh_protocol_tick(&protocol, t0 + MESH_TERN_IDLE_MS + MESH_TERN_ANSWER_WAIT_MS);
    MESH_TEST_FAIL_IF(!mesh_protocol_silent(&protocol),
                      "a request unanswered after ANSWER_WAIT: the node has gone");
    mesh_protocol_detach(&protocol);
    MESH_TEST_FAIL_IF(mesh_protocol_silent(&protocol) || mesh_tern_ready(&g_tern),
                      "and the detach clears it");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_sends_one_request_at_a_time, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    struct mesh_tern_frame contact = {.type = MESH_TERN_CONTACT, .seq = 1U};
    memcpy(contact.address, k_bob, sizeof k_bob);
    news(&protocol, &contact);
    const uint32_t bob = mesh_tern_routing_id(k_bob);
    const size_t before = wire.count;
    uint32_t first = 0U;
    MESH_TEST_FAIL_IF(mesh_tern_send_text(&g_tern, bob, "one", &first) != 0 ||
                          mesh_tern_send_text(&g_tern, bob, "two", NULL) != 0,
                      "both are taken");
    MESH_TEST_FAIL_IF(wire.count != before + 1U, "but only the first goes before its answer");
    const uint8_t queued[6] = {MESH_TERN_QUEUED, wire_last(&wire)[1], 0, 0, 0, 40};
    feed(&protocol, queued, sizeof queued);
    MESH_TEST_FAIL_IF(wire.count != before + 2U || wire_last(&wire)[0] != MESH_TERN_SEND,
                      "and the second on it");
    struct mesh_tern_queued taken;
    MESH_TEST_FAIL_IF(mesh_tern_take_queued(&g_tern, &taken),
                      "a send is not handed out before its MESSAGE is in the log");
    const uint8_t refused[3] = {MESH_TERN_ERROR, wire_last(&wire)[1], MESH_TERN_ERR_FULL};
    feed(&protocol, refused, sizeof refused);
    struct mesh_tern_frame message = {.type = MESH_TERN_MESSAGE,
                                      .seq = 2U,
                                      .id = 40U,
                                      .state = MESH_TERN_STATE_WAITING,
                                      .text_len = 3U,
                                      .text = "one"};
    memcpy(message.address, k_bob, sizeof k_bob);
    news(&protocol, &message);
    MESH_TEST_FAIL_IF(!mesh_tern_take_queued(&g_tern, &taken) || taken.ticket != first ||
                          taken.id != mesh_tern_packet_id(&g_tern, 40U),
                      "the first, once its message is logged");
    MESH_TEST_FAIL_IF(!mesh_tern_take_queued(&g_tern, &taken) || taken.ticket != first + 1U ||
                          taken.id != 0U,
                      "then the second, which the node refused");
    MESH_TEST_FAIL_IF(mesh_tern_send_text(&g_tern, 0x12345678U, "who?", NULL) != -ENOENT,
                      "a node with no address known is refused");
    char long_text[MESH_TERN_TEXT_MAX + 2U];
    memset(long_text, 'x', sizeof long_text - 1U);
    long_text[sizeof long_text - 1U] = '\0';
    MESH_TEST_FAIL_IF(mesh_tern_send_text(&g_tern, bob, long_text, NULL) != -EMSGSIZE,
                      "as is text past 128 bytes");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_the_frame_that_shows_a_gap_is_live_news, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    /* Count 1 never came: this message, at 2, shows the gap. */
    struct mesh_tern_frame message = {.type = MESH_TERN_MESSAGE,
                                      .seq = 2U,
                                      .id = 3U,
                                      .state = MESH_TERN_STATE_RECEIVED,
                                      .text_len = 2U,
                                      .text = "hi"};
    memcpy(message.address, k_bob, sizeof k_bob);
    news(&protocol, &message);
    const struct mesh_message *logged =
        mesh_message_log_find(&g_model.messages, mesh_tern_packet_id(&g_tern, 3U));
    MESH_TEST_FAIL_IF(logged == NULL || logged->replayed,
                      "it arrived live, and is announced as live, not as part of the sync");
    MESH_TEST_FAIL_IF(wire_last(&wire)[0] != MESH_TERN_SYNC, "and only then is a sync asked");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_two_nodes_message_ids_never_meet, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    struct mesh_tern_frame message = {.type = MESH_TERN_MESSAGE,
                                      .seq = 1U,
                                      .id = 1U,
                                      .state = MESH_TERN_STATE_RECEIVED,
                                      .text_len = 5U,
                                      .text = "first"};
    memcpy(message.address, k_bob, sizeof k_bob);
    news(&protocol, &message);
    const uint32_t first = mesh_tern_packet_id(&g_tern, 1U);

    /* Another node on the same run, which counts its messages from 1 too. The log is kept. */
    mesh_protocol_detach(&protocol);
    memset(&wire, 0, sizeof wire);
    mesh_protocol_attach(&protocol, wire_send, &wire);
    (void)mesh_protocol_begin(&protocol);
    struct mesh_tern_frame info = {.type = MESH_TERN_INFO, .seq = wire_last(&wire)[1]};
    news(&protocol, &info);
    answer(&protocol, &wire, MESH_TERN_OK);
    struct mesh_tern_frame self = {.type = MESH_TERN_SELF, .seq = 0U};
    memcpy(self.address, k_bob, sizeof k_bob); /* Bob's node, this time */
    news(&protocol, &self);
    message.seq = 1U;
    memcpy(message.address, k_node, sizeof k_node);
    memcpy(message.text, "other", 5U);
    news(&protocol, &message);
    const uint32_t second = mesh_tern_packet_id(&g_tern, 1U);
    MESH_TEST_FAIL_IF(first == second, "each node's message 1 has a packet id of its own");
    const struct mesh_message *a = mesh_message_log_find(&g_model.messages, first);
    const struct mesh_message *b = mesh_message_log_find(&g_model.messages, second);
    MESH_TEST_FAIL_IF(a == NULL || b == NULL || strcmp(a->text, "first") != 0 ||
                          strcmp(b->text, "other") != 0,
                      "and neither is taken for the other");
    MESH_TEST_FAIL_IF(mesh_tern_packet_id(&g_tern, second) != 1U,
                      "a packet id reads back to the node's id");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

/* Count `seq`, a record of `type` the vectors' node would send, about Bob unless said. */
static struct mesh_tern_frame record(uint8_t type, uint8_t seq, uint32_t id, uint8_t state,
                                     uint8_t flags) {
    struct mesh_tern_frame frame = {.type = type,
                                    .seq = seq,
                                    .id = id,
                                    .state = state,
                                    .flags = flags,
                                    .text_len = 1U,
                                    .text = "x"};
    memcpy(frame.address, k_bob, sizeof k_bob);
    return frame;
}

MESH_TEST_CASE(tern_a_synced_count_past_the_last_news_forgets_nothing, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    struct mesh_tern_frame contact = record(MESH_TERN_CONTACT, 1U, 0U, 0U, 0U);
    news(&protocol, &contact);
    const struct mesh_tern_frame neighbour = {
        .type = MESH_TERN_NEIGHBOUR, .seq = 2U, .routing_id = NEIGHBOUR_ID};
    news(&protocol, &neighbour);
    struct mesh_tern_frame message =
        record(MESH_TERN_MESSAGE, 3U, 5U, MESH_TERN_STATE_RECEIVED, 0U);
    news(&protocol, &message);
    message = record(MESH_TERN_MESSAGE, 4U, 9U, MESH_TERN_STATE_DELIVERED, 0U);
    news(&protocol, &message);
    /* Count 5 never came; 6 shows it. */
    const struct mesh_tern_frame power = {.type = MESH_TERN_POWER, .seq = 6U};
    news(&protocol, &power);
    MESH_TEST_FAIL_IF(last_sync_after(&wire) != 4U,
                      "a sync after one less than the received message still unread");

    /* The sync's last news, count 8, is lost: nothing after it shows the gap but the count. */
    struct mesh_tern_frame self = {.type = MESH_TERN_SELF, .seq = 7U};
    memcpy(self.address, k_node, sizeof k_node);
    news(&protocol, &self);
    const size_t before = wire.count;
    synced_at(&protocol, &wire, 9U);
    MESH_TEST_FAIL_IF(g_tern.contact_count != 1U || g_tern.neighbour_count != 1U ||
                          !model_node(mesh_tern_routing_id(k_bob))->in_nodedb,
                      "what the sync did not send may be what was lost: nothing is forgotten");
    MESH_TEST_FAIL_IF(wire.count != before + 1U || last_sync_after(&wire) != 4U ||
                          mesh_tern_ready(&g_tern),
                      "and the sync is asked again, from where the first missed news left it");

    /* This one arrives whole, and it is the whole list: the neighbour has gone. */
    self.seq = 9U;
    news(&protocol, &self);
    contact.seq = 10U;
    news(&protocol, &contact);
    synced_at(&protocol, &wire, 11U);
    MESH_TEST_FAIL_IF(!mesh_tern_ready(&g_tern) || g_tern.contact_count != 1U ||
                          g_tern.neighbour_count != 0U || g_tern.has_missed_since,
                      "a sync that ends on the count proves what is gone, and clears the mark");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_a_version_2_node_ends_its_sync_in_two_bytes, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake_at(&protocol, &wire, 2U),
                      "a SYNCED of two bytes ends a sync spoken at version 2");
    MESH_TEST_FAIL_IF(g_tern.agreed != 2U || g_tern.synced_version != 2U, "spoken at version 2");
    /* Another sync, whose SYNCED a later node pads with a count: version 2 reads none. */
    const struct mesh_tern_frame power = {.type = MESH_TERN_POWER, .seq = 3U};
    news(&protocol, &power);
    MESH_TEST_FAIL_IF(wire_last(&wire)[0] != MESH_TERN_SYNC, "a gap asks for a sync");
    const size_t before = wire.count;
    const uint8_t padded[3] = {MESH_TERN_SYNCED, wire_last(&wire)[1], 200U};
    feed(&protocol, padded, sizeof padded);
    MESH_TEST_FAIL_IF(!mesh_tern_ready(&g_tern) || wire.count != before,
                      "the byte past version 2's fields is not a count to be short of");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_news_of_a_later_version_is_counted_and_ignored, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake_at(&protocol, &wire, 1U), "the handshake should reach READY");
    const size_t before = wire.count;
    /* A GROUP cut short: at version 1 the type is undefined before it is short. */
    const uint8_t cut_group[4] = {MESH_TERN_GROUP, 1U, 0xc0, 0x03};
    feed(&protocol, cut_group, sizeof cut_group);
    MESH_TEST_FAIL_IF(wire.count != before || g_tern.missed_news || g_tern.news_expected != 2U ||
                          g_tern.group_count != 0U,
                      "news version 1 does not define is ignored, but counted");
    /* An ASKED cut short is version 1's own, and a record lost. */
    const uint8_t cut_asked[3] = {MESH_TERN_ASKED, 2U, 0xfc};
    feed(&protocol, cut_asked, sizeof cut_asked);
    MESH_TEST_FAIL_IF(wire.count != before + 1U || wire_last(&wire)[0] != MESH_TERN_SYNC,
                      "news it defines but cannot read is news missed");

    MESH_TEST_FAIL_IF(!handshake_at(&protocol, &wire, 0U), "the handshake should reach READY");
    const size_t at_zero = wire.count;
    const uint8_t cut_asked_0[3] = {MESH_TERN_ASKED, 1U, 0xfc};
    feed(&protocol, cut_asked_0, sizeof cut_asked_0);
    MESH_TEST_FAIL_IF(wire.count != at_zero || g_tern.missed_news,
                      "and at version 0 the same bytes are a type it does not define");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_a_sync_before_version_2_keeps_the_groups, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    struct mesh_tern_frame group = {
        .type = MESH_TERN_GROUP, .seq = 1U, .text_len = 3U, .text = "Hut"};
    memcpy(group.group, k_hut, sizeof k_hut);
    news(&protocol, &group);
    const struct mesh_tern_frame contact = record(MESH_TERN_CONTACT, 2U, 0U, 0U, 0U);
    news(&protocol, &contact);
    MESH_TEST_FAIL_IF(g_tern.group_count != 1U || g_tern.contact_count != 1U, "both are held");

    MESH_TEST_FAIL_IF(!reconnect_at(&protocol, &wire, 1U), "the node now speaks version 1");
    MESH_TEST_FAIL_IF(g_tern.contact_count != 0U,
                      "its sync sent no contact, so the node holds none");
    MESH_TEST_FAIL_IF(g_tern.group_count != 1U,
                      "but a version 1 sync sends no groups, and says nothing of them");
    MESH_TEST_FAIL_IF(!reconnect_at(&protocol, &wire, 2U), "and now version 2");
    MESH_TEST_FAIL_IF(g_tern.group_count != 0U, "whose sync is the whole list of groups");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_a_request_the_version_lacks_is_never_sent, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake_at(&protocol, &wire, 0U), "the handshake should reach READY");
    const struct mesh_tern_frame contact = record(MESH_TERN_CONTACT, 1U, 0U, 0U, 0U);
    news(&protocol, &contact);
    const size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_tern_end_session(&g_tern, mesh_tern_routing_id(k_bob)) != -EOPNOTSUPP ||
                          mesh_tern_make_group(&g_tern, "Hut") != -EOPNOTSUPP ||
                          mesh_tern_join(&g_tern, 4U) != -EOPNOTSUPP || wire.count != before,
                      "version 0 has no END_SESSION and no groups: refused here, not sent");

    /* Asked before INFO said the node's version, and found to be one it lacks. */
    struct mesh_protocol early = start(&wire);
    MESH_TEST_FAIL_IF(mesh_tern_make_group(&g_tern, "Hut") != 0, "queued behind the HELLO");
    const struct mesh_tern_frame info = {
        .type = MESH_TERN_INFO, .seq = wire_last(&wire)[1], .version = 1U};
    news(&early, &info);
    for (size_t i = 0; i < wire.count; ++i) {
        MESH_TEST_FAIL_IF(wire.frames[i][0] == MESH_TERN_MAKE_GROUP,
                          "a node of version 1 is never sent MAKE_GROUP");
    }
    MESH_TEST_FAIL_IF(wire_last(&wire)[0] != MESH_TERN_SET_TIME, "the conversation goes on");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_missed_news_reaches_back_to_what_may_change_of_all_three, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    struct mesh_tern_frame frame =
        record(MESH_TERN_MESSAGE, 1U, 3U, MESH_TERN_STATE_RECEIVED, MESH_TERN_MESSAGE_READ);
    news(&protocol, &frame);
    frame = record(MESH_TERN_GROUP_MESSAGE, 2U, 4U, MESH_TERN_STATE_SENT, 0U);
    news(&protocol, &frame);
    frame = record(MESH_TERN_INVITE, 3U, 5U, MESH_TERN_STATE_RECEIVED, 0U);
    news(&protocol, &frame);
    frame = record(MESH_TERN_GROUP_MESSAGE, 4U, 6U, MESH_TERN_STATE_WAITING, 0U);
    news(&protocol, &frame);
    frame = record(MESH_TERN_MESSAGE, 5U, 7U, MESH_TERN_STATE_DELIVERED, 0U);
    news(&protocol, &frame);
    MESH_TEST_FAIL_IF(g_model.messages.count != 2U || g_tern.group_item_count != 3U,
                      "messages to the log, group messages and invites beside it");
    const struct mesh_tern_frame power = {.type = MESH_TERN_POWER, .seq = 7U};
    news(&protocol, &power);
    MESH_TEST_FAIL_IF(last_sync_after(&wire) != 4U,
                      "after one less than the unread invite; a sent group message stays sent");

    /* The sync: the invite read since, on another client. */
    struct mesh_tern_frame self = {.type = MESH_TERN_SELF, .seq = 8U};
    memcpy(self.address, k_node, sizeof k_node);
    news(&protocol, &self);
    frame = record(MESH_TERN_INVITE, 9U, 5U, MESH_TERN_STATE_RECEIVED, MESH_TERN_MESSAGE_READ);
    news(&protocol, &frame);
    frame = record(MESH_TERN_GROUP_MESSAGE, 10U, 6U, MESH_TERN_STATE_WAITING, 0U);
    news(&protocol, &frame);
    synced(&protocol, &wire);
    MESH_TEST_FAIL_IF(!mesh_tern_ready(&g_tern), "synced");
    const struct mesh_tern_frame state = {
        .type = MESH_TERN_STATE, .seq = 12U, .id = 6U, .state = MESH_TERN_STATE_SENT};
    news(&protocol, &state); /* 11 is missed */
    MESH_TEST_FAIL_IF(last_sync_after(&wire) != 7U,
                      "the group message has gone; nothing held may change, so the newest");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_a_later_version_than_the_last_sync_asks_from_0_once, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake_at(&protocol, &wire, 2U), "synced at version 2");
    const struct mesh_tern_frame message =
        record(MESH_TERN_MESSAGE, 1U, 8U, MESH_TERN_STATE_DELIVERED, 0U);
    news(&protocol, &message);

    mesh_protocol_detach(&protocol);
    memset(&wire, 0, sizeof wire);
    mesh_protocol_attach(&protocol, wire_send, &wire);
    (void)mesh_protocol_begin(&protocol);
    const struct mesh_tern_frame info = {
        .type = MESH_TERN_INFO, .seq = wire_last(&wire)[1], .version = 3U};
    news(&protocol, &info);
    answer(&protocol, &wire, MESH_TERN_OK);
    MESH_TEST_FAIL_IF(last_sync_after(&wire) != 0U,
                      "the node may hold what version 2 was never sent, below id 8");
    struct mesh_tern_frame self = {.type = MESH_TERN_SELF, .seq = 0U};
    memcpy(self.address, k_node, sizeof k_node);
    news(&protocol, &self);
    synced(&protocol, &wire);

    MESH_TEST_FAIL_IF(!reconnect_at(&protocol, &wire, 3U), "and again at version 3");
    MESH_TEST_FAIL_IF(wire.count < 3U || wire.frames[2][0] != MESH_TERN_SYNC,
                      "HELLO, SET_TIME, SYNC");
    struct mesh_tern_frame sync;
    MESH_TEST_FAIL_IF(mesh_tern_decode(wire.frames[2], wire.lens[2], 3U, &sync) !=
                              MESH_TERN_DECODE_OK ||
                          sync.after != 8U,
                      "once: then after the newest held");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

/* The node's own fix (version 8): on the radio's roster entry while it has one, off it when the
   node says it has none, and a receiver state no version names is one that is on. */
MESH_TEST_CASE(tern_the_nodes_own_fix_is_the_radios_position, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    const uint32_t self_id = mesh_tern_routing_id(k_node);
    struct mesh_tern_frame fix = {.type = MESH_TERN_FIX,
                                  .seq = g_tern.news_expected,
                                  .receiver = MESH_TERN_RECEIVER_ON,
                                  .source = MESH_TERN_FIX_CLIENT,
                                  .lat = -336183000,
                                  .lon = -704517000,
                                  .altitude = MESH_TERN_NO_ALTITUDE,
                                  .age = 600U,
                                  .satellites = 4U};
    news(&protocol, &fix);
    const struct mesh_node_summary *self = model_node(self_id);
    MESH_TEST_FAIL_IF(
        self == NULL || !self->position.valid || self->position.latitude_i != -336183000 ||
            self->position.has_altitude || self->position.sats_in_view != 0U ||
            self->position.time != EXCHANGE_TIME - 600U || self->position.received != EXCHANGE_TIME,
        "a fix a client gave is the radio's position, with no satellites to it");
    fix.seq = g_tern.news_expected;
    fix.receiver = 7U; /* reserved */
    fix.source = MESH_TERN_FIX_NONE;
    news(&protocol, &fix);
    MESH_TEST_FAIL_IF(model_node(self_id)->position.valid || !g_tern.has_fix ||
                          g_tern.fix.receiver != MESH_TERN_RECEIVER_ON || g_tern.fix.lat != 0,
                      "no source is no position, and an unnamed receiver state is on");
    /* A version 7 sync says nothing of a fix: what the last connection said is not kept. */
    MESH_TEST_FAIL_IF(!reconnect_at(&protocol, &wire, 7U) || g_tern.has_fix,
                      "a node of version 7 has no receiver this client can switch");
    MESH_TEST_FAIL_IF(mesh_tern_set_receiver(&g_tern, false) != -EOPNOTSUPP,
                      "and SET 7 is refused here, not sent");
    MESH_TEST_FAIL_IF(
        !reconnect_at(&protocol, &wire, 8U) || mesh_tern_set_receiver(&g_tern, false) != 0 ||
            wire_last(&wire)[0] != MESH_TERN_SET ||
            wire_last(&wire)[2] != MESH_TERN_SETTING_RECEIVER || wire_last(&wire)[3] != 0U,
        "where version 8 sends SET 7 with 0");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

MESH_TEST_CASE(tern_off_is_version_9s_and_never_sent_below_it, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake_at(&protocol, &wire, 8U), "the handshake should reach READY");
    const size_t before = wire.count;
    MESH_TEST_FAIL_IF(mesh_tern_turn_off(&g_tern) != -EOPNOTSUPP || wire.count != before ||
                          g_tern.off != MESH_TERN_OFF_NONE,
                      "a node of version 8 is never sent OFF");
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    MESH_TEST_FAIL_IF(mesh_tern_turn_off(&g_tern) != 0 || wire_last(&wire)[0] != MESH_TERN_OFF ||
                          wire.lens[wire.count - 1U] != 2U,
                      "version 9's OFF is two bytes");
    MESH_TEST_FAIL_IF(mesh_tern_turn_off(&g_tern) != -EALREADY, "and asked once at a time");
    refuse(&protocol, &wire, MESH_TERN_ERR_NOT_NOW);
    uint8_t code = 0U;
    MESH_TEST_FAIL_IF(mesh_tern_take_off(&g_tern, &code) != MESH_TERN_OFF_REFUSED ||
                          code != MESH_TERN_ERR_NOT_NOW || !mesh_tern_ready(&g_tern) ||
                          mesh_tern_turned_off(&g_tern),
                      "a refusal is said, and the conversation goes on");

    /* Given up on: the node may be off. It is gone, and OFF may go again next time. */
    MESH_TEST_FAIL_IF(mesh_tern_turn_off(&g_tern) != 0, "OFF again");
    mesh_protocol_tick(&protocol, g_tern.awaiting_since_ms + MESH_TERN_ANSWER_WAIT_MS);
    MESH_TEST_FAIL_IF(!mesh_protocol_silent(&protocol) ||
                          mesh_tern_take_off(&g_tern, NULL) != MESH_TERN_OFF_UNCONFIRMED ||
                          mesh_tern_turned_off(&g_tern),
                      "unanswered: gone, unconfirmed, and not taken for off");
    MESH_TEST_FAIL_IF(!reconnect_at(&protocol, &wire, 9U) || mesh_tern_turn_off(&g_tern) != 0,
                      "one given up on may be sent again");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}

/* OK to OFF: the node turns off. Nothing behind the OFF goes, no PING follows, the drop is not
   silence, and the conversation says so until the next connection begins. */
MESH_TEST_CASE(tern_after_off_nothing_is_sent_and_nothing_reconnects, unit) {
    struct wire wire;
    struct mesh_protocol protocol;
    MESH_TEST_FAIL_IF(!handshake(&protocol, &wire), "the handshake should reach READY");
    uint32_t ticket = 0U;
    struct mesh_tern_frame contact = {
        .type = MESH_TERN_CONTACT, .seq = g_tern.news_expected, .text_len = 3U, .text = "Bob"};
    memcpy(contact.address, k_bob, sizeof k_bob);
    news(&protocol, &contact);
    MESH_TEST_FAIL_IF(
        mesh_tern_turn_off(&g_tern) != 0 ||
            mesh_tern_send_text(&g_tern, mesh_tern_routing_id(k_bob), "bye", &ticket) != 0,
        "OFF on the wire, a SEND queued behind it");
    const size_t sent = wire.count;
    answer(&protocol, &wire, MESH_TERN_OK);
    struct mesh_tern_queued queued;
    MESH_TEST_FAIL_IF(wire.count != sent || g_tern.queue_count != 0U ||
                          !mesh_tern_take_queued(&g_tern, &queued) || queued.ticket != ticket ||
                          queued.id != 0U,
                      "the SEND never goes, and ends as refused");
    MESH_TEST_FAIL_IF(mesh_tern_send_text(&g_tern, mesh_tern_routing_id(k_bob), "hi", NULL) !=
                              -ENOTCONN ||
                          mesh_tern_mark_read(&g_tern, 1U) != -ENOTCONN ||
                          mesh_tern_turn_off(&g_tern) != -ENOTCONN,
                      "nor anything asked after");
    mesh_protocol_tick(&protocol, g_tern.answered_ms + 10U * MESH_TERN_IDLE_MS);
    MESH_TEST_FAIL_IF(wire.count != sent || mesh_protocol_silent(&protocol),
                      "no PING, and the quiet that follows is not a node gone");
    /* The node drops the link as it turns off. */
    mesh_protocol_detach(&protocol);
    MESH_TEST_FAIL_IF(!mesh_tern_turned_off(&g_tern) ||
                          mesh_tern_take_off(&g_tern, NULL) != MESH_TERN_OFF_DONE,
                      "after the drop it is still a node turned off, which nothing reopens");
    MESH_TEST_FAIL_IF(!reconnect_at(&protocol, &wire, 9U) || mesh_tern_turned_off(&g_tern),
                      "until the user connects again, to a node turned on by hand");
    inkwell_time_wall_set_fixed(0U);
    record_success(test_name);
}
