#define _POSIX_C_SOURCE 200809L

/* The session's node cache, stats, traceroute and node actions. */

#include "framework/mesh_test.h"
#include "support/session_fixture.h"

#include "mesh/core/app.h"
#include "mesh/core/message.h"
#include "mesh/core/session.h"
#include "mesh/ui/store.h"
#include "mesh/utils/text.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/config.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"
#include "meshtastic/telemetry.pb.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Node names are radio input like message bodies are: whoever owns the node picks the bytes.
   They have to arrive sanitised and whole, because the field they land in is smaller than the
   one they came from. */
MESH_TEST_CASE(session_node_names_sanitised, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    struct {
        const char *label;
        uint32_t node_id;
        const char *long_name;
        const char *short_name;
        const char *expect_long;
        const char *expect_short;
    } cases[] = {
        /* The common case: an emoji short name, which is exactly what char[5] is sized for. */
        {"emoji short name", 0x1001U, "\xF0\x9F\x8C\xB2 Pine Ridge", "\xF0\x9F\x8C\xB2",
         "\xF0\x9F\x8C\xB2 Pine Ridge", "\xF0\x9F\x8C\xB2"},
        {"accented", 0x1002U, "Jos\xC3\xA9 Rep\xC3\xAAter", "J\xC3\xA9",
         "Jos\xC3\xA9 Rep\xC3\xAAter", "J\xC3\xA9"},
        /* Malformed bytes are replaced rather than copied on to the framebuffer and JSON. */
        {"invalid utf-8", 0x1003U, "bad\xFFname", "\x80\x80", "bad?name", "??"},
        /* Control bytes fold away. */
        {"control bytes", 0x1004U, "line\nbreak", "a\x01", "line break", "a?"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
        from_radio.which_payload_variant = meshtastic_FromRadio_node_info_tag;
        from_radio.node_info.num = cases[i].node_id;
        from_radio.node_info.has_user = true;
        snprintf(from_radio.node_info.user.long_name, sizeof from_radio.node_info.user.long_name,
                 "%s", cases[i].long_name);
        snprintf(from_radio.node_info.user.short_name, sizeof from_radio.node_info.user.short_name,
                 "%s", cases[i].short_name);

        uint8_t buffer[256];
        pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof buffer);
        MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_FromRadio_fields, &from_radio),
                          "encode node_info failed");
        mesh_session_handle_from_radio(&session, buffer, stream.bytes_written);

        const struct mesh_node_summary *summary = NULL;
        for (size_t n = 0; n < session.handshake.node_count; ++n) {
            if (session.handshake.nodes[n].node_id == cases[i].node_id) {
                summary = &session.handshake.nodes[n];
                break;
            }
        }
        MESH_TEST_FAIL_IF(summary == NULL, cases[i].label);
        MESH_TEST_FAIL_IF(strcmp(summary->long_name, cases[i].expect_long) != 0 ||
                              strcmp(summary->short_name, cases[i].expect_short) != 0,
                          cases[i].label);
    }

    /* A long name that does not fit the cache field is cut on a character boundary, so the
       tail is never half a sequence. Four emoji is sixteen bytes; the field holds 39 plus a
       NUL, so pack it past the edge. */
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    from_radio.node_info.num = 0x2001U;
    from_radio.node_info.has_user = true;
    for (int i = 0; i < 9; ++i) {
        strcat(from_radio.node_info.user.long_name, "\xF0\x9F\x8C\xB2");
    }
    strcat(from_radio.node_info.user.long_name, "xyz");

    uint8_t buffer[256];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof buffer);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_FromRadio_fields, &from_radio),
                      "encode long node_info failed");
    mesh_session_handle_from_radio(&session, buffer, stream.bytes_written);

    const struct mesh_node_summary *summary = NULL;
    for (size_t n = 0; n < session.handshake.node_count; ++n) {
        if (session.handshake.nodes[n].node_id == 0x2001U) {
            summary = &session.handshake.nodes[n];
            break;
        }
    }
    if (summary == NULL) {
        record_failure(test_name, "the long-named node was not cached");
        return;
    }
    /* Whatever survived has to be well-formed all the way to the NUL. */
    size_t offset = 0U;
    while (summary->long_name[offset] != '\0') {
        const size_t step = mesh_text_utf8_sequence_len(
            (const uint8_t *)&summary->long_name[offset], strlen(&summary->long_name[offset]));
        if (step == 0U) {
            record_failure(test_name, "truncation left a half character in the cache");
            return;
        }
        offset += step;
    }

    record_success(test_name);
}

/*
 * The node record beyond a name: what the NodeDB sync carries, and the three app payloads that
 * keep it current once the sync is over. The firmware replays its database exactly once per
 * connection, so the packet paths are the only thing standing between the detail screen and a
 * view that is frozen at connect time.
 */
MESH_TEST_CASE(session_node_detail_ingest, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    /* The sync: identity, a fix and a battery reading all in one NodeInfo. */
    meshtastic_FromRadio sync = meshtastic_FromRadio_init_default;
    sync.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    sync.node_info.num = 0x4001U;
    sync.node_info.last_heard = 1749990000U;
    sync.node_info.snr = 6.25f;
    sync.node_info.channel = 2U;
    sync.node_info.is_favorite = true;
    sync.node_info.has_user = true;
    snprintf(sync.node_info.user.id, sizeof sync.node_info.user.id, "!00004001");
    snprintf(sync.node_info.user.long_name, sizeof sync.node_info.user.long_name, "Ridge Repeater");
    snprintf(sync.node_info.user.short_name, sizeof sync.node_info.user.short_name, "RDG");
    sync.node_info.user.hw_model = meshtastic_HardwareModel_RAK4631;
    sync.node_info.user.role = meshtastic_Config_DeviceConfig_Role_ROUTER;
    sync.node_info.user.public_key.size = 32U;
    memset(sync.node_info.user.public_key.bytes, 0xAB, 32U);
    sync.node_info.has_position = true;
    sync.node_info.position.has_latitude_i = true;
    sync.node_info.position.latitude_i = 447654321;
    sync.node_info.position.has_longitude_i = true;
    sync.node_info.position.longitude_i = -680012345;
    sync.node_info.position.has_altitude = true;
    sync.node_info.position.altitude = 312;
    sync.node_info.position.sats_in_view = 9U;
    sync.node_info.has_device_metrics = true;
    sync.node_info.device_metrics.has_battery_level = true;
    sync.node_info.device_metrics.battery_level = 76U;
    sync.node_info.device_metrics.has_voltage = true;
    sync.node_info.device_metrics.voltage = 3.94f;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &sync),
                      "encode node_info failed");

    const struct mesh_node_summary *node = mesh_test_session_find_node(&session, 0x4001U);
    MESH_TEST_FAIL_IF(node == NULL, "the node was not cached");
    MESH_TEST_FAIL_IF(strcmp(node->user_id, "!00004001") != 0 ||
                          node->hw_model != (uint32_t)meshtastic_HardwareModel_RAK4631 ||
                          node->role != (uint32_t)meshtastic_Config_DeviceConfig_Role_ROUTER ||
                          node->public_key_len != 32U || node->public_key[0] != 0xABU ||
                          !node->is_favorite || node->channel != 2U,
                      "NodeInfo identity was not kept");
    MESH_TEST_FAIL_IF(!node->position.valid || node->position.latitude_i != 447654321 ||
                          node->position.longitude_i != -680012345 ||
                          !node->position.has_altitude || node->position.altitude != 312 ||
                          node->position.sats_in_view != 9U,
                      "NodeInfo position was not kept");
    MESH_TEST_FAIL_IF(!node->metrics.valid || !node->metrics.has_battery ||
                          node->metrics.battery_level != 76U || !node->metrics.has_voltage,
                      "NodeInfo device metrics were not kept");

    /* A POSITION_APP broadcast moves it. */
    meshtastic_Position position = meshtastic_Position_init_default;
    position.has_latitude_i = true;
    position.latitude_i = 447000000;
    position.has_longitude_i = true;
    position.longitude_i = -680000000;
    position.sats_in_view = 11U;
    uint8_t payload[256];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Position_fields, &position) ||
                          !mesh_test_session_feed_app_packet(&session, 0x4001U,
                                                             meshtastic_PortNum_POSITION_APP,
                                                             payload, stream.bytes_written),
                      "encode POSITION_APP failed");
    node = mesh_test_session_find_node(&session, 0x4001U);
    MESH_TEST_FAIL_IF(node->position.latitude_i != 447000000 || node->position.sats_in_view != 11U,
                      "POSITION_APP did not move the node");

    /* Environment telemetry: a reading the NodeDB never carries at all. */
    meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_default;
    telemetry.time = 1750000000U;
    telemetry.which_variant = meshtastic_Telemetry_environment_metrics_tag;
    telemetry.variant.environment_metrics.has_temperature = true;
    telemetry.variant.environment_metrics.temperature = 21.5f;
    telemetry.variant.environment_metrics.has_relative_humidity = true;
    telemetry.variant.environment_metrics.relative_humidity = 48.0f;
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Telemetry_fields, &telemetry) ||
                          !mesh_test_session_feed_app_packet(&session, 0x4001U,
                                                             meshtastic_PortNum_TELEMETRY_APP,
                                                             payload, stream.bytes_written),
                      "encode TELEMETRY_APP failed");
    node = mesh_test_session_find_node(&session, 0x4001U);
    MESH_TEST_FAIL_IF(!node->environment.valid || !node->environment.has_temperature ||
                          node->environment.temperature < 21.4f ||
                          node->environment.temperature > 21.6f,
                      "TELEMETRY_APP environment was not kept");

    /* A node that joins after the sync introduces itself over the air; without NODEINFO_APP it
       would sit in the list as a bare id forever. */
    meshtastic_User user = meshtastic_User_init_default;
    snprintf(user.id, sizeof user.id, "!00004002");
    snprintf(user.long_name, sizeof user.long_name, "Late Joiner");
    snprintf(user.short_name, sizeof user.short_name, "LATE");
    user.hw_model = meshtastic_HardwareModel_T_ECHO;
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_User_fields, &user) ||
                          !mesh_test_session_feed_app_packet(&session, 0x4002U,
                                                             meshtastic_PortNum_NODEINFO_APP,
                                                             payload, stream.bytes_written),
                      "encode NODEINFO_APP failed");
    const struct mesh_node_summary *joiner = mesh_test_session_find_node(&session, 0x4002U);
    MESH_TEST_FAIL_IF(joiner == NULL || strcmp(joiner->long_name, "Late Joiner") != 0 ||
                          strcmp(joiner->user_id, "!00004002") != 0 ||
                          joiner->hw_model != (uint32_t)meshtastic_HardwareModel_T_ECHO,
                      "NODEINFO_APP did not name the node");

    /* A second sync must not wipe what only the air ever told us. The radio's NodeDB has no
       environment telemetry to replace it with, so a naive rebuild would empty the section. */
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &sync),
                      "re-encode node_info failed");
    node = mesh_test_session_find_node(&session, 0x4001U);
    MESH_TEST_FAIL_IF(!node->environment.valid || !node->environment.has_temperature,
                      "a NodeDB resync wiped the environment telemetry");

    record_success(test_name);
}

/*
 * LocalStats: the radio's own report about the mesh. It is the one telemetry that belongs to
 * the session rather than to a node, and it only counts when it comes from our own node - the
 * firmware sends it to the attached client alone, so anything else wearing that variant is a
 * peer we should not be reading our own packet counters out of.
 */
MESH_TEST_CASE(session_local_stats, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    MESH_TEST_FAIL_IF(mesh_session_radio_stats(&session)->valid,
                      "stats claimed to be valid before any report");

    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = 0x7001U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &my_info),
                      "encode my_info failed");

    meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_default;
    telemetry.time = 1750000000U;
    telemetry.which_variant = meshtastic_Telemetry_local_stats_tag;
    telemetry.variant.local_stats.uptime_seconds = 90061U;
    telemetry.variant.local_stats.channel_utilization = 12.5f;
    telemetry.variant.local_stats.air_util_tx = 1.75f;
    telemetry.variant.local_stats.num_packets_tx = 412U;
    telemetry.variant.local_stats.num_packets_rx = 8210U;
    telemetry.variant.local_stats.num_packets_rx_bad = 23U;
    telemetry.variant.local_stats.num_rx_dupe = 114U;
    telemetry.variant.local_stats.num_tx_relay = 96U;
    telemetry.variant.local_stats.num_tx_dropped = 2U;
    telemetry.variant.local_stats.num_online_nodes = 37U;
    telemetry.variant.local_stats.num_total_nodes = 132U;
    telemetry.variant.local_stats.heap_total_bytes = 200704U;
    telemetry.variant.local_stats.heap_free_bytes = 63488U;
    telemetry.variant.local_stats.noise_floor = -98;

    uint8_t payload[256];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Telemetry_fields, &telemetry),
                      "encode local stats failed");
    const size_t payload_len = stream.bytes_written;

    /* A peer's packet wearing the same variant must not become our counters. */
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_app_packet(&session, 0x7002U,
                                                         meshtastic_PortNum_TELEMETRY_APP, payload,
                                                         payload_len),
                      "feed peer local stats failed");
    MESH_TEST_FAIL_IF(mesh_session_radio_stats(&session)->valid,
                      "a peer's LocalStats was taken as the radio's own");

    MESH_TEST_FAIL_IF(!mesh_test_session_feed_app_packet(&session, 0x7001U,
                                                         meshtastic_PortNum_TELEMETRY_APP, payload,
                                                         payload_len),
                      "feed local stats failed");

    const struct mesh_radio_stats *stats = mesh_session_radio_stats(&session);
    MESH_TEST_FAIL_IF(!stats->valid || stats->uptime_seconds != 90061U ||
                          stats->num_packets_tx != 412U || stats->num_packets_rx != 8210U ||
                          stats->num_packets_rx_bad != 23U || stats->num_rx_dupe != 114U ||
                          stats->num_tx_relay != 96U || stats->num_tx_dropped != 2U ||
                          stats->num_online_nodes != 37U || stats->num_total_nodes != 132U,
                      "LocalStats counters were not kept");
    MESH_TEST_FAIL_IF(!stats->has_heap || stats->heap_free_bytes != 63488U ||
                          !stats->has_noise_floor || stats->noise_floor != -98,
                      "LocalStats heap or noise floor was not kept");
    MESH_TEST_FAIL_IF(stats->channel_utilization < 12.4f || stats->channel_utilization > 12.6f,
                      "LocalStats channel utilization was not kept");

    /* Zero is a real answer for a counter but not for a heap size or a noise floor, and those
       two are the ones a bare zero would show as a confident reading of nothing. */
    telemetry.variant.local_stats.heap_total_bytes = 0U;
    telemetry.variant.local_stats.noise_floor = 0;
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Telemetry_fields, &telemetry) ||
                          !mesh_test_session_feed_app_packet(&session, 0x7001U,
                                                             meshtastic_PortNum_TELEMETRY_APP,
                                                             payload, stream.bytes_written),
                      "re-encode local stats failed");
    stats = mesh_session_radio_stats(&session);
    MESH_TEST_FAIL_IF(stats->has_heap || stats->has_noise_floor,
                      "an unreported heap or noise floor was shown as a reading");

    /* The stats describe the radio that is connected, so a dropped link must forget them. */
    mesh_session_detach(&session);
    MESH_TEST_FAIL_IF(mesh_session_radio_stats(&session)->valid,
                      "stats survived the link dropping");

    record_success(test_name);
}

/*
 * Traceroute end to end: the question we put on the air, the reply matched to it, and the
 * shape the UI is handed. The last part is the one worth pinning - RouteDiscovery carries the
 * intermediate nodes and a parallel array of link SNRs, and turning that into "each stop and
 * the reading of the link that reached it" is an off-by-one waiting to happen in both
 * directions.
 */
MESH_TEST_CASE(session_traceroute, unit) {
    struct mesh_session session;
    mesh_session_init(&session);
    struct mesh_test_trace_capture capture;
    memset(&capture, 0, sizeof capture);
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &capture);

    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = 0x1111U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &my_info),
                      "encode my_info failed");

    /* Tracing a route to ourselves has no links in it, and a broadcast would ask the whole
       mesh to answer at once. */
    MESH_TEST_FAIL_IF(mesh_session_send_traceroute(&session, 0x1111U) != -EINVAL ||
                          mesh_session_send_traceroute(&session, MESH_MESSAGE_BROADCAST_ADDR) !=
                              -EINVAL,
                      "self and broadcast traces should be refused");

    MESH_TEST_FAIL_IF(mesh_session_send_traceroute(&session, 0x3333U) != 0 || capture.calls != 1U,
                      "the traceroute was not sent");

    meshtastic_ToRadio sent = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(capture.packet, capture.len);
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_ToRadio_fields, &sent) ||
                          sent.which_payload_variant != meshtastic_ToRadio_packet_tag ||
                          sent.packet.to != 0x3333U ||
                          sent.packet.decoded.portnum != meshtastic_PortNum_TRACEROUTE_APP ||
                          !sent.packet.decoded.want_response,
                      "the request should be a TRACEROUTE_APP asking for a reply");
    const uint32_t request_id = sent.packet.id;

    MESH_TEST_FAIL_IF(mesh_session_traceroute(&session)->state != MESH_TRACEROUTE_PENDING,
                      "the trace should be pending after the send");
    /* One at a time: this client's half of the firmware's traceroute rate limit. */
    MESH_TEST_FAIL_IF(mesh_session_send_traceroute(&session, 0x4444U) != -EBUSY ||
                          capture.calls != 1U,
                      "a second trace should be refused while one is running");

    /* One node in the middle each way. snr_towards has one more entry than route: a reading
       per link, us->relay and relay->target. */
    meshtastic_RouteDiscovery route = meshtastic_RouteDiscovery_init_default;
    route.route_count = 1U;
    route.route[0] = 0x2222U;
    route.snr_towards_count = 2U;
    route.snr_towards[0] = 26; /* 6.5 dB */
    route.snr_towards[1] = 16; /* 4.0 dB */
    route.route_back_count = 1U;
    route.route_back[0] = 0x2222U;
    route.snr_back_count = 2U;
    route.snr_back[0] = 20;       /* 5.0 dB */
    route.snr_back[1] = INT8_MIN; /* the firmware's "not measured" */

    uint8_t payload[256];
    pb_ostream_t out = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&out, meshtastic_RouteDiscovery_fields, &route),
                      "encode RouteDiscovery failed");

    /* A reply that quotes a different request is somebody else's trace crossing our radio. */
    meshtastic_FromRadio stray = meshtastic_FromRadio_init_default;
    stray.which_payload_variant = meshtastic_FromRadio_packet_tag;
    stray.packet.from = 0x3333U;
    stray.packet.id = 0x9001U;
    stray.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    stray.packet.decoded.portnum = meshtastic_PortNum_TRACEROUTE_APP;
    stray.packet.decoded.request_id = request_id ^ 0xFFFFU;
    memcpy(stray.packet.decoded.payload.bytes, payload, out.bytes_written);
    stray.packet.decoded.payload.size = (pb_size_t)out.bytes_written;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &stray),
                      "encode stray traceroute failed");
    MESH_TEST_FAIL_IF(mesh_session_traceroute(&session)->state != MESH_TRACEROUTE_PENDING,
                      "another node's trace was taken as our reply");
    /* And it is not a message, however it is addressed. */
    MESH_TEST_FAIL_IF(mesh_session_messages(&session)->count != 0U,
                      "a RouteDiscovery reached the message log");

    meshtastic_FromRadio reply = stray;
    reply.packet.decoded.request_id = request_id;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &reply),
                      "encode traceroute reply failed");

    const struct mesh_traceroute *trace = mesh_session_traceroute(&session);
    MESH_TEST_FAIL_IF(trace->state != MESH_TRACEROUTE_DONE || trace->route_count != 1U ||
                          trace->route[0] != 0x2222U || trace->snr_count != 2U ||
                          trace->snr[0] != 26 || trace->back_count != 1U ||
                          trace->snr_back_count != 2U,
                      "the reply was not kept");

    /* The shape the UI draws: us, the relay, the target - with each stop carrying the reading
       of the link that got the packet to it, and the first stop carrying none. */
    struct mesh_ui_traceroute ui;
    mesh_app_flatten_traceroute(mesh_session_handshake(&session), trace, 0x1111U, &ui);
    MESH_TEST_FAIL_IF(ui.forward_count != 3U || ui.forward[0].node_id != 0x1111U ||
                          ui.forward[1].node_id != 0x2222U || ui.forward[2].node_id != 0x3333U,
                      "the forward path should be us, the relay, then the target");
    MESH_TEST_FAIL_IF(ui.forward[0].has_snr || !ui.forward[1].has_snr ||
                          ui.forward[1].snr_quarter_db != 26 || !ui.forward[2].has_snr ||
                          ui.forward[2].snr_quarter_db != 16,
                      "forward hops carry the wrong link SNR");
    MESH_TEST_FAIL_IF(ui.back_count != 3U || ui.back[0].node_id != 0x3333U ||
                          ui.back[2].node_id != 0x1111U || !ui.back[1].has_snr ||
                          ui.back[1].snr_quarter_db != 20 || ui.back[2].snr_quarter_db != INT8_MIN,
                      "the return path is wrong");

    /* Finished, so the slot is free again - and a dropped link forgets the route entirely. */
    MESH_TEST_FAIL_IF(mesh_session_send_traceroute(&session, 0x4444U) != 0,
                      "a finished trace should not block the next one");
    mesh_session_detach(&session);
    MESH_TEST_FAIL_IF(mesh_session_traceroute(&session)->state != MESH_TRACEROUTE_IDLE,
                      "the trace survived the link dropping");

    record_success(test_name);
}

/*
 * The two Nodes-tab actions that talk to the radio about another node. Ignore is an admin
 * write with no read-back, so the cached flag has to move here or the row would lie until the
 * node's next NodeInfo; asking for a name is a plain packet with want_response.
 */
MESH_TEST_CASE(session_node_actions, unit) {
    struct mesh_session session;
    mesh_session_init(&session);
    struct mesh_test_trace_capture capture;
    memset(&capture, 0, sizeof capture);
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &capture);

    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = 0x1111U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &my_info),
                      "encode my_info failed");

    meshtastic_FromRadio peer = meshtastic_FromRadio_init_default;
    peer.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    peer.node_info.num = 0x2222U;
    peer.node_info.has_user = true;
    snprintf(peer.node_info.user.short_name, sizeof peer.node_info.user.short_name, "NOSY");
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &peer),
                      "encode node_info failed");

    /* Ignoring the radio we talk through would drop our own traffic, and a node we have never
       heard of has no cached flag to move. */
    MESH_TEST_FAIL_IF(mesh_session_set_node_ignored(&session, 0x1111U, true) != -EINVAL ||
                          mesh_session_set_node_ignored(&session, 0x9999U, true) != -ENOENT,
                      "ignoring ourselves or an unknown node should be refused");

    MESH_TEST_FAIL_IF(mesh_session_set_node_ignored(&session, 0x2222U, true) <= 0,
                      "the ignore was not queued");
    const struct mesh_node_summary *node = mesh_test_session_find_node(&session, 0x2222U);
    MESH_TEST_FAIL_IF(node == NULL || !node->is_ignored, "the cached ignore flag did not move");
    /* There is no get_ignored, so asking again for what is already true sends nothing. */
    MESH_TEST_FAIL_IF(mesh_session_set_node_ignored(&session, 0x2222U, true) != 0,
                      "a redundant ignore should send nothing");

    /* Asking a node for its name: our own User out on NODEINFO_APP, wanting a response. */
    capture.calls = 0U;
    MESH_TEST_FAIL_IF(mesh_session_request_node_info(&session, 0x1111U) != -EINVAL ||
                          mesh_session_request_node_info(&session, MESH_MESSAGE_BROADCAST_ADDR) !=
                              -EINVAL,
                      "asking ourselves or everyone should be refused");

    /*
     * Before our own owner record arrives there is nothing truthful to send. A NodeInfo is
     * applied by overwriting the record wholesale, so a placeholder User carrying only an id
     * would blank this node's name and public key on every peer that received it.
     */
    MESH_TEST_FAIL_IF(mesh_session_request_node_info(&session, 0x2222U) != -EAGAIN ||
                          capture.calls != 0U,
                      "a request without our owner record should be refused");

    meshtastic_FromRadio owner = meshtastic_FromRadio_init_default;
    owner.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    owner.node_info.num = 0x1111U;
    owner.node_info.has_user = true;
    snprintf(owner.node_info.user.long_name, sizeof owner.node_info.user.long_name, "Brick");
    snprintf(owner.node_info.user.short_name, sizeof owner.node_info.user.short_name, "BRIK");
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &owner),
                      "encode our own node_info failed");

    MESH_TEST_FAIL_IF(mesh_session_request_node_info(&session, 0x2222U) != 0 || capture.calls != 1U,
                      "the NodeInfo request was not sent");
    meshtastic_ToRadio sent = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(capture.packet, capture.len);
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_ToRadio_fields, &sent) ||
                          sent.which_payload_variant != meshtastic_ToRadio_packet_tag ||
                          sent.packet.to != 0x2222U ||
                          sent.packet.decoded.portnum != meshtastic_PortNum_NODEINFO_APP ||
                          !sent.packet.decoded.want_response,
                      "the request should be a NODEINFO_APP asking for a reply");
    /* And it carries our real name, which is the half of the exchange the far end keeps. */
    meshtastic_User sent_user = meshtastic_User_init_default;
    pb_istream_t user_in =
        pb_istream_from_buffer(sent.packet.decoded.payload.bytes, sent.packet.decoded.payload.size);
    MESH_TEST_FAIL_IF(!pb_decode(&user_in, meshtastic_User_fields, &sent_user) ||
                          strcmp(sent_user.short_name, "BRIK") != 0,
                      "the request should carry our own owner record");

    record_success(test_name);
}
