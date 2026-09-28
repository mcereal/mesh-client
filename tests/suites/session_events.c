#define _POSIX_C_SOURCE 200809L

/*
 * What the session announces as it happens (struct mesh_session_event).
 *
 * A listener counts these, so the promise worth pinning is "once": a record announced twice is
 * a tally that drifts upward at every echo, and one announced when it was only handed back -
 * a roster seeded from the card - is a tally that grows at every launch.
 */

#include "framework/mesh_test.h"
#include "support/session_fixture.h"

#include "mesh/core/message.h"
#include "mesh/core/session.h"

#include <pb_encode.h>

#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EV_US 0x1111U
#define EV_PEER 0x2222U
#define EV_OTHER 0x3333U

static void ev_open(struct mesh_session *session, struct mesh_test_trace_capture *capture,
                    struct mesh_test_event_record *record) {
    mesh_session_init(session);
    memset(capture, 0, sizeof *capture);
    memset(record, 0, sizeof *record);
    mesh_session_attach(session, mesh_test_trace_capture_fn, capture);
    mesh_session_set_observer(session, mesh_test_event_record_fn, record);

    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = EV_US;
    (void)mesh_test_session_feed_from_radio(session, &my_info);
    /* The radio saying who it is is announced too; each case starts after it. */
    memset(record, 0, sizeof *record);
}

static meshtastic_FromRadio ev_text(uint32_t from, uint32_t id, const char *text) {
    meshtastic_FromRadio packet = meshtastic_FromRadio_init_default;
    packet.which_payload_variant = meshtastic_FromRadio_packet_tag;
    packet.packet.from = from;
    packet.packet.to = MESH_MESSAGE_BROADCAST_ADDR;
    packet.packet.id = id;
    packet.packet.has_rx_time = true;
    packet.packet.rx_time = 1750000000U;
    packet.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.packet.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    packet.packet.decoded.payload.size = (pb_size_t)strlen(text);
    memcpy(packet.packet.decoded.payload.bytes, text, packet.packet.decoded.payload.size);
    return packet;
}

MESH_TEST_CASE(session_events_announce_a_message_and_its_sender_once, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    struct mesh_test_event_record record;
    ev_open(&session, &capture, &record);

    meshtastic_FromRadio packet = ev_text(EV_PEER, 0x100U, "hello");
    packet.packet.hop_start = 3U;
    packet.packet.hop_limit = 1U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &packet), "encode failed");

    MESH_TEST_FAIL_IF(record.count != 2U, "one packet is one sender heard and one message");
    const struct mesh_test_event *heard = &record.events[0];
    MESH_TEST_FAIL_IF(heard->kind != MESH_SESSION_EVENT_NODE_HEARD || heard->node_id != EV_PEER ||
                          !heard->has_hops || heard->hops != 2U || heard->via_mqtt,
                      "the sender is heard, two hops out, over the air");
    const struct mesh_test_event *message = &record.events[1];
    MESH_TEST_FAIL_IF(message->kind != MESH_SESSION_EVENT_MESSAGE || message->node_id != EV_PEER ||
                          message->packet_id != 0x100U ||
                          message->direction != MESH_MESSAGE_INBOUND,
                      "and the message is announced as it was appended");
    record_success(test_name);
}

/* The hop count and the MQTT leg are the packet's, not the record's: a packet whose firmware
   does not report hops leaves the record's older count standing, and that count is not news. */
MESH_TEST_CASE(session_events_carry_the_packets_own_path, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    struct mesh_test_event_record record;
    ev_open(&session, &capture, &record);

    meshtastic_FromRadio first = ev_text(EV_PEER, 0x100U, "direct");
    first.packet.hop_start = 3U;
    first.packet.hop_limit = 3U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &first), "encode failed");
    meshtastic_FromRadio bridged = ev_text(EV_PEER, 0x101U, "bridged");
    bridged.packet.via_mqtt = true;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &bridged), "encode failed");

    MESH_TEST_FAIL_IF(mesh_test_event_count(&record, MESH_SESSION_EVENT_NODE_HEARD, EV_PEER) != 2U,
                      "each packet is a hearing");
    MESH_TEST_FAIL_IF(!record.events[0].has_hops || record.events[0].hops != 0U ||
                          record.events[0].via_mqtt,
                      "the first was heard direct");
    MESH_TEST_FAIL_IF(record.events[2].has_hops || !record.events[2].via_mqtt,
                      "the second came over MQTT and said nothing about hops");
    record_success(test_name);
}

MESH_TEST_CASE(session_events_a_send_is_announced_once_and_its_echo_never, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    struct mesh_test_event_record record;
    ev_open(&session, &capture, &record);

    uint32_t packet_id = 0U;
    MESH_TEST_FAIL_IF(mesh_session_send_text(&session, EV_OTHER, 0U, "out", true, &packet_id) != 0,
                      "the send failed");
    MESH_TEST_FAIL_IF(record.count != 1U || record.events[0].kind != MESH_SESSION_EVENT_MESSAGE ||
                          record.events[0].direction != MESH_MESSAGE_OUTBOUND ||
                          record.events[0].packet_id != packet_id,
                      "a send is announced when the record is made");

    /* The radio hands it back; the log refreshes the record the send made. */
    meshtastic_FromRadio echo = ev_text(EV_US, packet_id, "out");
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &echo), "encode failed");
    MESH_TEST_FAIL_IF(record.count != 1U, "an echo is neither a second message nor a node heard");
    record_success(test_name);
}

MESH_TEST_CASE(session_events_a_seeded_node_is_not_news, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    struct mesh_test_event_record record;
    ev_open(&session, &capture, &record);

    struct mesh_node_summary cached;
    memset(&cached, 0, sizeof cached);
    cached.node_id = EV_PEER;
    cached.last_heard = 1700000000U;
    mesh_session_seed_node(&session, &cached);
    MESH_TEST_FAIL_IF(record.count != 0U, "a node handed back from the card was already counted");
    record_success(test_name);
}

/*
 * The radio's database is announced as a listing - heard, but not necessarily while we were
 * watching - and only for a record the radio has a heard time for. Our own node is in that
 * database too and is never announced.
 */
MESH_TEST_CASE(session_events_the_nodedb_lists_what_the_radio_heard, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    struct mesh_test_event_record record;
    ev_open(&session, &capture, &record);
    MESH_TEST_FAIL_IF(mesh_session_begin_handshake(&session) != 0, "the handshake did not start");

    const struct {
        uint32_t num;
        uint32_t last_heard;
    } nodes[] = {{EV_US, 1750000000U}, {EV_PEER, 1750000000U}, {EV_OTHER, 0U}};
    for (size_t i = 0; i < sizeof nodes / sizeof nodes[0]; ++i) {
        meshtastic_FromRadio info = meshtastic_FromRadio_init_default;
        info.which_payload_variant = meshtastic_FromRadio_node_info_tag;
        info.node_info.num = nodes[i].num;
        info.node_info.last_heard = nodes[i].last_heard;
        info.node_info.has_hops_away = true;
        info.node_info.hops_away = 1U;
        MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &info), "encode failed");
    }

    MESH_TEST_FAIL_IF(record.count != 1U ||
                          record.events[0].kind != MESH_SESSION_EVENT_NODE_LISTED ||
                          record.events[0].node_id != EV_PEER || !record.events[0].has_hops ||
                          record.events[0].hops != 1U,
                      "only the node the radio heard is listed, and never our own");

    /* And a listener taken away hears nothing more. */
    mesh_session_set_observer(&session, NULL, NULL);
    meshtastic_FromRadio packet = ev_text(EV_PEER, 0x200U, "quiet");
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &packet), "encode failed");
    MESH_TEST_FAIL_IF(record.count != 1U, "a cleared observer is not called");
    record_success(test_name);
}

MESH_TEST_CASE(session_events_the_radio_says_who_it_is, unit) {
    struct mesh_session session;
    mesh_session_init(&session);
    struct mesh_test_event_record record;
    memset(&record, 0, sizeof record);
    mesh_session_set_observer(&session, mesh_test_event_record_fn, &record);
    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = EV_US;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &my_info), "encode failed");
    MESH_TEST_FAIL_IF(record.count != 1U || record.events[0].kind != MESH_SESSION_EVENT_RADIO ||
                          record.events[0].radio != EV_US,
                      "MyNodeInfo announces the attached radio, with no frame drawn");

    /* A protocol that fills the model says the same through the model call. */
    mesh_session_model_adopt_radio(&session, EV_OTHER);
    MESH_TEST_FAIL_IF(record.count != 2U || record.events[1].kind != MESH_SESSION_EVENT_RADIO ||
                          record.events[1].radio != EV_OTHER,
                      "adopting a radio announces it too");
    record_success(test_name);
}

/* A hearing is announced after the packet's payload is on the record, so a node's first fix is
   there to be measured from the packet that carried it. */
MESH_TEST_CASE(session_events_a_position_is_on_the_record_when_announced, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    struct mesh_test_event_record record;
    ev_open(&session, &capture, &record);

    meshtastic_Position position = meshtastic_Position_init_default;
    position.has_latitude_i = true;
    position.latitude_i = 516000000;
    position.has_longitude_i = true;
    position.longitude_i = 0;
    uint8_t payload[64];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Position_fields, &position) ||
                          !mesh_test_session_feed_app_packet(&session, EV_PEER,
                                                             meshtastic_PortNum_POSITION_APP,
                                                             payload, stream.bytes_written),
                      "encode POSITION_APP failed");
    MESH_TEST_FAIL_IF(record.count != 1U ||
                          record.events[0].kind != MESH_SESSION_EVENT_NODE_HEARD ||
                          !record.events[0].has_position,
                      "the first fix is on the record the hearing names");
    record_success(test_name);
}

/* ---- which nodes are discoveries ------------------------------------------------------------ */

static const struct mesh_node_summary *ev_node(const struct mesh_session *session, uint32_t id) {
    const struct mesh_handshake_status *handshake = mesh_session_handshake(session);
    for (size_t i = 0; i < handshake->node_count; ++i) {
        if (handshake->nodes[i].node_id == id) {
            return &handshake->nodes[i];
        }
    }
    return NULL;
}

static bool ev_feed_node_info(struct mesh_session *session, uint32_t num) {
    meshtastic_FromRadio info = meshtastic_FromRadio_init_default;
    info.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    info.node_info.num = num;
    info.node_info.last_heard = 1750000000U;
    return mesh_test_session_feed_from_radio(session, &info);
}

static bool ev_complete_sync(struct mesh_session *session) {
    meshtastic_FromRadio done = meshtastic_FromRadio_init_default;
    done.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
    done.config_complete_id = mesh_session_handshake(session)->request_id;
    return mesh_test_session_feed_from_radio(session, &done);
}

/*
 * The first sync of an empty roster is the radio's database arriving whole, and none of it is a
 * discovery - "80 new nodes" on a fresh install is true of every node and useful about none. A
 * node heard for the first time *after* that sync is one.
 */
MESH_TEST_CASE(session_nodes_the_first_sync_of_an_empty_roster_discovers_nothing, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    struct mesh_test_event_record record;
    ev_open(&session, &capture, &record);
    MESH_TEST_FAIL_IF(mesh_session_begin_handshake(&session) != 0, "the handshake did not start");

    MESH_TEST_FAIL_IF(!ev_feed_node_info(&session, EV_US) ||
                          !ev_feed_node_info(&session, EV_PEER) || !ev_complete_sync(&session),
                      "encode failed");
    MESH_TEST_FAIL_IF(mesh_session_nodes_discovered(&session) != 0U,
                      "a sync that began with nothing to compare against discovers nothing");
    const struct mesh_node_summary *peer = ev_node(&session, EV_PEER);
    MESH_TEST_FAIL_IF(peer == NULL || peer->discovered != 0U, "so the node it brought is not new");

    meshtastic_FromRadio packet = ev_text(EV_OTHER, 0x300U, "just arrived");
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &packet), "encode failed");
    const struct mesh_node_summary *other = ev_node(&session, EV_OTHER);
    MESH_TEST_FAIL_IF(mesh_session_nodes_discovered(&session) != 1U || other == NULL ||
                          other->discovered != 1U,
                      "a node first heard after the sync is the first discovery");

    /* Heard again, it is the same discovery - a node is new the once. */
    meshtastic_FromRadio again = ev_text(EV_OTHER, 0x301U, "still here");
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &again), "encode failed");
    MESH_TEST_FAIL_IF(mesh_session_nodes_discovered(&session) != 1U || other->discovered != 1U,
                      "hearing a known node again discovers nothing");
    record_success(test_name);
}

/*
 * A sync against a roster that already holds nodes is the other case: a node in *that* replay is
 * one the radio heard while the client was not running, which is exactly the news somebody coming
 * back wants. The node the card handed back is not, and neither is our own.
 */
MESH_TEST_CASE(session_nodes_a_resync_discovers_what_the_radio_heard_meanwhile, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    struct mesh_test_event_record record;
    ev_open(&session, &capture, &record);

    struct mesh_node_summary cached;
    memset(&cached, 0, sizeof cached);
    cached.node_id = EV_PEER;
    cached.discovered = 7U; /* another run's count, which must not survive the seed */
    mesh_session_seed_node(&session, &cached);
    MESH_TEST_FAIL_IF(mesh_session_nodes_discovered(&session) != 0U ||
                          ev_node(&session, EV_PEER)->discovered != 0U,
                      "a node handed back from the card is not a discovery");

    MESH_TEST_FAIL_IF(mesh_session_begin_handshake(&session) != 0, "the handshake did not start");
    MESH_TEST_FAIL_IF(!ev_feed_node_info(&session, EV_US) ||
                          !ev_feed_node_info(&session, EV_PEER) ||
                          !ev_feed_node_info(&session, EV_OTHER) || !ev_complete_sync(&session),
                      "encode failed");
    MESH_TEST_FAIL_IF(mesh_session_nodes_discovered(&session) != 1U,
                      "only the node the roster did not hold is a discovery");
    MESH_TEST_FAIL_IF(ev_node(&session, EV_OTHER)->discovered != 1U,
                      "and it carries its place in the count");
    MESH_TEST_FAIL_IF(ev_node(&session, EV_US)->discovered != 0U, "our own record is never news");
    record_success(test_name);
}

/* A first sync the link drops halfway through is still a first sync when it is retried: the half
   it delivered is not a roster the other half could be new against. */
MESH_TEST_CASE(session_nodes_an_interrupted_first_sync_stays_a_first_sync, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    struct mesh_test_event_record record;
    ev_open(&session, &capture, &record);

    MESH_TEST_FAIL_IF(mesh_session_begin_handshake(&session) != 0, "the handshake did not start");
    MESH_TEST_FAIL_IF(!ev_feed_node_info(&session, EV_PEER), "encode failed");
    /* The link drops before config_complete; the retry asks again. */
    MESH_TEST_FAIL_IF(mesh_session_begin_handshake(&session) != 0, "the retry did not start");
    MESH_TEST_FAIL_IF(!ev_feed_node_info(&session, EV_PEER) ||
                          !ev_feed_node_info(&session, EV_OTHER) || !ev_complete_sync(&session),
                      "encode failed");
    MESH_TEST_FAIL_IF(mesh_session_nodes_discovered(&session) != 0U,
                      "the rest of an interrupted first sync is not news");
    record_success(test_name);
}

/* A restart whose card held only our own record: the owner is known from the cache before the
   radio has said who it is, and that roster is as bare as an empty one. */
MESH_TEST_CASE(session_nodes_a_cache_of_only_ourselves_is_a_bare_roster, unit) {
    struct mesh_session session;
    mesh_session_init(&session);
    struct mesh_test_trace_capture capture;
    memset(&capture, 0, sizeof capture);
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &capture);

    mesh_session_set_roster_owner(&session, EV_US);
    struct mesh_node_summary self;
    memset(&self, 0, sizeof self);
    self.node_id = EV_US;
    mesh_session_seed_node(&session, &self);

    MESH_TEST_FAIL_IF(mesh_session_begin_handshake(&session) != 0, "the handshake did not start");
    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = EV_US;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &my_info) ||
                          !ev_feed_node_info(&session, EV_PEER) ||
                          !ev_feed_node_info(&session, EV_OTHER) || !ev_complete_sync(&session),
                      "encode failed");
    MESH_TEST_FAIL_IF(mesh_session_nodes_discovered(&session) != 0U,
                      "a roster of only ourselves has nothing for the sync to be new against");
    record_success(test_name);
}
