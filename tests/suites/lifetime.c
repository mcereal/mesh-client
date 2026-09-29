#define _POSIX_C_SOURCE 200809L

/*
 * The lifetime stats: numbers that only go up, across every run and every radio.
 *
 * The cases are about the two promises a tally has to keep - it counts a thing once, and it
 * still has the count after a restart - and about the lines drawn around what counts at all:
 * our own radios are not nodes we heard, and a path over MQTT says nothing about our reach.
 */

#include "inkwell/base/time.h"

#include "framework/mesh_test.h"
#include "support/fs_fixture.h"
#include "support/session_fixture.h"

#include "mesh/core/lifetime.h"
#include "mesh/core/message.h"
#include "mesh/core/session.h"

#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LT_US 0x1111U
#define LT_PEER 0x2222U
#define LT_OTHER 0x3333U

/* The lifetime struct carries the node set, so it is too large for a test's stack. */
static struct mesh_lifetime g_lifetime;

static bool lt_open(char *dir, size_t dir_len) {
    snprintf(dir, dir_len, "/tmp/mesh_lifetime_XXXXXX");
    if (mkdtemp(dir) == NULL) {
        return false;
    }
    return mesh_lifetime_init(&g_lifetime, dir) == 0;
}

static struct mesh_session g_session;

/* A session with our node at `us_lat`, so a distance can be measured from it. */
static void lt_session(int32_t us_lat_i, int32_t us_lon_i) {
    mesh_session_init(&g_session);
    g_session.handshake.has_my_info = true;
    g_session.handshake.my_info.my_node_num = LT_US;
    g_session.handshake.node_count = 1U;
    struct mesh_node_summary *self = &g_session.handshake.nodes[0];
    memset(self, 0, sizeof *self);
    self->node_id = LT_US;
    self->position.valid = us_lat_i != 0 || us_lon_i != 0;
    self->position.latitude_i = us_lat_i;
    self->position.longitude_i = us_lon_i;
}

static void lt_message(uint32_t from, uint32_t to, enum mesh_message_direction direction,
                       bool reaction) {
    struct mesh_message message;
    memset(&message, 0, sizeof message);
    message.from = from;
    message.to = to;
    message.direction = (uint8_t)direction;
    message.is_reaction = reaction;
    const struct mesh_session_event event = {.kind = MESH_SESSION_EVENT_MESSAGE,
                                             .message = &message};
    mesh_lifetime_observe(&g_lifetime, &g_session, &event);
}

static void lt_node(enum mesh_session_event_kind kind, const struct mesh_node_summary *node,
                    bool via_mqtt, bool has_hops, uint8_t hops) {
    const struct mesh_session_event event = {
        .kind = kind, .node = node, .via_mqtt = via_mqtt, .has_hops = has_hops, .hops = hops};
    mesh_lifetime_observe(&g_lifetime, &g_session, &event);
}

/* A received message, and how it arrived and was encrypted. */
static void lt_received(uint32_t to, bool via_mqtt, bool pki) {
    struct mesh_message message;
    memset(&message, 0, sizeof message);
    message.from = LT_PEER;
    message.to = to;
    message.direction = (uint8_t)MESH_MESSAGE_INBOUND;
    message.pki_encrypted = pki;
    const struct mesh_session_event event = {
        .kind = MESH_SESSION_EVENT_MESSAGE, .message = &message, .via_mqtt = via_mqtt};
    mesh_lifetime_observe(&g_lifetime, &g_session, &event);
}

/* A node heard over the air at `snr` dB. */
static void lt_heard_snr(const struct mesh_node_summary *node, bool via_mqtt, bool has_hops,
                         uint8_t hops, float snr) {
    const struct mesh_session_event event = {.kind = MESH_SESSION_EVENT_NODE_HEARD,
                                             .node = node,
                                             .via_mqtt = via_mqtt,
                                             .has_hops = has_hops,
                                             .hops = hops,
                                             .has_snr = true,
                                             .snr = snr};
    mesh_lifetime_observe(&g_lifetime, &g_session, &event);
}

static struct mesh_node_summary lt_summary(uint32_t id) {
    struct mesh_node_summary node;
    memset(&node, 0, sizeof node);
    node.node_id = id;
    return node;
}

/* How many lines of the seen file carry `key`. */
static unsigned lt_seen_lines(const char *dir, const char *key) {
    char path[512];
    snprintf(path, sizeof path, "%s/seen.stats", dir);
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return 0U;
    }
    char line[128];
    unsigned count = 0U;
    const size_t key_len = strlen(key);
    while (fgets(line, sizeof line, file) != NULL) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            count++;
        }
    }
    fclose(file);
    return count;
}

MESH_TEST_CASE(lifetime_counts_messages_by_direction_and_kind, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    lt_session(0, 0);

    lt_message(LT_PEER, MESH_MESSAGE_BROADCAST_ADDR, MESH_MESSAGE_INBOUND, false);
    lt_message(LT_PEER, LT_US, MESH_MESSAGE_INBOUND, false);
    lt_message(LT_US, LT_PEER, MESH_MESSAGE_OUTBOUND, false);
    lt_message(LT_US, MESH_MESSAGE_BROADCAST_ADDR, MESH_MESSAGE_OUTBOUND, false);
    lt_message(LT_US, LT_PEER, MESH_MESSAGE_OUTBOUND, true);
    lt_message(LT_PEER, LT_US, MESH_MESSAGE_INBOUND, true);

    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MESSAGES_RECEIVED) != 2U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MESSAGES_SENT) != 2U,
                      "two each way, reactions aside");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_DIRECT_RECEIVED) != 1U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_DIRECT_SENT) != 1U,
                      "one of each was direct");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_REACTIONS_SENT) != 1U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_REACTIONS_RECEIVED) != 1U,
                      "a reaction is its own count");
    MESH_TEST_FAIL_IF(!mesh_lifetime_dirty(&g_lifetime), "and the totals want writing");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/* One of our messages settling, as the session says it: the record is the one in the log, which
   is where a message has to be for anything to mark it. Appended the first time it is named. */
static void lt_settle_to(uint32_t packet_id, uint32_t to, enum mesh_message_ack previous,
                         enum mesh_message_ack now) {
    struct mesh_message *message = mesh_message_log_find(&g_session.messages, packet_id);
    if (message == NULL) {
        struct mesh_message record;
        memset(&record, 0, sizeof record);
        record.packet_id = packet_id;
        record.from = LT_US;
        record.to = to;
        record.direction = MESH_MESSAGE_OUTBOUND;
        message = mesh_message_log_append(&g_session.messages, &record);
    }
    message->ack = (uint8_t)now;
    const struct mesh_session_event event = {
        .kind = MESH_SESSION_EVENT_DELIVERY, .message = message, .previous_ack = (uint8_t)previous};
    mesh_lifetime_observe(&g_lifetime, &g_session, &event);
}

static void lt_settle(uint32_t packet_id, enum mesh_message_ack previous,
                      enum mesh_message_ack now) {
    lt_settle_to(packet_id, LT_PEER, previous, now);
}

static bool lt_deliveries(uint64_t delivered, uint64_t failed) {
    return mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MESSAGES_DELIVERED) == delivered &&
           mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MESSAGES_FAILED) == failed;
}

/*
 * Each direct message that asked to be confirmed is in the count its bubble says.
 *
 * Counted when it leaves pending, then moved between the counts rather than added again - a
 * message whose answer changes, however often, is still one message. Nothing that never asked
 * is counted: a reaction, or a broadcast (MeshCore's pending on one is its place in the radio's
 * queue). And only a message this run counted is moved, since only for those is it known which
 * count holds it.
 */
MESH_TEST_CASE(lifetime_counts_each_delivery_once, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    lt_session(0, 0);

    lt_settle(1U, MESH_MESSAGE_ACK_PENDING, MESH_MESSAGE_ACK_DELIVERED);
    lt_settle(2U, MESH_MESSAGE_ACK_PENDING, MESH_MESSAGE_ACK_FAILED);
    lt_settle(3U, MESH_MESSAGE_ACK_PENDING, MESH_MESSAGE_ACK_FAILED);
    MESH_TEST_FAIL_IF(!lt_deliveries(1U, 2U), "one delivered and two failed");

    lt_settle(4U, MESH_MESSAGE_ACK_PENDING, MESH_MESSAGE_ACK_NONE);
    lt_settle_to(5U, MESH_MESSAGE_BROADCAST_ADDR, MESH_MESSAGE_ACK_PENDING,
                 MESH_MESSAGE_ACK_FAILED);
    lt_settle_to(6U, MESH_MESSAGE_BROADCAST_ADDR, MESH_MESSAGE_ACK_PENDING, MESH_MESSAGE_ACK_NONE);
    /* A reaction asks for nothing, so failing to send one is not a message undelivered. */
    lt_settle(7U, MESH_MESSAGE_ACK_NONE, MESH_MESSAGE_ACK_FAILED);
    lt_settle(7U, MESH_MESSAGE_ACK_FAILED, MESH_MESSAGE_ACK_DELIVERED);
    MESH_TEST_FAIL_IF(!lt_deliveries(1U, 2U), "a message nothing answers is in neither count");

    /* Delivered, refused after, then answered again: one message, delivered. */
    lt_settle(1U, MESH_MESSAGE_ACK_DELIVERED, MESH_MESSAGE_ACK_FAILED);
    MESH_TEST_FAIL_IF(!lt_deliveries(0U, 3U), "a delivered message refused after moves across");
    lt_settle(1U, MESH_MESSAGE_ACK_FAILED, MESH_MESSAGE_ACK_DELIVERED);
    MESH_TEST_FAIL_IF(!lt_deliveries(1U, 2U), "and back, taking only its own failure with it");
    lt_settle(2U, MESH_MESSAGE_ACK_FAILED, MESH_MESSAGE_ACK_DELIVERED);
    MESH_TEST_FAIL_IF(!lt_deliveries(2U, 1U), "a late answer moves a failure to a delivery");

    MESH_TEST_FAIL_IF(mesh_lifetime_flush(&g_lifetime) != 0, "the flush failed");
    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0, "the stats did not reopen");
    MESH_TEST_FAIL_IF(!lt_deliveries(2U, 1U), "and both survive a restart");

    /* A message counted before a reset or a restart stays where it was left: which count holds
       it is not known, and moving it would take one that is some other message's. */
    lt_settle(3U, MESH_MESSAGE_ACK_FAILED, MESH_MESSAGE_ACK_DELIVERED);
    MESH_TEST_FAIL_IF(!lt_deliveries(2U, 1U), "a message the last run counted is not moved");
    MESH_TEST_FAIL_IF(mesh_lifetime_reset(&g_lifetime) != 0, "the reset failed");
    lt_settle(8U, MESH_MESSAGE_ACK_PENDING, MESH_MESSAGE_ACK_DELIVERED);
    lt_settle(2U, MESH_MESSAGE_ACK_DELIVERED, MESH_MESSAGE_ACK_FAILED);
    lt_settle(3U, MESH_MESSAGE_ACK_FAILED, MESH_MESSAGE_ACK_DELIVERED);
    MESH_TEST_FAIL_IF(!lt_deliveries(1U, 0U), "nor is one counted before a reset");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/*
 * A message still in the session's log keeps its place however long ago it was counted.
 *
 * The log drops messages in the order they were sent and the stats count them in the order they
 * were answered, which are not the same order - so the place a new count takes is one whose
 * message has left the log, never the oldest counted. Here the log's oldest message is answered
 * last, after every other has been counted, and the first one counted can still change.
 */
MESH_TEST_CASE(lifetime_a_delivery_in_the_log_keeps_its_place, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    lt_session(0, 0);

    /* The oldest message goes out first and waits; the rest fill the log and are answered. */
    const uint32_t oldest = 1000U;
    lt_settle(oldest, MESH_MESSAGE_ACK_NONE, MESH_MESSAGE_ACK_PENDING);
    for (uint32_t i = 1U; i < MESH_MESSAGE_LOG_CAPACITY; ++i) {
        lt_settle(oldest + i, MESH_MESSAGE_ACK_PENDING, MESH_MESSAGE_ACK_DELIVERED);
    }
    lt_settle(oldest, MESH_MESSAGE_ACK_PENDING, MESH_MESSAGE_ACK_DELIVERED);
    MESH_TEST_FAIL_IF(!lt_deliveries(MESH_MESSAGE_LOG_CAPACITY, 0U), "every message delivered");

    /* One more message pushes the oldest out of the log, and takes its place. */
    lt_settle(oldest + MESH_MESSAGE_LOG_CAPACITY, MESH_MESSAGE_ACK_PENDING,
              MESH_MESSAGE_ACK_DELIVERED);
    lt_settle(oldest + 1U, MESH_MESSAGE_ACK_DELIVERED, MESH_MESSAGE_ACK_FAILED);
    MESH_TEST_FAIL_IF(!lt_deliveries(MESH_MESSAGE_LOG_CAPACITY, 1U),
                      "a message still in the log moves when its answer changes");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

MESH_TEST_CASE(lifetime_survives_a_restart, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    lt_session(0, 0);
    lt_message(LT_PEER, MESH_MESSAGE_BROADCAST_ADDR, MESH_MESSAGE_INBOUND, false);
    lt_message(LT_US, LT_PEER, MESH_MESSAGE_OUTBOUND, false);
    struct mesh_node_summary peer = lt_summary(LT_PEER);
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, false, true, 3U);
    mesh_lifetime_note_radio(&g_lifetime, LT_US);
    MESH_TEST_FAIL_IF(mesh_lifetime_flush(&g_lifetime) != 0 || mesh_lifetime_dirty(&g_lifetime),
                      "the totals were written");

    /* The next run, from the card alone. */
    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0, "the stats did not reopen");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MESSAGES_RECEIVED) != 1U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MESSAGES_SENT) != 1U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_DIRECT_SENT) != 1U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MOST_HOPS) != 3U,
                      "the counts and the record come back");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD) != 1U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_RADIOS) != 1U,
                      "and so does the set");

    /* A node written before the flush is on the card even though nothing flushed it: the seen
       file is appended as it learns, so a SIGKILL cannot take a node back. */
    struct mesh_node_summary other = lt_summary(LT_OTHER);
    lt_node(MESH_SESSION_EVENT_NODE_LISTED, &other, false, false, 0U);
    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0, "the stats did not reopen");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD) != 2U,
                      "a node reaches the card without a flush");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

MESH_TEST_CASE(lifetime_a_node_is_one_node_however_often_heard, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    lt_session(0, 0);
    struct mesh_node_summary peer = lt_summary(LT_PEER);
    for (int i = 0; i < 3; ++i) {
        lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, false, false, 0U);
    }
    lt_node(MESH_SESSION_EVENT_NODE_LISTED, &peer, false, false, 0U);

    /* A node only ever bridged in is heard, but not by us. */
    struct mesh_node_summary bridged = lt_summary(LT_OTHER);
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &bridged, true, false, 0U);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD) != 2U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD_RF) != 1U,
                      "two nodes, one of them over the air");
    MESH_TEST_FAIL_IF(lt_seen_lines(dir, "heard") != 2U || lt_seen_lines(dir, "rf") != 1U,
                      "and one line per thing learned, however often it was heard");

    /* Then it is heard over the air too, which is one more fact and one more line. */
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &bridged, false, false, 0U);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD_RF) != 2U ||
                          lt_seen_lines(dir, "rf") != 2U || lt_seen_lines(dir, "heard") != 2U,
                      "a node heard over the air later is counted there once");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

MESH_TEST_CASE(lifetime_our_radios_are_not_nodes_we_heard, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    lt_session(0, 0);
    /* A second radio of ours, heard over the air before it was ever plugged in. */
    struct mesh_node_summary spare = lt_summary(LT_OTHER);
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &spare, false, false, 0U);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD) != 1U,
                      "until it is attached it is a node like any other");
    mesh_lifetime_note_radio(&g_lifetime, LT_US);
    mesh_lifetime_note_radio(&g_lifetime, LT_OTHER);
    mesh_lifetime_note_radio(&g_lifetime, LT_OTHER);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_RADIOS) != 2U,
                      "two radios, however often attached");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD) != 0U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD_RF) != 0U,
                      "and once it is, it is not counted as a node heard");
    MESH_TEST_FAIL_IF(lt_seen_lines(dir, "radio") != 2U, "each radio is written down once");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/*
 * The records are about this radio's reach: a hop count or a distance counts only from a packet
 * we watched arrive over the air. A listing's hop count may be days old, and a bridged packet's
 * path is somebody else's.
 */
MESH_TEST_CASE(lifetime_records_take_only_what_arrived_over_the_air, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    /* Us at 51.5, 0; the peer 0.1 degrees of latitude north, about 11.1 km. */
    lt_session(515000000, 0);
    struct mesh_node_summary peer = lt_summary(LT_PEER);
    peer.position.valid = true;
    peer.position.latitude_i = 516000000;
    peer.position.longitude_i = 0;

    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, true, true, 7U);
    lt_node(MESH_SESSION_EVENT_NODE_LISTED, &peer, false, true, 6U);
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, false, false, 0U);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MOST_HOPS) != 0U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M) != 0U,
                      "MQTT, a listing and a packet with no hop count set no record");

    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, false, true, 2U);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MOST_HOPS) != 2U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M) != 0U,
                      "two hops out is a hop record and no distance record");

    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, false, true, 0U);
    const uint64_t farthest = mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M);
    MESH_TEST_FAIL_IF(farthest < 11000U || farthest > 11250U,
                      "heard direct at about 11.1 km sets the distance");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MOST_HOPS) != 2U,
                      "and a record is never lowered");

    /* With no fix of our own there is nothing to measure from. */
    peer.position.latitude_i = 530000000;
    g_session.handshake.nodes[0].position.valid = false;
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, false, true, 0U);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M) != farthest,
                      "no fix of ours is no distance");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/*
 * A record set to 0 is still a record, and says so across a restart.
 *
 * Both records have a real 0: a node only ever heard straight to us is a most-hops of 0, and two
 * radios at one spot are a distance of 0. The value cannot tell those from "nothing measured
 * yet", so the stats carry which records have been set and write it down beside them.
 */
MESH_TEST_CASE(lifetime_a_record_of_zero_is_a_measurement, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    lt_session(515000000, 0);
    MESH_TEST_FAIL_IF(mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_MOST_HOPS) ||
                          mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M),
                      "nothing heard is nothing measured");
    MESH_TEST_FAIL_IF(!mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_MESSAGES_SENT),
                      "a count's zero is a count of none, so it is always measured");

    /* Heard direct, standing exactly where we are. */
    struct mesh_node_summary peer = lt_summary(LT_PEER);
    peer.position.valid = true;
    peer.position.latitude_i = 515000000;
    peer.position.longitude_i = 0;
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, false, true, 0U);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MOST_HOPS) != 0U ||
                          !mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_MOST_HOPS),
                      "zero hops heard is a most-hops of zero, measured");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M) != 0U ||
                          !mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M),
                      "zero metres heard direct is a distance of zero, measured");
    MESH_TEST_FAIL_IF(!mesh_lifetime_dirty(&g_lifetime), "a first measurement wants writing");

    /* The peer then turns out to be one of our radios, which takes it out of the node counts -
       and must not take the record with it. */
    mesh_lifetime_note_radio(&g_lifetime, LT_PEER);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD_RF) != 0U,
                      "our own radio is not a node we heard");
    MESH_TEST_FAIL_IF(!mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_MOST_HOPS),
                      "a record outlives the count of the node that set it");

    MESH_TEST_FAIL_IF(mesh_lifetime_flush(&g_lifetime) != 0, "the flush failed");
    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0, "the stats did not reopen");
    MESH_TEST_FAIL_IF(!mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_MOST_HOPS) ||
                          !mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M),
                      "a record of zero is still measured after a restart");

    MESH_TEST_FAIL_IF(mesh_lifetime_reset(&g_lifetime) != 0, "the reset failed");
    MESH_TEST_FAIL_IF(mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_MOST_HOPS),
                      "a reset leaves nothing measured");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/* A card from before the marker: a record above 0 was measured, and a 0 is read as not yet. */
MESH_TEST_CASE(lifetime_a_record_from_an_older_card_reads_as_measured, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    char path[128];
    snprintf(path, sizeof path, "%s/totals.stats", dir);
    FILE *file = fopen(path, "w");
    MESH_TEST_FAIL_IF(file == NULL, "could not write the totals");
    fputs("most_hops=4\nfarthest_direct_m=0\n", file);
    fclose(file);

    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0, "the stats did not reopen");
    MESH_TEST_FAIL_IF(!mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_MOST_HOPS),
                      "a record above zero on an older card was measured");
    MESH_TEST_FAIL_IF(mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M),
                      "a zero on an older card is the none-yet it most likely was");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/*
 * A record names who set it and when, and keeps the first to reach it.
 *
 * A tie is not a new record: a record that changed hands whenever somebody matched it would name
 * whoever was heard last. The holder goes on the card beside the record and comes back with it.
 */
MESH_TEST_CASE(lifetime_a_record_names_its_holder, unit) {
    char dir[64];
    inkwell_time_wall_set_fixed(1750000000U);
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    lt_session(0, 0);
    uint32_t node = 0U;
    uint32_t at = 0U;
    MESH_TEST_FAIL_IF(mesh_lifetime_holder(&g_lifetime, MESH_LIFETIME_MOST_HOPS, &node, &at),
                      "nothing measured has nobody to name");
    MESH_TEST_FAIL_IF(mesh_lifetime_holder(&g_lifetime, MESH_LIFETIME_MESSAGES_SENT, &node, &at),
                      "a count has no holder");

    const struct mesh_node_summary peer = lt_summary(LT_PEER);
    const struct mesh_node_summary other = lt_summary(LT_OTHER);
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, false, true, 3U);
    inkwell_time_wall_set_fixed(1760000000U);
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &other, false, true, 3U);
    MESH_TEST_FAIL_IF(!mesh_lifetime_holder(&g_lifetime, MESH_LIFETIME_MOST_HOPS, &node, &at) ||
                          node != LT_PEER || at != 1750000000U,
                      "a tie keeps the node that got there first, and its day");

    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &other, false, true, 5U);
    MESH_TEST_FAIL_IF(!mesh_lifetime_holder(&g_lifetime, MESH_LIFETIME_MOST_HOPS, &node, &at) ||
                          node != LT_OTHER || at != 1760000000U,
                      "a new record changes hands");

    MESH_TEST_FAIL_IF(mesh_lifetime_flush(&g_lifetime) != 0, "the flush failed");
    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0, "the stats did not reopen");
    node = 0U;
    at = 0U;
    MESH_TEST_FAIL_IF(!mesh_lifetime_holder(&g_lifetime, MESH_LIFETIME_MOST_HOPS, &node, &at) ||
                          node != LT_OTHER || at != 1760000000U,
                      "the holder comes back from the card");

    MESH_TEST_FAIL_IF(mesh_lifetime_reset(&g_lifetime) != 0, "the reset failed");
    MESH_TEST_FAIL_IF(mesh_lifetime_holder(&g_lifetime, MESH_LIFETIME_MOST_HOPS, &node, &at),
                      "a reset leaves nobody holding anything");
    inkwell_time_wall_set_fixed(0U);
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/*
 * A holder line is believed only while it names the record the card holds.
 *
 * A build that does not know the holder carries its line through untouched while raising the
 * record itself, so the line can outlive the record it was about. Crediting the new record to
 * the old holder would be a wrong name; no name is merely a missing one.
 */
MESH_TEST_CASE(lifetime_a_holder_of_an_older_record_is_dropped, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    char path[128];
    snprintf(path, sizeof path, "%s/totals.stats", dir);
    FILE *file = fopen(path, "w");
    MESH_TEST_FAIL_IF(file == NULL, "could not write the totals");
    /* The holder line first, so it is read before the value it is checked against. */
    fputs("farthest_direct_m.holder=00002222\nfarthest_direct_m.held_value=900\n"
          "farthest_direct_m.held_at=1750000000\n"
          "most_hops.holder=00003333\nmost_hops.held_value=4\nmost_hops.held_at=1750000000\n"
          "most_hops=6\nmost_hops.measured=1\n"
          "farthest_direct_m=900\nfarthest_direct_m.measured=1\n",
          file);
    fclose(file);

    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0, "the stats did not reopen");
    uint32_t node = 0U;
    uint32_t at = 0U;
    MESH_TEST_FAIL_IF(mesh_lifetime_holder(&g_lifetime, MESH_LIFETIME_MOST_HOPS, &node, &at),
                      "a holder of 4 hops does not hold a record of 6");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MOST_HOPS) != 6U ||
                          !mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_MOST_HOPS),
                      "and the record itself stands");
    MESH_TEST_FAIL_IF(
        !mesh_lifetime_holder(&g_lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M, &node, &at) ||
            node != LT_PEER || at != 1750000000U,
        "a holder of the record it names is believed");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/*
 * Every holder line fits what an older build keeps of a key it does not know.
 *
 * Such a build carries a foreign key through a rewrite only while it and its value are shorter
 * than MESH_LIFETIME_FOREIGN_KEY and _VALUE, and drops it silently otherwise - so a card moved
 * back to it and forward again would lose the holder of a record that never changed. Measured
 * on the widest a line gets: a distance record past the longest a packet could travel, set with a
 * clock.
 */
MESH_TEST_CASE(lifetime_holder_lines_survive_an_older_build, unit) {
    char dir[64];
    inkwell_time_wall_set_fixed(4000000000U);
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    /* Us at the south pole, the peer at the north: about 20,000 km, eight digits of metres. */
    lt_session(-900000000, 0);
    struct mesh_node_summary peer = lt_summary(0xFFFFFFFEU);
    peer.position.valid = true;
    peer.position.latitude_i = 900000000;
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, false, true, 0U);
    inkwell_time_wall_set_fixed(0U);
    MESH_TEST_FAIL_IF(
        !mesh_lifetime_holder(&g_lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M, NULL, NULL),
        "the record has a holder to write");
    MESH_TEST_FAIL_IF(mesh_lifetime_flush(&g_lifetime) != 0, "the flush failed");

    char path[128];
    snprintf(path, sizeof path, "%s/totals.stats", dir);
    FILE *file = fopen(path, "r");
    MESH_TEST_FAIL_IF(file == NULL, "the totals are gone");
    char line[128];
    unsigned holder_lines = 0U;
    bool fits = true;
    while (fgets(line, sizeof line, file) != NULL) {
        char *equals = strchr(line, '=');
        if (equals == NULL || (strstr(line, ".held") == NULL && strstr(line, ".holder") == NULL)) {
            continue;
        }
        holder_lines++;
        const size_t key_len = (size_t)(equals - line);
        const size_t value_len = strcspn(equals + 1, "\n");
        fits =
            fits && key_len < MESH_LIFETIME_FOREIGN_KEY && value_len < MESH_LIFETIME_FOREIGN_VALUE;
    }
    fclose(file);
    /* Heard straight to us, so the one hearing set all three distance and hop records: three
       lines each. */
    MESH_TEST_FAIL_IF(holder_lines != 9U, "a record's holder is three lines");
    MESH_TEST_FAIL_IF(!fits, "every holder line should fit an older build's foreign key");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/*
 * Of the messages received, the ones that came over MQTT and the direct ones encrypted with our
 * key each have a count of their own. Only received: a send is on the log before the radio has
 * decided how it goes.
 */
MESH_TEST_CASE(lifetime_counts_mqtt_and_private_messages, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    lt_session(0, 0);

    lt_received(MESH_MESSAGE_BROADCAST_ADDR, true, false);
    lt_received(LT_US, true, true);
    lt_received(LT_US, false, true);
    lt_received(LT_US, false, false);
    /* A broadcast is never private, whatever the flag says. */
    lt_received(MESH_MESSAGE_BROADCAST_ADDR, false, true);

    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MESSAGES_RECEIVED) != 5U,
                      "every one of them is a message received");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_RECEIVED_MQTT) != 2U,
                      "two came over MQTT");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_DIRECT_RECEIVED_PRIVATE) != 2U,
                      "two were direct and encrypted with our key");

    /* A reaction is not a message, and a send is not received. */
    struct mesh_message reaction;
    memset(&reaction, 0, sizeof reaction);
    reaction.from = LT_PEER;
    reaction.to = LT_US;
    reaction.direction = (uint8_t)MESH_MESSAGE_INBOUND;
    reaction.is_reaction = true;
    reaction.pki_encrypted = true;
    const struct mesh_session_event event = {
        .kind = MESH_SESSION_EVENT_MESSAGE, .message = &reaction, .via_mqtt = true};
    mesh_lifetime_observe(&g_lifetime, &g_session, &event);
    lt_message(LT_US, LT_PEER, MESH_MESSAGE_OUTBOUND, false);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_RECEIVED_MQTT) != 2U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_DIRECT_RECEIVED_PRIVATE) !=
                              2U,
                      "a reaction and a send move neither");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/* The farthest node heard is heard over the air at any hop count, and never over MQTT. */
MESH_TEST_CASE(lifetime_farthest_heard_takes_any_hop_count, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    lt_session(515000000, 0);
    struct mesh_node_summary peer = lt_summary(LT_PEER);
    peer.position.valid = true;
    peer.position.latitude_i = 520000000; /* about 55.6 km north */

    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, true, true, 0U);
    lt_node(MESH_SESSION_EVENT_NODE_LISTED, &peer, false, true, 0U);
    MESH_TEST_FAIL_IF(mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_FARTHEST_HEARD_M),
                      "MQTT and a listing say nothing about our reach");

    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, false, true, 3U);
    const uint64_t heard = mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_FARTHEST_HEARD_M);
    MESH_TEST_FAIL_IF(heard < 55000U || heard > 56000U, "three hops out is still heard");
    MESH_TEST_FAIL_IF(mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_FARTHEST_DIRECT_M),
                      "and is not a direct record");
    uint32_t holder = 0U;
    MESH_TEST_FAIL_IF(
        !mesh_lifetime_holder(&g_lifetime, MESH_LIFETIME_FARTHEST_HEARD_M, &holder, NULL) ||
            holder != LT_PEER,
        "the record names the node heard");

    /* A packet with no hop count is still a packet our radio heard. */
    struct mesh_node_summary other = lt_summary(LT_OTHER);
    other.position.valid = true;
    other.position.latitude_i = 530000000;
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &other, false, false, 0U);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_FARTHEST_HEARD_M) <= heard,
                      "a farther node with no hop count raises it");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/*
 * The weakest signal decoded: the smallest SNR of a packet that came straight to our radio.
 *
 * A relayed packet's SNR is the last relay's, not the node's the record would name, and an MQTT
 * packet's is somebody else's antenna, so neither is taken. The record is signed - a packet under
 * the noise is below zero - and has to come back with its sign and its holder after a restart.
 */
MESH_TEST_CASE(lifetime_weakest_signal_is_the_faintest_heard_direct, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    lt_session(0, 0);
    const struct mesh_node_summary peer = lt_summary(LT_PEER);
    const struct mesh_node_summary other = lt_summary(LT_OTHER);

    lt_heard_snr(&peer, true, true, 0U, -18.0f);
    lt_heard_snr(&peer, false, true, 2U, -18.0f);
    lt_heard_snr(&peer, false, false, 0U, -18.0f);
    MESH_TEST_FAIL_IF(mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_WEAKEST_SNR_QDB),
                      "MQTT, a relay and an unknown path set no signal record");

    lt_heard_snr(&peer, false, true, 0U, strtof("nan", NULL));
    lt_heard_snr(&peer, false, true, 0U, strtof("-inf", NULL));
    lt_heard_snr(&peer, false, true, 0U, -1.0e30f);
    MESH_TEST_FAIL_IF(mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_WEAKEST_SNR_QDB),
                      "a reading no radio makes is a malformed packet's, not a record");
    lt_heard_snr(&peer, false, true, 0U, 6.5f);
    MESH_TEST_FAIL_IF(!mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_WEAKEST_SNR_QDB) ||
                          mesh_lifetime_signed(&g_lifetime, MESH_LIFETIME_WEAKEST_SNR_QDB) != 26,
                      "the first direct packet is the record, in quarters of a dB");
    lt_heard_snr(&other, false, true, 0U, -7.25f);
    lt_heard_snr(&peer, false, true, 0U, -3.0f);
    lt_heard_snr(&peer, false, true, 0U, -7.25f);
    MESH_TEST_FAIL_IF(mesh_lifetime_signed(&g_lifetime, MESH_LIFETIME_WEAKEST_SNR_QDB) != -29,
                      "the fainter packet lowers it, a stronger one does not");
    uint32_t holder = 0U;
    MESH_TEST_FAIL_IF(
        !mesh_lifetime_holder(&g_lifetime, MESH_LIFETIME_WEAKEST_SNR_QDB, &holder, NULL) ||
            holder != LT_OTHER,
        "and a tie keeps the node that was that faint first");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_WEAKEST_SNR_QDB) != 0U,
                      "a signed record is not read as an unsigned one");

    MESH_TEST_FAIL_IF(mesh_lifetime_flush(&g_lifetime) != 0, "the flush failed");
    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0, "the stats did not reopen");
    holder = 0U;
    MESH_TEST_FAIL_IF(mesh_lifetime_signed(&g_lifetime, MESH_LIFETIME_WEAKEST_SNR_QDB) != -29 ||
                          !mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_WEAKEST_SNR_QDB),
                      "a record below zero comes back with its sign");
    MESH_TEST_FAIL_IF(
        !mesh_lifetime_holder(&g_lifetime, MESH_LIFETIME_WEAKEST_SNR_QDB, &holder, NULL) ||
            holder != LT_OTHER,
        "and with its holder");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/*
 * A fact whose append failed is still counted, and is written by the next flush that can: the
 * set values are not in the totals, so a fact that never reached the seen file would be gone at
 * the next launch, and a node already in the set is never fresh enough to be written again.
 */
MESH_TEST_CASE(lifetime_a_failed_append_is_retried, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    MESH_TEST_FAIL_IF(mesh_lifetime_flush(&g_lifetime) != 0, "the first flush failed");
    lt_session(0, 0);
    /* The card goes away under the stats. */
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "could not take the directory away");

    struct mesh_node_summary peer = lt_summary(LT_PEER);
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, false, false, 0U);
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, false, false, 0U);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD_RF) != 1U,
                      "the node counts now");
    MESH_TEST_FAIL_IF(!mesh_lifetime_dirty(&g_lifetime) || g_lifetime.pending != 1U,
                      "and is waiting for a flush that can write it");
    MESH_TEST_FAIL_IF(mesh_lifetime_flush(&g_lifetime) >= 0 || !mesh_lifetime_dirty(&g_lifetime),
                      "a flush with nowhere to write fails and stays dirty");

    /* It comes back. */
    MESH_TEST_FAIL_IF(mkdir(dir, 0700) != 0, "could not restore the directory");
    MESH_TEST_FAIL_IF(mesh_lifetime_flush(&g_lifetime) != 0 || mesh_lifetime_dirty(&g_lifetime) ||
                          g_lifetime.pending != 0U,
                      "the next flush writes what was pending");
    MESH_TEST_FAIL_IF(lt_seen_lines(dir, "heard") != 1U || lt_seen_lines(dir, "rf") != 1U,
                      "once each");
    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0 ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD_RF) != 1U,
                      "and the node survives a restart");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/* A card moved back to an older build keeps what the newer one counted. */
MESH_TEST_CASE(lifetime_keeps_keys_it_does_not_know, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    char path[128];
    snprintf(path, sizeof path, "%s/totals.stats", dir);
    FILE *file = fopen(path, "w");
    MESH_TEST_FAIL_IF(file == NULL, "could not write the totals");
    fputs("messages_sent=41\nfuture_stat=7\n", file);
    fclose(file);

    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0, "the stats did not reopen");
    lt_session(0, 0);
    lt_message(LT_US, LT_PEER, MESH_MESSAGE_OUTBOUND, false);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MESSAGES_SENT) != 42U,
                      "the count carries on from the card");
    MESH_TEST_FAIL_IF(mesh_lifetime_flush(&g_lifetime) != 0, "the flush failed");

    file = fopen(path, "r");
    MESH_TEST_FAIL_IF(file == NULL, "the totals are gone");
    char line[128];
    bool kept = false;
    while (fgets(line, sizeof line, file) != NULL) {
        kept = kept || strcmp(line, "future_stat=7\n") == 0;
    }
    fclose(file);
    MESH_TEST_FAIL_IF(!kept, "a key this build does not know survives the rewrite");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

MESH_TEST_CASE(lifetime_a_torn_seen_line_is_skipped, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    char path[128];
    snprintf(path, sizeof path, "%s/seen.stats", dir);
    FILE *file = fopen(path, "w");
    MESH_TEST_FAIL_IF(file == NULL, "could not write the seen file");
    /* One whole line, one line that is not a node, and an append the power cut short. */
    fputs("heard=00002222\nheard=zz\nheard=0000", file);
    fclose(file);
    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0, "the stats did not reopen");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD) != 1U,
                      "only the whole line is a node");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

MESH_TEST_CASE(lifetime_reset_starts_from_nothing, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    lt_session(0, 0);
    struct mesh_node_summary peer = lt_summary(LT_PEER);
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &peer, false, true, 4U);
    lt_message(LT_US, LT_PEER, MESH_MESSAGE_OUTBOUND, false);
    MESH_TEST_FAIL_IF(mesh_lifetime_flush(&g_lifetime) != 0, "the flush failed");
    const uint32_t revision = g_lifetime.revision;

    MESH_TEST_FAIL_IF(mesh_lifetime_reset(&g_lifetime) != 0, "the reset failed");
    MESH_TEST_FAIL_IF(g_lifetime.revision == revision, "a reset is a change a screen redraws for");
    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0, "the stats did not reopen");
    for (unsigned i = 0; i < MESH_LIFETIME_STAT_COUNT; ++i) {
        MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, (enum mesh_lifetime_stat)i) != 0U,
                          mesh_lifetime_key((enum mesh_lifetime_stat)i));
    }
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/* `since` is written only off a clock that is credibly one, and the counts do not wait for it. */
MESH_TEST_CASE(lifetime_since_is_the_first_credible_second, unit) {
    char dir[64];
    inkwell_time_wall_set_fixed(86400U); /* 2 January 1970: a Brick that has never been set */
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    lt_session(0, 0);
    lt_message(LT_PEER, MESH_MESSAGE_BROADCAST_ADDR, MESH_MESSAGE_INBOUND, false);
    MESH_TEST_FAIL_IF(g_lifetime.since != 0U, "1970 is not a date");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MESSAGES_RECEIVED) != 1U,
                      "but the message still counts");

    inkwell_time_wall_set_fixed(1750000000U); /* set over Wi-Fi mid-run */
    lt_message(LT_PEER, MESH_MESSAGE_BROADCAST_ADDR, MESH_MESSAGE_INBOUND, false);
    inkwell_time_wall_set_fixed(1760000000U);
    lt_message(LT_PEER, MESH_MESSAGE_BROADCAST_ADDR, MESH_MESSAGE_INBOUND, false);
    inkwell_time_wall_set_fixed(0U);
    MESH_TEST_FAIL_IF(g_lifetime.since != 1750000000U, "the first credible second, and only it");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/* A full set turns a node away rather than forgetting one, and says the counts are a floor. */
MESH_TEST_CASE(lifetime_a_full_set_says_so, unit) {
    /* Disabled - no directory - so eight thousand nodes do not become eight thousand appends. */
    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, "") == 0, "an empty directory is refused");
    lt_session(0, 0);
    struct mesh_node_summary node = lt_summary(0U);
    for (uint32_t i = 1U; i <= MESH_LIFETIME_NODES_MAX; ++i) {
        node.node_id = 0x10000U + i * 7U;
        lt_node(MESH_SESSION_EVENT_NODE_HEARD, &node, false, false, 0U);
    }
    MESH_TEST_FAIL_IF(!mesh_lifetime_complete(&g_lifetime), "full is not yet over");
    node.node_id = 1U;
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &node, false, false, 0U);
    MESH_TEST_FAIL_IF(mesh_lifetime_complete(&g_lifetime) ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD) !=
                              MESH_LIFETIME_NODES_MAX,
                      "one more is turned away, and the count says it is a floor");
    node.node_id = 0x10000U + 7U;
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &node, true, false, 0U);
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD) !=
                          MESH_LIFETIME_NODES_MAX,
                      "a node already held is still held");
    record_success(test_name);
}

/*
 * A full set is still a floor after a restart.
 *
 * The seen file cannot say so on its own: a set that turned a node away holds exactly
 * MESH_LIFETIME_NODES_MAX nodes, the same as one that merely reached its size, because the
 * refusal appends nothing. So the refusal goes in the totals, and a restart that dropped it
 * would put an exact-looking count on the screen over nodes that were never counted.
 */
MESH_TEST_CASE(lifetime_a_full_set_is_still_a_floor_after_a_restart, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");

    /* A seen file already at the set's size, written directly rather than through eight
       thousand appends. */
    char path[128];
    snprintf(path, sizeof path, "%s/seen.stats", dir);
    FILE *file = fopen(path, "w");
    MESH_TEST_FAIL_IF(file == NULL, "could not write the seen file");
    for (uint32_t i = 1U; i <= MESH_LIFETIME_NODES_MAX; ++i) {
        fprintf(file, "heard=%08x\n", 0x10000U + i);
    }
    fclose(file);

    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0, "the stats did not reopen");
    MESH_TEST_FAIL_IF(!mesh_lifetime_complete(&g_lifetime),
                      "a set at its size that has turned nobody away is still exact");

    lt_session(0, 0);
    struct mesh_node_summary stranger = lt_summary(0x7FFFFFFFU);
    lt_node(MESH_SESSION_EVENT_NODE_HEARD, &stranger, false, false, 0U);
    MESH_TEST_FAIL_IF(mesh_lifetime_complete(&g_lifetime), "one more is turned away");
    MESH_TEST_FAIL_IF(!mesh_lifetime_dirty(&g_lifetime), "and that is something to write down");
    MESH_TEST_FAIL_IF(mesh_lifetime_flush(&g_lifetime) != 0, "the flush failed");

    MESH_TEST_FAIL_IF(mesh_lifetime_init(&g_lifetime, dir) != 0, "the stats did not reopen");
    MESH_TEST_FAIL_IF(mesh_lifetime_complete(&g_lifetime),
                      "the restart should remember the set is a floor");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD) !=
                          MESH_LIFETIME_NODES_MAX,
                      "with every node it did hold");

    MESH_TEST_FAIL_IF(mesh_lifetime_reset(&g_lifetime) != 0, "the reset failed");
    MESH_TEST_FAIL_IF(!mesh_lifetime_complete(&g_lifetime), "and only a reset forgets it");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}

/* End to end, through the session's own observer: what one conversation adds up to. */
MESH_TEST_CASE(lifetime_counts_what_the_session_announces, unit) {
    char dir[64];
    MESH_TEST_FAIL_IF(!lt_open(dir, sizeof dir), "the stats did not open");
    mesh_session_init(&g_session);
    struct mesh_test_trace_capture capture;
    memset(&capture, 0, sizeof capture);
    mesh_session_attach(&g_session, mesh_test_trace_capture_fn, &capture);
    mesh_session_set_observer(&g_session, mesh_lifetime_observe, &g_lifetime);
    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = LT_US;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&g_session, &my_info), "encode failed");

    const uint8_t hello[] = "hello";
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_app_packet(&g_session, LT_PEER,
                                                         meshtastic_PortNum_TEXT_MESSAGE_APP, hello,
                                                         sizeof hello - 1U),
                      "encode failed");
    uint32_t packet_id = 0U;
    MESH_TEST_FAIL_IF(mesh_session_send_text(&g_session, LT_PEER, 0U, "hi", true, &packet_id) != 0,
                      "the send failed");
    /* The echo of our send, and the same node again. */
    meshtastic_FromRadio echo = meshtastic_FromRadio_init_default;
    echo.which_payload_variant = meshtastic_FromRadio_packet_tag;
    echo.packet.from = LT_US;
    echo.packet.to = LT_PEER;
    echo.packet.id = packet_id;
    echo.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    echo.packet.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    memcpy(echo.packet.decoded.payload.bytes, "hi", 2U);
    echo.packet.decoded.payload.size = 2U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&g_session, &echo), "encode failed");

    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MESSAGES_RECEIVED) != 1U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_MESSAGES_SENT) != 1U ||
                          mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_DIRECT_SENT) != 1U,
                      "one message each way, the echo counted by nobody");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_NODES_HEARD_RF) != 1U,
                      "one node heard, and not our own");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_RADIOS) != 1U,
                      "and the radio counted from its MyNodeInfo, with nothing published");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_RECEIVED_MQTT) != 0U,
                      "nothing so far came over MQTT");

    /* A message over MQTT, and a direct packet heard under the noise. */
    meshtastic_FromRadio bridged = meshtastic_FromRadio_init_default;
    bridged.which_payload_variant = meshtastic_FromRadio_packet_tag;
    bridged.packet.from = LT_OTHER;
    bridged.packet.to = MESH_MESSAGE_BROADCAST_ADDR;
    bridged.packet.id = 0x5151U;
    bridged.packet.via_mqtt = true;
    bridged.packet.rx_snr = -19.0f;
    bridged.packet.hop_start = 3U;
    bridged.packet.hop_limit = 3U;
    bridged.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    bridged.packet.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    memcpy(bridged.packet.decoded.payload.bytes, "far", 3U);
    bridged.packet.decoded.payload.size = 3U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&g_session, &bridged), "encode failed");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_RECEIVED_MQTT) != 1U,
                      "a message over MQTT is counted as one");
    MESH_TEST_FAIL_IF(mesh_lifetime_measured(&g_lifetime, MESH_LIFETIME_WEAKEST_SNR_QDB),
                      "and its SNR is not our radio's");

    meshtastic_FromRadio faint = bridged;
    faint.packet.id = 0x5152U;
    faint.packet.via_mqtt = false;
    faint.packet.rx_snr = -12.5f;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&g_session, &faint), "encode failed");
    MESH_TEST_FAIL_IF(mesh_lifetime_signed(&g_lifetime, MESH_LIFETIME_WEAKEST_SNR_QDB) != -50,
                      "a direct packet's SNR is the record");
    MESH_TEST_FAIL_IF(mesh_lifetime_value(&g_lifetime, MESH_LIFETIME_RECEIVED_MQTT) != 1U,
                      "and the one over the air is not MQTT's");
    MESH_TEST_FAIL_IF(!mesh_test_remove_tree(dir), "cleanup failed");
    record_success(test_name);
}
