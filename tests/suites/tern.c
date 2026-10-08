/*
 * Tern's conversation: the client half of draft/companion.md, driven through its struct
 * mesh_protocol as a link would drive it, with what it learns read back off the session model.
 * The first case replays the specification's own exchange (tests/data/tern_companion.json); the
 * rest are the rules around it - a lapsed connection, missed news, silence and the ping.
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

static void news(const struct mesh_protocol *protocol, const struct mesh_tern_frame *frame) {
    uint8_t out[MESH_TERN_MAX_FRAME];
    const int len = mesh_tern_encode(frame, out, sizeof out);
    if (len > 0) {
        feed(protocol, out, (size_t)len);
    }
}

static const struct mesh_node_summary *model_node(uint32_t id) {
    for (size_t i = 0; i < g_model.handshake.node_count; ++i) {
        if (g_model.handshake.nodes[i].node_id == id) {
            return &g_model.handshake.nodes[i];
        }
    }
    return NULL;
}

/* HELLO, INFO, SET_TIME, OK, SYNC, an empty sync: READY, news counted from 0 again. */
static bool handshake(struct mesh_protocol *protocol, struct wire *wire) {
    *protocol = start(wire);
    if (wire->count != 1U || wire_last(wire)[0] != MESH_TERN_HELLO) {
        return false;
    }
    struct mesh_tern_frame info = {.type = MESH_TERN_INFO, .seq = wire_last(wire)[1]};
    news(protocol, &info);
    answer(protocol, wire, MESH_TERN_OK); /* SET_TIME */
    if (wire_last(wire)[0] != MESH_TERN_SYNC) {
        return false;
    }
    struct mesh_tern_frame self = {.type = MESH_TERN_SELF, .seq = 0U};
    memcpy(self.address, k_node, sizeof k_node);
    news(protocol, &self);
    answer(protocol, wire, MESH_TERN_SYNCED);
    return mesh_tern_ready(&g_tern);
}

/* The vectors' exchange, frame by frame: what the client sends is what the exchange says it
   sends, but for SEND's `ref`, which a client chooses at random. */
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
    size_t sent = 0U; /* client frames matched so far */
    size_t steps = 0U;
    char failure[160] = "";
    while (failure[0] == '\0' && inkwell_json_next_element(&json)) {
        char from[16] = "";
        char type[32] = "";
        char hex[512] = "";
        char key[16];
        bool ok = inkwell_json_enter_object(&json);
        while (ok && inkwell_json_next_key(&json, key, sizeof key)) {
            if (strcmp(key, "from") == 0) {
                ok = inkwell_json_read_string(&json, from, sizeof from);
            } else if (strcmp(key, "type") == 0) {
                ok = inkwell_json_read_string(&json, type, sizeof type);
            } else if (strcmp(key, "frame") == 0) {
                ok = inkwell_json_read_string(&json, hex, sizeof hex);
            } else {
                ok = inkwell_json_skip_value(&json);
            }
        }
        uint8_t frame[MESH_TERN_MAX_FRAME];
        size_t len = 0U;
        for (const char *p = hex; p[0] != '\0' && p[1] != '\0' && len < sizeof frame; p += 2) {
            const char pair[3] = {p[0], p[1], '\0'};
            frame[len++] = (uint8_t)strtoul(pair, NULL, 16);
        }
        if (!ok) {
            snprintf(failure, sizeof failure, "exchange step %zu should read", steps);
        } else if (strcmp(from, "node") == 0) {
            feed(&protocol, frame, len);
        } else {
            /* What the user does, the exchange shows the client doing. */
            if (strcmp(type, "READ") == 0) {
                (void)mesh_tern_mark_read(&g_tern, 17U);
            } else if (strcmp(type, "SEND") == 0) {
                (void)mesh_tern_send_text(&g_tern, mesh_tern_routing_id(k_bob),
                                          "On the ridge by six", NULL);
            }
            const bool same_ref = strcmp(type, "SEND") == 0;
            if (wire.count != sent + 1U || wire.lens[sent] != len ||
                memcmp(wire.frames[sent], frame, 2U) != 0 ||
                memcmp(wire.frames[sent] + (same_ref ? 6U : 2U), frame + (same_ref ? 6U : 2U),
                       len - (same_ref ? 6U : 2U)) != 0) {
                snprintf(failure, sizeof failure, "exchange step %zu: the client's %s differs",
                         steps, type);
            }
            sent += 1U;
        }
        steps += 1U;
    }
    free(document);
    MESH_TEST_FAIL_IF(failure[0] != '\0', failure);
    MESH_TEST_FAIL_IF(steps != 20U, "all twenty steps");

    /* And what it learned. */
    const uint32_t self_id = mesh_tern_routing_id(k_node);
    const uint32_t bob_id = mesh_tern_routing_id(k_bob);
    MESH_TEST_FAIL_IF(!mesh_tern_ready(&g_tern) || g_model.handshake.my_info.my_node_num != self_id,
                      "the node is the radio, known by its routing id");
    const struct mesh_node_summary *bob = model_node(bob_id);
    MESH_TEST_FAIL_IF(bob == NULL || strcmp(bob->long_name, "Bob") != 0 || !bob->in_nodedb ||
                          bob->public_key_len != 32U || memcmp(bob->public_key, k_bob, 32U) != 0,
                      "Bob is a contact, by name, with his address");
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

    struct mesh_message *received = mesh_message_log_find(&g_model.messages, 17U);
    MESH_TEST_FAIL_IF(received == NULL || received->direction != MESH_MESSAGE_INBOUND ||
                          received->from != bob_id ||
                          strcmp(received->text, "Where are you?") != 0 || !received->replayed ||
                          received->rx_time != 1789999000U,
                      "Bob's message, under the node's id, handed back by the sync");
    size_t copies = 0U;
    for (size_t i = 0; i < g_model.messages.count; ++i) {
        copies += mesh_message_log_at(&g_model.messages, i)->packet_id == 17U ? 1U : 0U;
    }
    MESH_TEST_FAIL_IF(copies != 1U, "marked read, the record lands on the entry it already has");

    struct mesh_tern_queued queued;
    MESH_TEST_FAIL_IF(!mesh_tern_take_queued(&g_tern, &queued) || queued.id != 18U ||
                          queued.ticket != 1U || mesh_tern_take_queued(&g_tern, &queued),
                      "QUEUED's id, once, under the send's ticket");
    const struct mesh_message *mine = mesh_message_log_find(&g_model.messages, 18U);
    MESH_TEST_FAIL_IF(mine == NULL || mine->direction != MESH_MESSAGE_OUTBOUND ||
                          mine->to != bob_id || mine->ack != MESH_MESSAGE_ACK_DELIVERED ||
                          mine->replayed,
                      "our message, waiting, then sent, then delivered");
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
    const struct mesh_message *logged = mesh_message_log_find(&g_model.messages, 5U);
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
    struct mesh_tern_frame sync;
    MESH_TEST_FAIL_IF(mesh_tern_decode(wire_last(&wire), wire.lens[wire.count - 1U], &sync) !=
                              MESH_TERN_DECODE_OK ||
                          sync.after != 6U,
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
    answer(&protocol, &wire, MESH_TERN_SYNCED);
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
                          taken.id != 40U,
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
