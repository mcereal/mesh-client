#define _POSIX_C_SOURCE 200809L

/*
 * Store & Forward: asking a router for the traffic that arrived while this client was off.
 *
 * The cases are grouped by the promise rather than by the file, because the interesting ones
 * cross the seam: a replayed message is decoded in store_forward.c, de-duplicated in message.c
 * and appended by session.c, and the promise that matters - "one press does not put a second
 * copy of the last four hours under the first" - is only true if all three agree.
 */

#include "framework/mesh_test.h"
#include "support/session_fixture.h"
#include "support/ui_fixture.h"

#include "mesh/core/session.h"
#include "mesh/core/store_forward.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

#include "meshtastic/storeforward.pb.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <errno.h>
#include <string.h>

#define SF_US 0x1111U
#define SF_ROUTER 0x2222U
#define SF_TALKER 0x3333U
#define SF_ROUTER_B 0x4444U
#define SF_WHEN 1750000000U

/* One StoreAndForward wrapped in the MeshPacket a router would have sent it in: the envelope
   carries the *original* sender, channel and time when the payload is a replayed message,
   which is the whole reason a replay lands in the right conversation. */
static bool sf_feed(struct mesh_session *session, const meshtastic_StoreAndForward *sf,
                    uint32_t from, uint8_t channel, uint32_t rx_time) {
    uint8_t payload[256];
    pb_ostream_t out = pb_ostream_from_buffer(payload, sizeof payload);
    if (!pb_encode(&out, meshtastic_StoreAndForward_fields, sf)) {
        return false;
    }
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_packet_tag;
    from_radio.packet.from = from;
    from_radio.packet.to = SF_US;
    from_radio.packet.id = 0x5F00U;
    from_radio.packet.channel = channel;
    from_radio.packet.has_rx_time = rx_time != 0U;
    from_radio.packet.rx_time = rx_time;
    from_radio.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    from_radio.packet.decoded.portnum = meshtastic_PortNum_STORE_FORWARD_APP;
    memcpy(from_radio.packet.decoded.payload.bytes, payload, out.bytes_written);
    from_radio.packet.decoded.payload.size = (pb_size_t)out.bytes_written;
    return mesh_test_session_feed_from_radio(session, &from_radio);
}

/* A ROUTER_TEXT_* frame: what one replayed message looks like on the air. */
static meshtastic_StoreAndForward sf_text(const char *text, bool broadcast) {
    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_default;
    sf.rr = broadcast ? meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_BROADCAST
                      : meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_DIRECT;
    sf.which_variant = meshtastic_StoreAndForward_text_tag;
    sf.variant.text.size = (pb_size_t)strlen(text);
    memcpy(sf.variant.text.bytes, text, sf.variant.text.size);
    return sf;
}

static meshtastic_StoreAndForward sf_history(uint32_t count, uint32_t last_request) {
    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_default;
    sf.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY;
    sf.which_variant = meshtastic_StoreAndForward_history_tag;
    sf.variant.history.history_messages = count;
    sf.variant.history.last_request = last_request;
    return sf;
}

static void sf_open_session(struct mesh_session *session, struct mesh_test_trace_capture *capture) {
    mesh_session_init(session);
    memset(capture, 0, sizeof *capture);
    mesh_session_attach(session, mesh_test_trace_capture_fn, capture);

    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = SF_US;
    (void)mesh_test_session_feed_from_radio(session, &my_info);
}

/* Decodes the last ToRadio the session handed the link, and the StoreAndForward inside it. */
static bool sf_last_sent(const struct mesh_test_trace_capture *capture,
                         meshtastic_ToRadio *to_radio, meshtastic_StoreAndForward *sf) {
    *to_radio = (meshtastic_ToRadio)meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(capture->packet, capture->len);
    if (!pb_decode(&in, meshtastic_ToRadio_fields, to_radio) ||
        to_radio->which_payload_variant != meshtastic_ToRadio_packet_tag ||
        to_radio->packet.which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        to_radio->packet.decoded.portnum != meshtastic_PortNum_STORE_FORWARD_APP) {
        return false;
    }
    *sf = (meshtastic_StoreAndForward)meshtastic_StoreAndForward_init_default;
    pb_istream_t body = pb_istream_from_buffer(to_radio->packet.decoded.payload.bytes,
                                               to_radio->packet.decoded.payload.size);
    return pb_decode(&body, meshtastic_StoreAndForward_fields, sf);
}

/* ---- the wire ------------------------------------------------------------------------------ */

MESH_TEST_CASE(store_forward_encodes_a_history_request, unit) {
    struct mesh_store_forward_request request = {
        .dest = SF_ROUTER,
        .packet_id = 0x1234U,
        .channel = 2U,
        .window_minutes = 0U,
        .cursor = 17U,
        .ping = false,
    };
    uint8_t buffer[256];
    size_t written = 0U;
    MESH_TEST_FAIL_IF(mesh_store_forward_encode(&request, buffer, sizeof buffer, &written) != 0,
                      "a history request should encode");

    meshtastic_ToRadio sent = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_ToRadio_fields, &sent),
                      "the request should decode as a ToRadio");
    MESH_TEST_FAIL_IF(sent.packet.to != SF_ROUTER || sent.packet.id != 0x1234U ||
                          sent.packet.channel != 2U ||
                          sent.packet.decoded.portnum != meshtastic_PortNum_STORE_FORWARD_APP,
                      "the request should be addressed to the router on its own channel");
    /* The reply is the acknowledgement; a Routing ack on top would be a second round trip
       across the mesh to learn what the replay already says. */
    MESH_TEST_FAIL_IF(sent.packet.want_ack || !sent.packet.decoded.want_response,
                      "a request should want a response and not an ack");

    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_default;
    pb_istream_t body =
        pb_istream_from_buffer(sent.packet.decoded.payload.bytes, sent.packet.decoded.payload.size);
    MESH_TEST_FAIL_IF(!pb_decode(&body, meshtastic_StoreAndForward_fields, &sf),
                      "the payload should decode as a StoreAndForward");
    MESH_TEST_FAIL_IF(sf.rr != meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY ||
                          sf.which_variant != meshtastic_StoreAndForward_history_tag,
                      "the request should be a CLIENT_HISTORY carrying a History");
    /* The cursor from the last reply, so a second press does not fetch the first press's
       messages again; and a window of 0, which leaves the router its own configured one. */
    MESH_TEST_FAIL_IF(sf.variant.history.last_request != 17U || sf.variant.history.window != 0U,
                      "the request should carry the cursor and no window of its own");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_refuses_to_broadcast_a_history_request, unit) {
    struct mesh_store_forward_request request = {
        .dest = MESH_MESSAGE_BROADCAST_ADDR,
        .packet_id = 1U,
        .ping = false,
    };
    uint8_t buffer[256];
    size_t written = 0U;
    /* Every router on the mesh would replay its whole window at us at once. The broadcast
       address belongs to the ping and to nothing else on this port. */
    MESH_TEST_FAIL_IF(mesh_store_forward_encode(&request, buffer, sizeof buffer, &written) !=
                          -EINVAL,
                      "a broadcast history request should be refused");

    request.ping = true;
    MESH_TEST_FAIL_IF(mesh_store_forward_encode(&request, buffer, sizeof buffer, &written) != 0,
                      "the ping is the one thing that may go to everybody");

    meshtastic_ToRadio sent = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_default;
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_ToRadio_fields, &sent), "the ping should decode");
    pb_istream_t body =
        pb_istream_from_buffer(sent.packet.decoded.payload.bytes, sent.packet.decoded.payload.size);
    MESH_TEST_FAIL_IF(!pb_decode(&body, meshtastic_StoreAndForward_fields, &sf) ||
                          sf.rr != meshtastic_StoreAndForward_RequestResponse_CLIENT_PING ||
                          sf.which_variant != 0U,
                      "the ping should be a bare CLIENT_PING");

    record_success(test_name);
}

/* ---- learning who the router is ------------------------------------------------------------ */

MESH_TEST_CASE(store_forward_heartbeat_names_the_router, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    meshtastic_StoreAndForward beat = meshtastic_StoreAndForward_init_default;
    beat.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT;
    beat.which_variant = meshtastic_StoreAndForward_heartbeat_tag;
    beat.variant.heartbeat.period = 900U;
    beat.variant.heartbeat.secondary = 1U;
    MESH_TEST_FAIL_IF(!sf_feed(&session, &beat, SF_ROUTER, 0U, SF_WHEN), "encode heartbeat failed");

    const struct mesh_store_forward *state = mesh_session_store_forward(&session);
    MESH_TEST_FAIL_IF(state->router != SF_ROUTER || state->heartbeat_period != 900U ||
                          !state->router_secondary,
                      "the heartbeat should name the router, its period and its rank");
    /* A heartbeat is often the only packet a router ever sends, so it is also what keeps the
       router in the roster - a router the Nodes tab has never heard of is a router nothing can
       name. */
    MESH_TEST_FAIL_IF(mesh_test_session_find_node(&session, SF_ROUTER) == NULL,
                      "a heartbeat should touch the node that sent it");
    /* Nothing was asked for, so nothing is running. */
    MESH_TEST_FAIL_IF(state->state != MESH_STORE_FORWARD_IDLE,
                      "an unprompted heartbeat should not start a request");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_finds_a_router_then_asks_it, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    /* Nothing has announced itself yet, so the press has to go looking first. On a mesh whose
       router heartbeats every fifteen minutes, waiting for one would make this useless in
       exactly the minutes after a boot. */
    MESH_TEST_FAIL_IF(mesh_session_request_history(&session) != 0 || capture.calls != 1U,
                      "the request should have put a ping on the air");

    meshtastic_ToRadio sent;
    meshtastic_StoreAndForward sf;
    MESH_TEST_FAIL_IF(!sf_last_sent(&capture, &sent, &sf), "the ping should decode");
    MESH_TEST_FAIL_IF(sent.packet.to != MESH_MESSAGE_BROADCAST_ADDR ||
                          sf.rr != meshtastic_StoreAndForward_RequestResponse_CLIENT_PING,
                      "with no router known the press should broadcast a ping");
    MESH_TEST_FAIL_IF(mesh_session_store_forward(&session)->state != MESH_STORE_FORWARD_SEEKING,
                      "the client should be seeking a router");

    /* A second press while one is running would restart the count against a replay still on
       its way. */
    MESH_TEST_FAIL_IF(mesh_session_request_history(&session) != -EBUSY || capture.calls != 1U,
                      "a second request should be refused while one is running");

    meshtastic_StoreAndForward pong = meshtastic_StoreAndForward_init_default;
    pong.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_PONG;
    MESH_TEST_FAIL_IF(!sf_feed(&session, &pong, SF_ROUTER, 3U, SF_WHEN), "encode pong failed");

    /* Armed, not sent: the reply arrived on the link's read path and the follow-up goes out on
       the next tick rather than writing back down the link on the same turn. */
    MESH_TEST_FAIL_IF(capture.calls != 1U, "the follow-up should not be sent from the ingest");
    mesh_session_tick(&session, 1000U);
    MESH_TEST_FAIL_IF(capture.calls != 2U, "the tick should have sent the history request");

    MESH_TEST_FAIL_IF(!sf_last_sent(&capture, &sent, &sf), "the history request should decode");
    MESH_TEST_FAIL_IF(sent.packet.to != SF_ROUTER || sent.packet.channel != 3U ||
                          sf.rr != meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY,
                      "the request should go to the router that answered, the way it came");
    MESH_TEST_FAIL_IF(mesh_session_store_forward(&session)->state != MESH_STORE_FORWARD_REQUESTED,
                      "the client should now be waiting on the router");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_asks_a_router_it_already_knows, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    meshtastic_StoreAndForward beat = meshtastic_StoreAndForward_init_default;
    beat.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT;
    MESH_TEST_FAIL_IF(!sf_feed(&session, &beat, SF_ROUTER, 1U, SF_WHEN), "encode heartbeat failed");

    MESH_TEST_FAIL_IF(mesh_session_request_history(&session) != 0 || capture.calls != 1U,
                      "the request should have gone out");
    meshtastic_ToRadio sent;
    meshtastic_StoreAndForward sf;
    MESH_TEST_FAIL_IF(!sf_last_sent(&capture, &sent, &sf), "the request should decode");
    /* No ping: we already know who would answer it. */
    MESH_TEST_FAIL_IF(sent.packet.to != SF_ROUTER ||
                          sf.rr != meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY,
                      "a known router should be asked directly");

    record_success(test_name);
}

/* ---- the replay ---------------------------------------------------------------------------- */

MESH_TEST_CASE(store_forward_replay_lands_in_the_transcript, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    meshtastic_StoreAndForward history = sf_history(2U, 42U);
    MESH_TEST_FAIL_IF(!sf_feed(&session, &history, SF_ROUTER, 0U, SF_WHEN),
                      "encode ROUTER_HISTORY failed");
    const struct mesh_store_forward *state = mesh_session_store_forward(&session);
    MESH_TEST_FAIL_IF(state->state != MESH_STORE_FORWARD_REPLAYING || state->expected != 2U ||
                          state->cursor != 42U,
                      "the announcement should say how much is coming and leave a cursor");

    /* The envelope is the original sender's, which is what puts a replayed message in the
       conversation it was said in rather than in one with the router. */
    meshtastic_StoreAndForward first = sf_text("morning all", true);
    meshtastic_StoreAndForward second = sf_text("you awake?", false);
    MESH_TEST_FAIL_IF(!sf_feed(&session, &first, SF_TALKER, 4U, SF_WHEN - 3600U) ||
                          !sf_feed(&session, &second, SF_TALKER, 0U, SF_WHEN - 1800U),
                      "encode replayed texts failed");

    const struct mesh_message_log *log = mesh_session_messages(&session);
    MESH_TEST_FAIL_IF(log->count != 2U, "both replayed messages should be in the log");

    const struct mesh_message *broadcast = mesh_message_log_at(log, 0U);
    MESH_TEST_FAIL_IF(strcmp(broadcast->text, "morning all") != 0 || broadcast->from != SF_TALKER ||
                          broadcast->channel != 4U || broadcast->to != MESH_MESSAGE_BROADCAST_ADDR,
                      "a replayed broadcast should keep its sender and channel");
    /*
     * And carry no date. mesh.proto says of rx_time that the field "is _never_ sent on the radio
     * link itself", so the stamp on a replay packet is our own radio marking when the *replay*
     * landed - copying it would date the whole window at the minute it was fetched, and the
     * StoreAndForward `text` variant carries no timestamp to use instead.
     */
    MESH_TEST_FAIL_IF(broadcast->rx_time != 0U,
                      "a replayed message must not be dated by the replay's arrival");
    const struct mesh_message *direct = mesh_message_log_at(log, 1U);
    /* ROUTER_TEXT_DIRECT is how the router says it was addressed to us; `to` on the replay
       packet is us either way, so it cannot be read from there. */
    MESH_TEST_FAIL_IF(direct->to != SF_US, "a replayed direct message should be addressed to us");
    /* Nothing is waiting on an ack for something that happened hours ago. */
    MESH_TEST_FAIL_IF(direct->ack != MESH_MESSAGE_ACK_NONE || direct->has_hops_away ||
                          direct->rx_snr != 0.0F,
                      "a replayed message should carry no delivery state, hop count or SNR");

    MESH_TEST_FAIL_IF(state->received != 2U || state->stored != 2U ||
                          state->state != MESH_STORE_FORWARD_DONE,
                      "the replay should have finished with both messages stored");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_replay_skips_what_we_already_had, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    /* Heard live, the ordinary way. */
    const char *said = "back in ten";
    meshtastic_FromRadio live = meshtastic_FromRadio_init_default;
    live.which_payload_variant = meshtastic_FromRadio_packet_tag;
    live.packet.from = SF_TALKER;
    live.packet.to = MESH_MESSAGE_BROADCAST_ADDR;
    live.packet.id = 0x9001U;
    live.packet.channel = 1U;
    live.packet.has_rx_time = true;
    live.packet.rx_time = SF_WHEN - 600U;
    live.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    live.packet.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    memcpy(live.packet.decoded.payload.bytes, said, strlen(said));
    live.packet.decoded.payload.size = (pb_size_t)strlen(said);
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &live), "encode live failed");
    MESH_TEST_FAIL_IF(mesh_session_messages(&session)->count != 1U, "the live message is missing");

    meshtastic_StoreAndForward history = sf_history(2U, 0U);
    MESH_TEST_FAIL_IF(!sf_feed(&session, &history, SF_ROUTER, 0U, SF_WHEN), "encode history");

    /* The router replays its whole window, which for a client that was only briefly off is
       mostly traffic it already heard. The copy carries a different packet id - it is inside
       the router's own packet - so nothing but the content can tell it is the same message. */
    meshtastic_StoreAndForward again = sf_text(said, true);
    meshtastic_StoreAndForward fresh = sf_text("missed this one", true);
    /*
     * The replay packets are stamped *now*: our own radio marks when they landed, hours after
     * the messages inside them were said. That is the real shape of the thing, and it is why
     * matching on the stamp cannot work - the live copy above reads SF_WHEN - 600 and its own
     * replay reads SF_WHEN, so a de-duplication that compared them would call one message two
     * and append the router's whole window on top of the copies it duplicates.
     */
    MESH_TEST_FAIL_IF(!sf_feed(&session, &again, SF_TALKER, 1U, SF_WHEN) ||
                          !sf_feed(&session, &fresh, SF_TALKER, 1U, SF_WHEN),
                      "encode replayed texts failed");

    MESH_TEST_FAIL_IF(mesh_session_messages(&session)->count != 2U,
                      "the message we already had should not be in the log twice");
    const struct mesh_store_forward *state = mesh_session_store_forward(&session);
    /* Two numbers, and they are not the same fact: the router did two messages of work and the
       user gained one. A row saying "2 messages" here would be describing the router. */
    MESH_TEST_FAIL_IF(state->received != 2U || state->stored != 1U,
                      "the replay should count what arrived and what was new separately");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_replay_without_an_announcement, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    /* The ROUTER_HISTORY is one packet on a lossy band and the messages are the payload. A
       replay whose announcement went missing is still a replay. */
    meshtastic_StoreAndForward text = sf_text("did you get this", true);
    MESH_TEST_FAIL_IF(!sf_feed(&session, &text, SF_TALKER, 0U, SF_WHEN), "encode text failed");

    MESH_TEST_FAIL_IF(mesh_session_messages(&session)->count != 1U,
                      "the message should have been kept anyway");
    const struct mesh_store_forward *state = mesh_session_store_forward(&session);
    MESH_TEST_FAIL_IF(state->state != MESH_STORE_FORWARD_REPLAYING || state->received != 1U,
                      "a text with no announcement should read as a replay in progress");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_empty_window_is_not_a_failure, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    meshtastic_StoreAndForward history = sf_history(0U, 0U);
    MESH_TEST_FAIL_IF(!sf_feed(&session, &history, SF_ROUTER, 0U, SF_WHEN), "encode history");
    MESH_TEST_FAIL_IF(mesh_session_store_forward(&session)->state != MESH_STORE_FORWARD_EMPTY,
                      "a router with nothing for us should report nothing missed");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_router_refusals, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    meshtastic_StoreAndForward busy = meshtastic_StoreAndForward_init_default;
    busy.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_BUSY;
    MESH_TEST_FAIL_IF(!sf_feed(&session, &busy, SF_ROUTER, 0U, SF_WHEN), "encode busy failed");
    MESH_TEST_FAIL_IF(mesh_session_store_forward(&session)->state != MESH_STORE_FORWARD_BUSY,
                      "a busy router should say so rather than look like a failure");
    /* Busy is not running, so the row can be pressed again - which is the whole difference
       between it and a timeout. */
    MESH_TEST_FAIL_IF(mesh_session_request_history(&session) != 0,
                      "a busy router should not block the next press");

    meshtastic_StoreAndForward error = meshtastic_StoreAndForward_init_default;
    error.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_ERROR;
    MESH_TEST_FAIL_IF(!sf_feed(&session, &error, SF_ROUTER, 0U, SF_WHEN), "encode error failed");
    MESH_TEST_FAIL_IF(mesh_session_store_forward(&session)->state != MESH_STORE_FORWARD_FAILED,
                      "a router in error should read as a failed request");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_frames_are_never_messages, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    /* Somebody else's request crossing our radio, and a router's own statistics. Neither is a
       message, and the port is claimed so neither can become one. */
    meshtastic_StoreAndForward theirs = meshtastic_StoreAndForward_init_default;
    theirs.rr = meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY;
    meshtastic_StoreAndForward stats = meshtastic_StoreAndForward_init_default;
    stats.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS;
    stats.which_variant = meshtastic_StoreAndForward_stats_tag;
    stats.variant.stats.messages_saved = 120U;
    stats.variant.stats.messages_max = 200U;
    MESH_TEST_FAIL_IF(!sf_feed(&session, &theirs, SF_TALKER, 0U, SF_WHEN) ||
                          !sf_feed(&session, &stats, SF_ROUTER, 0U, SF_WHEN),
                      "encode frames failed");

    MESH_TEST_FAIL_IF(mesh_session_messages(&session)->count != 0U,
                      "a Store & Forward frame should never reach the message log");
    const struct mesh_store_forward *state = mesh_session_store_forward(&session);
    MESH_TEST_FAIL_IF(!state->has_stats || state->messages_saved != 120U ||
                          state->messages_max != 200U,
                      "a router's statistics should be kept when it volunteers them");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_a_second_router_is_a_clean_slate, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    /* Router A answers a request and leaves a cursor and its own statistics behind. */
    meshtastic_StoreAndForward beat = meshtastic_StoreAndForward_init_default;
    beat.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT;
    beat.which_variant = meshtastic_StoreAndForward_heartbeat_tag;
    beat.variant.heartbeat.secondary = 1U;
    meshtastic_StoreAndForward history = sf_history(0U, 77U);
    meshtastic_StoreAndForward stats = meshtastic_StoreAndForward_init_default;
    stats.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS;
    stats.which_variant = meshtastic_StoreAndForward_stats_tag;
    stats.variant.stats.messages_saved = 120U;
    MESH_TEST_FAIL_IF(!sf_feed(&session, &beat, SF_ROUTER, 0U, SF_WHEN) ||
                          !sf_feed(&session, &history, SF_ROUTER, 0U, SF_WHEN) ||
                          !sf_feed(&session, &stats, SF_ROUTER, 0U, SF_WHEN),
                      "encode router A frames failed");
    const struct mesh_store_forward *state = mesh_session_store_forward(&session);
    MESH_TEST_FAIL_IF(state->cursor != 77U || !state->router_secondary || !state->has_stats,
                      "router A's cursor, rank and statistics should have been kept");

    /* Then router B announces itself. Nothing of A's is true of B. */
    meshtastic_StoreAndForward other = meshtastic_StoreAndForward_init_default;
    other.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT;
    MESH_TEST_FAIL_IF(!sf_feed(&session, &other, SF_ROUTER_B, 5U, SF_WHEN),
                      "encode router B heartbeat failed");

    MESH_TEST_FAIL_IF(state->router != SF_ROUTER_B || state->router_channel != 5U,
                      "the newest announcement should win while nothing is running");
    /*
     * The cursor is the sharp one. The .proto calls it an index into *the server's* packet
     * history, so sending A's to B asks B to skip to a position in a table it does not have -
     * it would silently miss messages, which is this feature failing in the one direction
     * nothing on the screen could show.
     */
    MESH_TEST_FAIL_IF(state->cursor != 0U, "one router's cursor must not be sent to another");
    MESH_TEST_FAIL_IF(state->router_secondary || state->has_stats,
                      "one router's rank and statistics must not be shown under another's name");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_only_the_router_we_asked_may_answer, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    meshtastic_StoreAndForward beat = meshtastic_StoreAndForward_init_default;
    beat.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT;
    MESH_TEST_FAIL_IF(!sf_feed(&session, &beat, SF_ROUTER, 0U, SF_WHEN), "encode heartbeat failed");
    MESH_TEST_FAIL_IF(mesh_session_request_history(&session) != 0, "the request should have gone");

    meshtastic_StoreAndForward history = sf_history(3U, 5U);
    MESH_TEST_FAIL_IF(!sf_feed(&session, &history, SF_ROUTER, 0U, SF_WHEN), "encode history");
    const struct mesh_store_forward *state = mesh_session_store_forward(&session);
    MESH_TEST_FAIL_IF(state->expected != 3U || state->state != MESH_STORE_FORWARD_REPLAYING,
                      "the router we asked should have started the replay");

    /*
     * Now a second router talks over it with its own count and its own heartbeat. Every arm of
     * the ingest writes the state the running request is filling, so without a guard this resets
     * the count we are part way through and swaps the router mid-replay. On a mesh with one
     * router it never happens, which is exactly why it would have been found late.
     */
    meshtastic_StoreAndForward theirs = sf_history(99U, 1234U);
    meshtastic_StoreAndForward their_beat = meshtastic_StoreAndForward_init_default;
    their_beat.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT;
    MESH_TEST_FAIL_IF(!sf_feed(&session, &theirs, SF_ROUTER_B, 0U, SF_WHEN) ||
                          !sf_feed(&session, &their_beat, SF_ROUTER_B, 0U, SF_WHEN),
                      "encode router B frames failed");

    MESH_TEST_FAIL_IF(state->router != SF_ROUTER, "the router we asked should still be answering");
    MESH_TEST_FAIL_IF(state->expected != 3U || state->cursor != 5U,
                      "another router's answer must not overwrite the running one");
    MESH_TEST_FAIL_IF(state->received != 0U,
                      "another router's announcement must not be counted as our replay");

    /*
     * A replayed *message* is the deliberate exception, and the guard cannot cover it: the
     * envelope of a ROUTER_TEXT_* frame names the original sender rather than the router that
     * relayed it, so there is nothing on it to compare. Gating messages the same way would
     * reject every one the feature exists to collect; two routers replaying at once merging
     * into one count is the far milder failure, and the log de-duplicates what overlaps.
     */
    meshtastic_StoreAndForward ours = sf_text("one of ours", true);
    MESH_TEST_FAIL_IF(!sf_feed(&session, &ours, SF_TALKER, 0U, SF_WHEN), "encode text failed");
    MESH_TEST_FAIL_IF(state->received != 1U || mesh_session_messages(&session)->count != 1U,
                      "the active router's replay should still land");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_replay_does_not_touch_the_sender, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    /* A node heard long ago, over a link that was measured then. */
    meshtastic_FromRadio node = meshtastic_FromRadio_init_default;
    node.which_payload_variant = meshtastic_FromRadio_packet_tag;
    node.packet.from = SF_TALKER;
    node.packet.to = MESH_MESSAGE_BROADCAST_ADDR;
    node.packet.id = 0x7001U;
    node.packet.has_rx_time = true;
    node.packet.rx_time = SF_WHEN - 86400U;
    node.packet.rx_snr = -9.0F;
    node.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    node.packet.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    memcpy(node.packet.decoded.payload.bytes, "old", 3);
    node.packet.decoded.payload.size = 3U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &node), "encode node failed");

    const struct mesh_node_summary *before = mesh_test_session_find_node(&session, SF_TALKER);
    MESH_TEST_FAIL_IF(before == NULL, "the node should be in the roster");
    const uint32_t heard_before = before->last_heard;
    const float snr_before = before->snr;

    /*
     * Now the router replays something that node said. The envelope names the sender, but the
     * packet came one hop from the *router* - so its SNR and its arrival measure a link to the
     * router, not to the sender. Filing them under the sender would make asking for history
     * quietly report every node in the window as freshly reachable.
     */
    meshtastic_StoreAndForward text = sf_text("said a day ago", true);
    MESH_TEST_FAIL_IF(!sf_feed(&session, &text, SF_TALKER, 0U, SF_WHEN), "encode replay failed");
    MESH_TEST_FAIL_IF(mesh_session_messages(&session)->count != 2U, "the replay should be kept");

    const struct mesh_node_summary *after = mesh_test_session_find_node(&session, SF_TALKER);
    MESH_TEST_FAIL_IF(after == NULL || after->last_heard != heard_before ||
                          after->snr != snr_before,
                      "a replay must not refresh the sender's link measurements");

    /* A router's own frame is the opposite case: that node really did just send us a packet. */
    meshtastic_StoreAndForward beat = meshtastic_StoreAndForward_init_default;
    beat.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT;
    MESH_TEST_FAIL_IF(!sf_feed(&session, &beat, SF_ROUTER, 0U, SF_WHEN), "encode heartbeat failed");
    MESH_TEST_FAIL_IF(mesh_test_session_find_node(&session, SF_ROUTER) == NULL,
                      "a heartbeat should still put its router in the roster");

    record_success(test_name);
}

/* ---- the silences -------------------------------------------------------------------------- */

MESH_TEST_CASE(store_forward_timeouts_tell_the_two_silences_apart, unit) {
    /* A ping nobody answered and a router that never replied are the same silence from two
       different places, and only one of them is worth pressing again. */
    struct mesh_store_forward sf;
    mesh_store_forward_reset(&sf);
    mesh_store_forward_sent(&sf, MESH_MESSAGE_BROADCAST_ADDR, 0U, 0U, true);
    MESH_TEST_FAIL_IF(mesh_store_forward_tick(&sf, MESH_STORE_FORWARD_SEEK_TIMEOUT_MS - 1U),
                      "the ping should still be waiting");
    MESH_TEST_FAIL_IF(!mesh_store_forward_tick(&sf, MESH_STORE_FORWARD_SEEK_TIMEOUT_MS) ||
                          sf.state != MESH_STORE_FORWARD_NO_ROUTER,
                      "a ping nobody answered should report that there is no router");

    mesh_store_forward_reset(&sf);
    mesh_store_forward_sent(&sf, SF_ROUTER, 0U, 0U, false);
    sf.cursor = 9U;
    MESH_TEST_FAIL_IF(!mesh_store_forward_tick(&sf, MESH_STORE_FORWARD_REQUEST_TIMEOUT_MS) ||
                          sf.state != MESH_STORE_FORWARD_TIMEOUT,
                      "a router that was asked and said nothing should read as a timeout");
    /* And it is forgotten. Keeping it would send every press from here to the same silence
       instead of going looking for a router that is actually there. */
    MESH_TEST_FAIL_IF(sf.router != 0U || sf.cursor != 0U,
                      "a silent router should not be remembered");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_replay_that_stops_arriving, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    meshtastic_StoreAndForward history = sf_history(30U, 0U);
    meshtastic_StoreAndForward text = sf_text("one of thirty", true);
    MESH_TEST_FAIL_IF(!sf_feed(&session, &history, SF_ROUTER, 0U, SF_WHEN) ||
                          !sf_feed(&session, &text, SF_TALKER, 0U, SF_WHEN),
                      "encode frames failed");

    /*
     * The router paces the replay itself, so a gap is what ends one - not a deadline. Twenty
     * nine messages short is still a replay that delivered one, and the one it delivered is in
     * the log.
     *
     * The gap is measured from the real monotonic clock, because that is what the ingest above
     * stamped the last arrival with; a tick handed a small number would be a tick from before
     * the packet landed.
     */
    mesh_session_tick(&session, mesh_time_monotonic_ms() + MESH_STORE_FORWARD_REPLAY_GAP_MS + 1U);
    const struct mesh_store_forward *state = mesh_session_store_forward(&session);
    MESH_TEST_FAIL_IF(state->state != MESH_STORE_FORWARD_DONE || state->received != 1U ||
                          state->stored != 1U,
                      "a replay that stopped should report what it managed");
    MESH_TEST_FAIL_IF(mesh_session_messages(&session)->count != 1U,
                      "the message that did arrive should have been kept");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_router_does_not_survive_the_radio, unit) {
    struct mesh_session session;
    struct mesh_test_trace_capture capture;
    sf_open_session(&session, &capture);

    meshtastic_StoreAndForward beat = meshtastic_StoreAndForward_init_default;
    beat.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT;
    MESH_TEST_FAIL_IF(!sf_feed(&session, &beat, SF_ROUTER, 2U, SF_WHEN), "encode heartbeat failed");
    MESH_TEST_FAIL_IF(mesh_session_store_forward(&session)->router != SF_ROUTER,
                      "the router should have been learned");

    /*
     * The router is a node number *and a channel index*, and a channel index only names a
     * channel against the table of the radio we are attached to. The cursor goes with it: it
     * indexes a table inside a router we are no longer sure we are about to talk to.
     */
    mesh_session_detach(&session);
    const struct mesh_store_forward *state = mesh_session_store_forward(&session);
    MESH_TEST_FAIL_IF(state->router != 0U || state->router_channel != 0U || state->cursor != 0U,
                      "a dropped link should take the router with it");
    /* The messages it fetched stay, because they are things that happened. */
    MESH_TEST_FAIL_IF(mesh_session_messages(&session)->count != 0U, "no messages were expected");

    record_success(test_name);
}

/* ---- the rows ------------------------------------------------------------------------------ */

/* The Store & Forward section, with a given request state layered on. */
static uint32_t sf_rows(const struct mesh_ui_store_forward *sf, bool link_up,
                        struct mesh_ui_settings_item *out, uint32_t max) {
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.has_store_forward = true;
    settings.store_forward = *sf;
    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.link_up = link_up;
    return mesh_ui_settings_items(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_STORE_FORWARD,
                                  MESH_UI_SETTINGS_NO_CHANNEL, out, max);
}

/* Finds the row with this label, or NULL. */
static const struct mesh_ui_settings_item *sf_row(const struct mesh_ui_settings_item *items,
                                                  uint32_t count, const char *label) {
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(items[i].label, label) == 0) {
            return &items[i];
        }
    }
    return NULL;
}

MESH_TEST_CASE(store_forward_rows_say_who_would_answer, unit) {
    struct mesh_ui_settings_item items[48];
    struct mesh_ui_store_forward sf;
    memset(&sf, 0, sizeof sf);

    uint32_t count = sf_rows(&sf, true, items, (uint32_t)(sizeof items / sizeof items[0]));
    const struct mesh_ui_settings_item *router = sf_row(items, count, "Router");
    /* Listed before the press rather than only after one: "none heard yet" is the answer to
       why the press is about to take half a minute. */
    MESH_TEST_FAIL_IF(router == NULL || strcmp(router->value, "none heard yet") != 0,
                      "with no router the row should say so");
    const struct mesh_ui_settings_item *ask = sf_row(items, count, "Get missed messages");
    MESH_TEST_FAIL_IF(ask == NULL || ask->kind != MESH_UI_SETTING_ACTION ||
                          ask->number != MESH_UI_SETTINGS_ACTION_REQUEST_HISTORY,
                      "the request row should be a pressable action");
    /* Nothing has been asked for, so there is nothing for a "last request" row to report. */
    MESH_TEST_FAIL_IF(sf_row(items, count, "Last request") != NULL,
                      "there should be no result row before the first press");

    /* With no link the row says why it cannot be pressed rather than disappearing: a section
       whose length changes when the radio drops moves the cursor out from under the user. */
    count = sf_rows(&sf, false, items, (uint32_t)(sizeof items / sizeof items[0]));
    ask = sf_row(items, count, "Get missed messages");
    MESH_TEST_FAIL_IF(ask == NULL || ask->kind != MESH_UI_SETTING_INFO ||
                          strcmp(ask->value, "not connected") != 0,
                      "without a link the request row should say why");

    sf.router = SF_ROUTER;
    sf.router_secondary = true;
    mesh_str_copy(sf.router_name, sizeof sf.router_name, "Hilltop");
    count = sf_rows(&sf, true, items, (uint32_t)(sizeof items / sizeof items[0]));
    router = sf_row(items, count, "Router");
    /* A secondary router is still a router - on a mesh whose primary is off it is the only
       one - so the row names it and adds the caveat rather than refusing it. */
    MESH_TEST_FAIL_IF(router == NULL || strcmp(router->value, "Hilltop (secondary)") != 0,
                      "a secondary router should be named with its rank");

    record_success(test_name);
}

MESH_TEST_CASE(store_forward_rows_say_what_the_request_did, unit) {
    struct mesh_ui_settings_item items[48];
    struct mesh_ui_store_forward sf;
    memset(&sf, 0, sizeof sf);
    sf.router = SF_ROUTER;
    mesh_str_copy(sf.router_name, sizeof sf.router_name, "Hilltop");

    /* Running: the action row becomes the progress reading, because the session refuses a
       second press anyway and a row that vanished mid-replay would move the rows under it. */
    sf.state = (uint8_t)MESH_STORE_FORWARD_REPLAYING;
    sf.expected = 30U;
    sf.received = 4U;
    uint32_t count = sf_rows(&sf, true, items, (uint32_t)(sizeof items / sizeof items[0]));
    const struct mesh_ui_settings_item *ask = sf_row(items, count, "Get missed messages");
    MESH_TEST_FAIL_IF(ask == NULL || ask->kind != MESH_UI_SETTING_INFO ||
                          strcmp(ask->value, "4 of 30") != 0,
                      "a running replay should be reported on the row that started it");

    /* Finished: what the router sent, and what of it was new. */
    sf.state = (uint8_t)MESH_STORE_FORWARD_DONE;
    sf.received = 30U;
    sf.stored = 3U;
    count = sf_rows(&sf, true, items, (uint32_t)(sizeof items / sizeof items[0]));
    const struct mesh_ui_settings_item *last = sf_row(items, count, "Last request");
    MESH_TEST_FAIL_IF(last == NULL || strcmp(last->value, "3 new of 30") != 0,
                      "a finished replay should count what was added and what arrived");
    MESH_TEST_FAIL_IF(sf_row(items, count, "Get missed messages") == NULL ||
                          sf_row(items, count, "Get missed messages")->kind !=
                              MESH_UI_SETTING_ACTION,
                      "a finished request should leave the row pressable again");

    /* A window of four hours replayed at a client that was off for ten minutes is mostly
       traffic it already has, so "30 messages" would be describing the router's work. */
    sf.stored = 0U;
    count = sf_rows(&sf, true, items, (uint32_t)(sizeof items / sizeof items[0]));
    last = sf_row(items, count, "Last request");
    MESH_TEST_FAIL_IF(last == NULL || strcmp(last->value, "30 replayed, none new") != 0,
                      "a replay that added nothing should say so");

    sf.state = (uint8_t)MESH_STORE_FORWARD_NO_ROUTER;
    count = sf_rows(&sf, true, items, (uint32_t)(sizeof items / sizeof items[0]));
    last = sf_row(items, count, "Last request");
    MESH_TEST_FAIL_IF(last == NULL || strcmp(last->value, "no router answered") != 0,
                      "a ping nobody answered should be reported as such");

    record_success(test_name);
}
