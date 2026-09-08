#define _POSIX_C_SOURCE 200809L

/* The session's node cache, stats, traceroute and node actions. */

#include "framework/mesh_test.h"
#include "support/proto_fixture.h"
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
 * What a fix is dated by, and what a fix has to be before it is kept at all.
 *
 * Three separate things used to be one: the node's clock, our clock, and "is this a point on
 * Earth". A Position carries `time` (the sender's own clock, which upstream leaves off the
 * mesh to save space and which is therefore usually 0) and `timestamp` (when the GPS actually
 * solved). Neither is when we heard it, and last_heard cannot stand in for either.
 */
MESH_TEST_CASE(session_position_clocks_and_range, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    uint8_t payload[256];
    pb_ostream_t stream;

    /* A first fix, dated only by the sender's own clock. */
    meshtastic_Position position = meshtastic_Position_init_default;
    position.has_latitude_i = true;
    position.latitude_i = 447654321;
    position.has_longitude_i = true;
    position.longitude_i = -680012345;
    position.time = 1749000000U;
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Position_fields, &position) ||
                          !mesh_test_session_feed_app_packet(&session, 0x4001U,
                                                             meshtastic_PortNum_POSITION_APP,
                                                             payload, stream.bytes_written),
                      "encode POSITION_APP failed");
    const struct mesh_node_summary *node = mesh_test_session_find_node(&session, 0x4001U);
    MESH_TEST_FAIL_IF(node == NULL || !node->position.valid, "the fix was not kept");
    MESH_TEST_FAIL_IF(node->position.time != 1749000000U,
                      "a fix dated only by `time` should use it");
    /* Ours, from the packet's rx_time, and not the same number. */
    MESH_TEST_FAIL_IF(node->position.received != 1750000000U,
                      "the fix did not record when it reached us");

    /* `timestamp` is when the GPS solved, so it wins over the sender's clock. */
    position.timestamp = 1749500000U;
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Position_fields, &position) ||
                          !mesh_test_session_feed_app_packet(&session, 0x4001U,
                                                             meshtastic_PortNum_POSITION_APP,
                                                             payload, stream.bytes_written),
                      "encode dated POSITION_APP failed");
    node = mesh_test_session_find_node(&session, 0x4001U);
    MESH_TEST_FAIL_IF(node->position.time != 1749500000U,
                      "`timestamp` should outrank the sender's own clock");

    /* The common case on a real mesh: neither clock set. The fix is still kept, and it is
       still stamped with our arrival - which is the whole reason `received` exists. */
    meshtastic_Position undated = meshtastic_Position_init_default;
    undated.has_latitude_i = true;
    undated.latitude_i = 447000000;
    undated.has_longitude_i = true;
    undated.longitude_i = -680000000;
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Position_fields, &undated) ||
                          !mesh_test_session_feed_app_packet(&session, 0x4001U,
                                                             meshtastic_PortNum_POSITION_APP,
                                                             payload, stream.bytes_written),
                      "encode undated POSITION_APP failed");
    node = mesh_test_session_find_node(&session, 0x4001U);
    MESH_TEST_FAIL_IF(node->position.latitude_i != 447000000, "the undated fix did not land");
    MESH_TEST_FAIL_IF(node->position.time != 0U,
                      "a node that dated nothing must not be given a date");
    MESH_TEST_FAIL_IF(node->position.received != 1750000000U,
                      "an undated fix still has an arrival time");

    /*
     * An impossible pair. The wire carries sfixed32, which holds four times the range a
     * coordinate can occupy, so this is what a buggy or hostile sender looks like - and the
     * node keeps the fix we already believed rather than being moved 150 degrees north.
     */
    meshtastic_Position absurd = meshtastic_Position_init_default;
    absurd.has_latitude_i = true;
    absurd.latitude_i = 1500000000;
    absurd.has_longitude_i = true;
    absurd.longitude_i = -680000000;
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Position_fields, &absurd) ||
                          !mesh_test_session_feed_app_packet(&session, 0x4001U,
                                                             meshtastic_PortNum_POSITION_APP,
                                                             payload, stream.bytes_written),
                      "encode out-of-range POSITION_APP failed");
    node = mesh_test_session_find_node(&session, 0x4001U);
    MESH_TEST_FAIL_IF(node->position.latitude_i != 447000000,
                      "an out-of-range fix must not replace a good one");
    MESH_TEST_FAIL_IF(!node->position.valid, "an out-of-range fix must not invalidate the last");

    /* And a node whose *first* fix is out of range simply has none, rather than one in a
       place that does not exist. */
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Position_fields, &absurd) ||
                          !mesh_test_session_feed_app_packet(&session, 0x4002U,
                                                             meshtastic_PortNum_POSITION_APP,
                                                             payload, stream.bytes_written),
                      "encode first out-of-range POSITION_APP failed");
    const struct mesh_node_summary *stranger = mesh_test_session_find_node(&session, 0x4002U);
    MESH_TEST_FAIL_IF(stranger == NULL, "the node should still be in the roster");
    MESH_TEST_FAIL_IF(stranger->position.valid, "an out-of-range first fix must not be kept");

    /*
     * A NodeInfo replayed out of the radio's NodeDB, carrying a fix the node never dated.
     *
     * The tempting stamp here is the node's own last_heard, and it is wrong for the same
     * reason the whole `received` field exists: last_heard is the node's most recent packet of
     * any kind, so a node whose coordinates are days old but which sent telemetry a minute ago
     * would report a one-minute-old fix. We did not watch this fix arrive and we say so.
     */
    meshtastic_FromRadio replay = meshtastic_FromRadio_init_default;
    replay.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    replay.node_info.num = 0x4004U;
    replay.node_info.last_heard = 1750000560U; /* chatty a minute ago */
    replay.node_info.has_position = true;
    replay.node_info.position.has_latitude_i = true;
    replay.node_info.position.latitude_i = 447654321;
    replay.node_info.position.has_longitude_i = true;
    replay.node_info.position.longitude_i = -680012345;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &replay),
                      "encode replayed node_info failed");
    const struct mesh_node_summary *replayed = mesh_test_session_find_node(&session, 0x4004U);
    MESH_TEST_FAIL_IF(replayed == NULL || !replayed->position.valid,
                      "a replayed fix should still be kept");
    MESH_TEST_FAIL_IF(replayed->position.received == replay.node_info.last_heard,
                      "a replayed fix must not be dated by unrelated node activity");
    MESH_TEST_FAIL_IF(replayed->position.received != 0U || replayed->position.time != 0U,
                      "a replayed fix nobody dated has no clock at all");
    /* The node is still as recently heard as it says; only the *fix* is undated. */
    MESH_TEST_FAIL_IF(replayed->last_heard != 1750000560U,
                      "the node's own last_heard should be untouched");

    /* A replayed NodeInfo whose fix the node *did* date keeps that date, because the node
       answering the question is the one case where an answer exists. */
    replay.node_info.num = 0x4005U;
    replay.node_info.position.timestamp = 1749500000U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &replay),
                      "encode dated replayed node_info failed");
    const struct mesh_node_summary *dated = mesh_test_session_find_node(&session, 0x4005U);
    MESH_TEST_FAIL_IF(dated == NULL || dated->position.time != 1749500000U,
                      "a replayed fix the node dated should keep that date");

    /* Null Island is a real point, and a client that quietly dropped it would be guessing at
       the sender's firmware rather than range-checking. */
    meshtastic_Position origin = meshtastic_Position_init_default;
    origin.has_latitude_i = true;
    origin.latitude_i = 0;
    origin.has_longitude_i = true;
    origin.longitude_i = 0;
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Position_fields, &origin) ||
                          !mesh_test_session_feed_app_packet(&session, 0x4003U,
                                                             meshtastic_PortNum_POSITION_APP,
                                                             payload, stream.bytes_written),
                      "encode 0,0 POSITION_APP failed");
    const struct mesh_node_summary *at_origin = mesh_test_session_find_node(&session, 0x4003U);
    MESH_TEST_FAIL_IF(at_origin == NULL || !at_origin->position.valid,
                      "0,0 is a real place and must be kept");

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

/* Helper: runs a full want_config cycle, replaying `nodes` as the radio's NodeDB. */
static bool session_test_sync(struct mesh_session *session, uint32_t my_node, const uint32_t *nodes,
                              size_t node_count) {
    if (mesh_session_begin_handshake(session) != 0) {
        return false;
    }
    const uint32_t request_id = session->handshake.request_id;

    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = my_node;
    my_info.my_info.nodedb_count = (uint16_t)node_count;
    if (!mesh_test_session_feed_from_radio(session, &my_info)) {
        return false;
    }

    for (size_t i = 0; i < node_count; ++i) {
        meshtastic_FromRadio info = meshtastic_FromRadio_init_default;
        info.which_payload_variant = meshtastic_FromRadio_node_info_tag;
        info.node_info.num = nodes[i];
        info.node_info.has_user = true;
        snprintf(info.node_info.user.short_name, sizeof info.node_info.user.short_name, "N%03u",
                 (unsigned)(i % 1000U));
        if (!mesh_test_session_feed_from_radio(session, &info)) {
            return false;
        }
    }

    meshtastic_FromRadio done = meshtastic_FromRadio_init_default;
    done.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
    done.config_complete_id = request_id;
    return mesh_test_session_feed_from_radio(session, &done);
}

/*
 * A node with no User is the normal case out in the field, not an error: the firmware replays
 * its database once and a node heard afterwards is a number until it introduces itself. The
 * phone shows "Meshtastic e54c" for one of those, derived from the node number, and so must
 * this - a row of dashes with an empty line beside it reads as a broken client.
 */
MESH_TEST_CASE(session_node_default_identity, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    /* Straight out of the NodeDB with nothing in it but a number. */
    meshtastic_FromRadio bare = meshtastic_FromRadio_init_default;
    bare.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    bare.node_info.num = 0xb2a7e54cU;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &bare), "encode node_info");

    const struct mesh_node_summary *node = mesh_test_session_find_node(&session, 0xb2a7e54cU);
    MESH_TEST_FAIL_IF(node == NULL, "the userless node was not cached");
    MESH_TEST_FAIL_IF(strcmp(node->short_name, "e54c") != 0 ||
                          strcmp(node->long_name, "Meshtastic e54c") != 0 ||
                          strcmp(node->user_id, "!b2a7e54c") != 0 || node->has_user,
                      "the derived identity is not what the apps show");

    /* A real User replaces it, and says so. */
    meshtastic_FromRadio named = meshtastic_FromRadio_init_default;
    named.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    named.node_info.num = 0xb2a7e54cU;
    named.node_info.has_user = true;
    snprintf(named.node_info.user.long_name, sizeof named.node_info.user.long_name,
             "Hill Repeater");
    snprintf(named.node_info.user.short_name, sizeof named.node_info.user.short_name, "HILL");
    snprintf(named.node_info.user.id, sizeof named.node_info.user.id, "!b2a7e54c");
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &named), "encode named");
    node = mesh_test_session_find_node(&session, 0xb2a7e54cU);
    MESH_TEST_FAIL_IF(node == NULL || strcmp(node->short_name, "HILL") != 0 || !node->has_user,
                      "a real name did not replace the derived one");

    /* An empty User is the radio saying it knows nothing; it must not blank what we have. */
    meshtastic_FromRadio empty = meshtastic_FromRadio_init_default;
    empty.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    empty.node_info.num = 0xb2a7e54cU;
    empty.node_info.has_user = true;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &empty), "encode empty user");
    node = mesh_test_session_find_node(&session, 0xb2a7e54cU);
    MESH_TEST_FAIL_IF(node == NULL || strcmp(node->short_name, "HILL") != 0,
                      "an empty User blanked a name we already had");

    /* Same for a node that arrives as a bare packet rather than through the database. */
    const uint8_t payload[] = {0x01};
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_app_packet(&session, 0x0a1b2c3dU,
                                                         meshtastic_PortNum_TEXT_MESSAGE_APP,
                                                         payload, sizeof payload),
                      "feed text packet");
    node = mesh_test_session_find_node(&session, 0x0a1b2c3dU);
    MESH_TEST_FAIL_IF(node == NULL || strcmp(node->short_name, "2c3d") != 0 ||
                          strcmp(node->user_id, "!0a1b2c3d") != 0,
                      "a node heard before its NodeInfo has no derived identity");

    record_success(test_name);
}

/*
 * The roster outlives the connection. The radio's NodeDB is small - 80 entries on the hardware
 * this targets - and evicts as it fills, so a resync that reset the cache to whatever the radio
 * still held lost nodes that existed nowhere else. What the resync does decide is which of them
 * the radio can still reach.
 */
MESH_TEST_CASE(session_roster_survives_resync, unit) {
    struct mesh_session session;
    mesh_session_init(&session);
    struct mesh_test_trace_capture capture;
    memset(&capture, 0, sizeof capture);
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &capture);

    const uint32_t first[] = {0x2222U, 0x3333U};
    MESH_TEST_FAIL_IF(!session_test_sync(&session, 0x1111U, first, 2U), "first sync");
    const struct mesh_node_summary *kept = mesh_test_session_find_node(&session, 0x3333U);
    MESH_TEST_FAIL_IF(kept == NULL || !kept->in_nodedb, "a synced node is not marked in-NodeDB");

    /* Reconnect. The radio has since forgotten 0x3333. */
    mesh_session_detach(&session);
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &capture);
    const uint32_t second[] = {0x2222U};
    MESH_TEST_FAIL_IF(!session_test_sync(&session, 0x1111U, second, 1U), "second sync");

    kept = mesh_test_session_find_node(&session, 0x3333U);
    MESH_TEST_FAIL_IF(kept == NULL, "the roster forgot a node the radio evicted");
    MESH_TEST_FAIL_IF(kept->in_nodedb, "an evicted node is still marked in-NodeDB");
    MESH_TEST_FAIL_IF(strcmp(kept->short_name, "N001") != 0, "the evicted node lost its name");
    const struct mesh_node_summary *still = mesh_test_session_find_node(&session, 0x2222U);
    MESH_TEST_FAIL_IF(still == NULL || !still->in_nodedb, "a resynced node is not in-NodeDB");

    /* A restart loses the session but not the cache, so the owner is handed back the same way
       the nodes are; without it the next radio - any radio - inherits this roster. */
    struct mesh_session restarted;
    mesh_session_init(&restarted);
    mesh_session_set_roster_owner(&restarted, 0x1111U);
    MESH_TEST_FAIL_IF(mesh_session_roster_owner(&restarted) != 0x1111U, "owner not restored");
    struct mesh_node_summary restored;
    memset(&restored, 0, sizeof restored);
    restored.node_id = 0x3333U;
    (void)mesh_str_copy(restored.short_name, sizeof restored.short_name, "N001");
    mesh_session_seed_node(&restarted, &restored);
    mesh_session_attach(&restarted, mesh_test_trace_capture_fn, &capture);
    const uint32_t elsewhere[] = {0x5555U};
    MESH_TEST_FAIL_IF(!session_test_sync(&restarted, 0x9999U, elsewhere, 1U), "restarted sync");
    MESH_TEST_FAIL_IF(mesh_test_session_find_node(&restarted, 0x3333U) != NULL,
                      "a restored roster survived a radio it does not belong to");

    /* A name nobody could have derived came from a User, whatever an older cache failed to
       record; a derived one stays derived. */
    struct mesh_node_summary legacy;
    memset(&legacy, 0, sizeof legacy);
    legacy.node_id = 0xb2a7e54cU;
    (void)mesh_str_copy(legacy.short_name, sizeof legacy.short_name, "HILL");
    (void)mesh_str_copy(legacy.long_name, sizeof legacy.long_name, "Hill Repeater");
    mesh_session_seed_node(&restarted, &legacy);
    const struct mesh_node_summary *seeded = mesh_test_session_find_node(&restarted, 0xb2a7e54cU);
    MESH_TEST_FAIL_IF(seeded == NULL || !seeded->has_user,
                      "a stored name from before has_user reads as derived");

    struct mesh_node_summary plain;
    memset(&plain, 0, sizeof plain);
    plain.node_id = 0x0a1b2c3dU;
    (void)mesh_str_copy(plain.short_name, sizeof plain.short_name, "2c3d");
    (void)mesh_str_copy(plain.long_name, sizeof plain.long_name, "Meshtastic 2c3d");
    mesh_session_seed_node(&restarted, &plain);
    seeded = mesh_test_session_find_node(&restarted, 0x0a1b2c3dU);
    MESH_TEST_FAIL_IF(seeded == NULL || seeded->has_user,
                      "a derived name was mistaken for the node's own");
    mesh_session_detach(&restarted);

    /* Another radio is another mesh's view: that roster is not this one's. */
    const uint32_t third[] = {0x4444U};
    MESH_TEST_FAIL_IF(!session_test_sync(&session, 0x9999U, third, 1U), "third sync");
    MESH_TEST_FAIL_IF(mesh_test_session_find_node(&session, 0x2222U) != NULL ||
                          mesh_test_session_find_node(&session, 0x3333U) != NULL,
                      "the roster survived a radio swap");
    MESH_TEST_FAIL_IF(mesh_test_session_find_node(&session, 0x4444U) == NULL,
                      "the new radio's nodes are missing");

    record_success(test_name);
}

/*
 * Dropping cached nodes on purpose. The roster outliving the radio's NodeDB is what makes the
 * client worth more than a mirror of it, and it is also what leaves the Nodes tab holding
 * eighty-one nodes the moment somebody resets the radio's database - so there has to be a way
 * to say "drop what the radio no longer has" without sending the radio anything at all.
 *
 * Two things it must not drop, for the same reasons eviction never takes them: our own record,
 * which every screen resolves a name through, and a pinned node, which is the user having said
 * keep this one.
 */
MESH_TEST_CASE(session_forget_nodes, unit) {
    struct mesh_session session;
    mesh_session_init(&session);
    struct mesh_test_trace_capture capture;
    memset(&capture, 0, sizeof capture);
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &capture);

    /* A NodeInfo is proof on arrival, not at config_complete: a list drawn mid-sync must not
       show every node as one the radio has forgotten. */
    MESH_TEST_FAIL_IF(mesh_session_begin_handshake(&session) != 0, "handshake");
    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = 0x1111U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &my_info), "my_info");
    meshtastic_FromRadio info = meshtastic_FromRadio_init_default;
    info.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    info.node_info.num = 0x2222U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &info), "node_info");
    const struct mesh_node_summary *early = mesh_test_session_find_node(&session, 0x2222U);
    MESH_TEST_FAIL_IF(early == NULL || !early->in_nodedb,
                      "a node is not in the NodeDB until the sync ends");
    MESH_TEST_FAIL_IF(mesh_session_forgettable_nodes(&session, true) != 0U,
                      "a sync in progress counted a node as one to forget");

    /* Now the real thing: four nodes, ourselves among them, as a radio replays them. */
    const uint32_t all[] = {0x1111U, 0x2222U, 0x3333U, 0x4444U};
    MESH_TEST_FAIL_IF(!session_test_sync(&session, 0x1111U, all, 4U), "first sync");
    MESH_TEST_FAIL_IF(session.handshake.node_count != 4U, "the roster did not fill");
    MESH_TEST_FAIL_IF(mesh_session_forgettable_nodes(&session, true) != 0U,
                      "a freshly synced roster has nodes the radio does not");
    MESH_TEST_FAIL_IF(mesh_session_forgettable_nodes(&session, false) != 3U,
                      "emptying the roster would drop everything but our own record");

    /* Pin one of the two the radio is about to forget. */
    for (size_t i = 0; i < session.handshake.node_count; ++i) {
        if (session.handshake.nodes[i].node_id == 0x3333U) {
            session.handshake.nodes[i].is_favorite = true;
        }
    }

    /* The radio's database has been reset: it comes back carrying itself and one node. */
    mesh_session_detach(&session);
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &capture);
    const uint32_t after_reset[] = {0x1111U, 0x2222U};
    MESH_TEST_FAIL_IF(!session_test_sync(&session, 0x1111U, after_reset, 2U), "second sync");
    MESH_TEST_FAIL_IF(session.handshake.node_count != 4U, "the reset took the roster with it");
    /* Two nodes are off the radio and one of them is pinned, so the count the Settings row
       shows is one - what the press would remove, not what has gone stale. A row advertising
       the other number would sit there offering to drop a node it always keeps. */
    MESH_TEST_FAIL_IF(mesh_session_forgettable_nodes(&session, true) != 1U,
                      "the pinned orphan was counted as one the press would drop");

    /* Off-radio only: the unpinned orphan goes and nothing else does. */
    MESH_TEST_FAIL_IF(mesh_session_forget_nodes(&session, true) != 1,
                      "forgetting off-radio nodes dropped the wrong number");
    MESH_TEST_FAIL_IF(mesh_test_session_find_node(&session, 0x4444U) != NULL,
                      "the orphan is still in the roster");
    MESH_TEST_FAIL_IF(mesh_test_session_find_node(&session, 0x3333U) == NULL,
                      "a pinned node was forgotten");
    MESH_TEST_FAIL_IF(mesh_test_session_find_node(&session, 0x1111U) == NULL ||
                          mesh_test_session_find_node(&session, 0x2222U) == NULL,
                      "a node the radio still carries was forgotten");
    MESH_TEST_FAIL_IF(session.handshake.node_count != 3U, "the roster did not shrink by one");
    MESH_TEST_FAIL_IF(mesh_session_forget_nodes(&session, true) != 0,
                      "a second press found something else to drop");
    MESH_TEST_FAIL_IF(mesh_session_forgettable_nodes(&session, true) != 0U,
                      "the count and the act disagree: nothing left to drop, still offered");

    /* All of them: everything but ourselves and the pin. */
    MESH_TEST_FAIL_IF(mesh_session_forgettable_nodes(&session, false) != 1U,
                      "emptying the roster would drop the protected records too");
    MESH_TEST_FAIL_IF(mesh_session_forget_nodes(&session, false) != 1,
                      "emptying the roster dropped the wrong number");
    MESH_TEST_FAIL_IF(session.handshake.node_count != 2U ||
                          mesh_test_session_find_node(&session, 0x1111U) == NULL ||
                          mesh_test_session_find_node(&session, 0x3333U) == NULL,
                      "ourselves and the pin should be what is left");
    MESH_TEST_FAIL_IF(mesh_session_forget_nodes(&session, false) != 0,
                      "emptying an empty roster reported work");
    MESH_TEST_FAIL_IF(mesh_session_forget_nodes(NULL, false) != -EINVAL, "NULL session");

    /* With no link at all - which is when a roster is most likely to be tidied - our own node
       is remembered through the roster's owner rather than my_info, which the drop cleared. */
    mesh_session_detach(&session);
    MESH_TEST_FAIL_IF(session.handshake.has_my_info, "detach kept my_info");
    struct mesh_node_summary stranger;
    memset(&stranger, 0, sizeof stranger);
    stranger.node_id = 0x5555U;
    mesh_session_seed_node(&session, &stranger);
    MESH_TEST_FAIL_IF(mesh_session_forget_nodes(&session, false) != 1,
                      "an offline clear dropped the wrong number");
    MESH_TEST_FAIL_IF(mesh_test_session_find_node(&session, 0x3333U) == NULL,
                      "the pin did not survive an offline clear");
    MESH_TEST_FAIL_IF(mesh_test_session_find_node(&session, 0x1111U) == NULL,
                      "our own node went with them");

    record_success(test_name);
}

/*
 * Full is no longer a wall. A roster that outlives connections does reach its cap on a busy
 * mesh, and the entry that goes is the least useful one: a node the radio has already forgotten
 * before one it still carries, oldest first, and never a node the user pinned.
 */
MESH_TEST_CASE(session_roster_eviction, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    for (uint32_t i = 0; i < MESH_SESSION_MAX_NODES; ++i) {
        struct mesh_node_summary node;
        memset(&node, 0, sizeof node);
        node.node_id = 0x10000U + i;
        node.last_heard = 2000U + i;
        node.in_nodedb = true;
        /* One pinned node, and it is the oldest thing in the roster - exactly the entry a
           last_heard-only rule would take first. */
        if (i == 0U) {
            node.is_favorite = true;
            node.last_heard = 1U;
        }
        /* One the radio no longer carries, but heard more recently than most. */
        if (i == 1U) {
            node.in_nodedb = false;
            node.last_heard = 9000U;
        }
        mesh_session_seed_node(&session, &node);
    }
    MESH_TEST_FAIL_IF(session.handshake.node_count != MESH_SESSION_MAX_NODES,
                      "the roster did not fill");

    const uint8_t payload[] = {0x01};
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_app_packet(&session, 0xfeedU,
                                                         meshtastic_PortNum_TEXT_MESSAGE_APP,
                                                         payload, sizeof payload),
                      "feed text packet");

    MESH_TEST_FAIL_IF(mesh_test_session_find_node(&session, 0xfeedU) == NULL,
                      "a full roster refused a new node");
    MESH_TEST_FAIL_IF(mesh_test_session_find_node(&session, 0x10001U) != NULL,
                      "the node the radio had forgotten was not the one evicted");
    MESH_TEST_FAIL_IF(mesh_test_session_find_node(&session, 0x10000U) == NULL,
                      "a pinned node was evicted");
    MESH_TEST_FAIL_IF(session.handshake.node_count != MESH_SESSION_MAX_NODES,
                      "eviction changed the roster size");

    /* A seed for a node already in the roster is a no-op, not a duplicate. */
    struct mesh_node_summary again;
    memset(&again, 0, sizeof again);
    again.node_id = 0xfeedU;
    mesh_session_seed_node(&session, &again);
    MESH_TEST_FAIL_IF(session.handshake.node_count != MESH_SESSION_MAX_NODES,
                      "re-seeding a known node grew the roster");

    record_success(test_name);
}

/*
 * The three things the radio says about itself that used to fall off the end of the FromRadio
 * switch. Each one is the answer to a question nothing else on the link can answer, which is
 * why they are worth keeping at all:
 *
 * - a ClientNotification is the firmware explaining a decision to the user;
 * - a QueueStatus refusal is the *only* report of a packet that never went on the air, so no
 *   Routing reply will ever arrive to mark the message failed;
 * - `rebooted` says every fact the config sync gave us now describes a dead process.
 */
MESH_TEST_CASE(session_radio_announcements, unit) {
    struct mesh_session session;
    mesh_session_init(&session);
    struct mesh_test_trace_capture capture;
    memset(&capture, 0, sizeof capture);
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &capture);

    MESH_TEST_FAIL_IF(mesh_session_notification(&session)->seq != 0U,
                      "a notification was claimed before any arrived");
    MESH_TEST_FAIL_IF(mesh_session_queue_status(&session)->valid,
                      "a queue status was claimed before any arrived");

    /* The message is radio text like a node name, so it arrives sanitised: a control byte in
       it must not reach the framebuffer, and the wire field is far longer than our slot. */
    meshtastic_FromRadio note = meshtastic_FromRadio_init_default;
    note.which_payload_variant = meshtastic_FromRadio_clientNotification_tag;
    note.clientNotification.time = 1750000000U;
    note.clientNotification.level = meshtastic_LogRecord_Level_WARNING;
    note.clientNotification.has_reply_id = true;
    note.clientNotification.reply_id = 0x1234U;
    snprintf(note.clientNotification.message, sizeof note.clientNotification.message,
             "Duty cycle\nlimit reached");
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &note),
                      "encode clientNotification failed");

    const struct mesh_client_notification *held = mesh_session_notification(&session);
    MESH_TEST_FAIL_IF(held->seq != 1U, "the first notification did not take sequence 1");
    MESH_TEST_FAIL_IF(strcmp(held->text, "Duty cycle limit reached") != 0,
                      "the notification text was not sanitised into the slot");
    MESH_TEST_FAIL_IF(!held->has_reply_id || held->reply_id != 0x1234U ||
                          held->level != (uint8_t)meshtastic_LogRecord_Level_WARNING ||
                          held->time != 1750000000U,
                      "the notification's metadata was not kept");

    /* Two identical notifications are two events. Only the counter can say so. */
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &note),
                      "re-feed clientNotification failed");
    MESH_TEST_FAIL_IF(mesh_session_notification(&session)->seq != 2U,
                      "a repeated notification did not advance the sequence");

    /* A queue status with no refusal in it is depth reporting and touches no message. */
    const uint32_t packet_id = 0x5150U;
    struct mesh_message sent;
    memset(&sent, 0, sizeof sent);
    sent.packet_id = packet_id;
    sent.direction = MESH_MESSAGE_OUTBOUND;
    sent.ack = MESH_MESSAGE_ACK_PENDING;
    mesh_str_copy(sent.text, sizeof sent.text, "hello");
    MESH_TEST_FAIL_IF(mesh_message_log_append(&session.messages, &sent) == NULL,
                      "seeding the outbound message failed");

    meshtastic_FromRadio queue = meshtastic_FromRadio_init_default;
    queue.which_payload_variant = meshtastic_FromRadio_queueStatus_tag;
    queue.queueStatus.res = 0;
    queue.queueStatus.free = 14;
    queue.queueStatus.maxlen = 16;
    queue.queueStatus.mesh_packet_id = packet_id;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &queue),
                      "encode accepted queueStatus failed");
    MESH_TEST_FAIL_IF(!mesh_session_queue_status(&session)->valid ||
                          mesh_session_queue_status(&session)->free != 14U ||
                          mesh_session_queue_status(&session)->maxlen != 16U,
                      "the queue depth was not kept");
    MESH_TEST_FAIL_IF(mesh_message_log_find(&session.messages, packet_id)->ack !=
                          MESH_MESSAGE_ACK_PENDING,
                      "an accepted packet was marked failed");

    /* A refusal is the packet never leaving the radio. `res` is a Routing_Error, so it lands
       on the message as the reason without any new failure vocabulary. */
    queue.queueStatus.res = (int8_t)meshtastic_Routing_Error_NO_INTERFACE;
    queue.queueStatus.free = 0;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &queue),
                      "encode refused queueStatus failed");
    const struct mesh_message *refused = mesh_message_log_find(&session.messages, packet_id);
    MESH_TEST_FAIL_IF(refused->ack != MESH_MESSAGE_ACK_FAILED ||
                          refused->ack_error != (uint8_t)meshtastic_Routing_Error_NO_INTERFACE,
                      "a refused packet was not marked failed with the radio's reason");

    /* A reboot re-runs the config sync: everything the last one told us describes a process
       that no longer exists. The want_config_id that goes out is the proof it did. */
    const unsigned sends_before = capture.calls;
    meshtastic_FromRadio rebooted = meshtastic_FromRadio_init_default;
    rebooted.which_payload_variant = meshtastic_FromRadio_rebooted_tag;
    rebooted.rebooted = true;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &rebooted),
                      "encode rebooted failed");
    MESH_TEST_FAIL_IF(capture.calls == sends_before,
                      "a reported reboot did not re-run the config sync");
    MESH_TEST_FAIL_IF(!session.handshake.request_in_flight,
                      "the re-run handshake is not in flight");
    MESH_TEST_FAIL_IF(session.reboot_notices != 1U,
                      "the reboot counter did not survive the handshake reset it triggers");

    /* The reboot cleared the per-connection state, notification and queue included: they
       described the process that just died. */
    MESH_TEST_FAIL_IF(mesh_session_notification(&session)->seq != 0U ||
                          mesh_session_queue_status(&session)->valid,
                      "the dead process's announcements survived its reboot");

    /* But the conversation does not belong to the radio, so it is still there. */
    MESH_TEST_FAIL_IF(mesh_message_log_find(&session.messages, packet_id) == NULL,
                      "the reboot took the message log with it");

    /* And a dropped link forgets the counter, so the next radio's first reboot is its first. */
    mesh_session_detach(&session);
    MESH_TEST_FAIL_IF(session.reboot_notices != 0U, "the reboot counter survived the link drop");

    /*
     * RSSI rides on the packet alongside SNR and answers a different question: how loud the
     * signal was rather than how far above the noise. A packet that reached us over MQTT was
     * not heard by this radio at all, so its RSSI describes somebody else's antenna and must
     * not be recorded as ours.
     */
    meshtastic_MeshPacket rf = mesh_test_make_decoded_packet(
        0x9101U, 0x9102U, 0U, 900U, meshtastic_PortNum_TEXT_MESSAGE_APP, "hi", 2U);
    rf.has_rx_rssi = true;
    rf.rx_rssi = -97;
    meshtastic_FromRadio wrapper = meshtastic_FromRadio_init_default;
    wrapper.which_payload_variant = meshtastic_FromRadio_packet_tag;
    wrapper.packet = rf;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &wrapper),
                      "feed an RF packet failed");
    const struct mesh_node_summary *rf_node = mesh_test_session_find_node(&session, 0x9101U);
    MESH_TEST_FAIL_IF(rf_node == NULL || !rf_node->has_rssi || rf_node->rx_rssi != -97,
                      "the packet's RSSI was not recorded on the node");

    wrapper.packet.from = 0x9103U;
    wrapper.packet.via_mqtt = true;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &wrapper),
                      "feed an MQTT packet failed");
    const struct mesh_node_summary *mqtt_node = mesh_test_session_find_node(&session, 0x9103U);
    MESH_TEST_FAIL_IF(mqtt_node == NULL || mqtt_node->has_rssi,
                      "an MQTT-fed packet's RSSI was taken as this radio's reading");

    record_success(test_name);
}

/*
 * The four Telemetry variants beyond device and environment metrics, each landing on the node
 * that broadcast it.
 *
 * What is worth pinning here is the has_* handling rather than the copy. Every field in these
 * messages is optional on the wire except HostMetrics', so a sender that has one sensor and not
 * another must not leave the node claiming a reading of zero for the one it lacks - and
 * HostMetrics, which has no optional flags at all, must derive them from what a running host
 * cannot plausibly report.
 */
MESH_TEST_CASE(session_sensor_telemetry, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    const uint32_t node_id = 0x9001U;
    uint8_t payload[512];

    /* A two-channel current monitor: the third channel is absent, not zero. */
    meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_default;
    telemetry.time = 1750000000U;
    telemetry.which_variant = meshtastic_Telemetry_power_metrics_tag;
    telemetry.variant.power_metrics.has_ch1_voltage = true;
    telemetry.variant.power_metrics.ch1_voltage = 12.6f;
    telemetry.variant.power_metrics.has_ch1_current = true;
    telemetry.variant.power_metrics.ch1_current = 340.0f;
    telemetry.variant.power_metrics.has_ch2_voltage = true;
    telemetry.variant.power_metrics.ch2_voltage = 5.02f;
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Telemetry_fields, &telemetry) ||
                          !mesh_test_session_feed_app_packet(&session, node_id,
                                                             meshtastic_PortNum_TELEMETRY_APP,
                                                             payload, stream.bytes_written),
                      "feed power metrics failed");

    const struct mesh_node_summary *node = mesh_test_session_find_node(&session, node_id);
    MESH_TEST_FAIL_IF(node == NULL, "the power-metrics packet did not create the node");
    MESH_TEST_FAIL_IF(!node->power.valid || !node->power.channel[0].has_voltage ||
                          node->power.channel[0].voltage < 12.5f ||
                          node->power.channel[0].voltage > 12.7f ||
                          !node->power.channel[0].has_current,
                      "channel 1 readings were not kept");
    MESH_TEST_FAIL_IF(!node->power.channel[1].has_voltage || node->power.channel[1].has_current,
                      "channel 2's absent current was taken as a reading");
    MESH_TEST_FAIL_IF(node->power.channel[2].has_voltage || node->power.channel[2].has_current,
                      "an unreported third channel was taken as a reading");

    /* Air quality: a particulate sensor with no CO2 on it. */
    telemetry = (meshtastic_Telemetry)meshtastic_Telemetry_init_default;
    telemetry.time = 1750000100U;
    telemetry.which_variant = meshtastic_Telemetry_air_quality_metrics_tag;
    telemetry.variant.air_quality_metrics.has_pm25_standard = true;
    telemetry.variant.air_quality_metrics.pm25_standard = 12U;
    telemetry.variant.air_quality_metrics.has_pm_voc_idx = true;
    telemetry.variant.air_quality_metrics.pm_voc_idx = 103.0f;
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Telemetry_fields, &telemetry) ||
                          !mesh_test_session_feed_app_packet(&session, node_id,
                                                             meshtastic_PortNum_TELEMETRY_APP,
                                                             payload, stream.bytes_written),
                      "feed air quality failed");
    node = mesh_test_session_find_node(&session, node_id);
    MESH_TEST_FAIL_IF(!node->air_quality.valid || node->air_quality.pm25_standard != 12U ||
                          !node->air_quality.has_voc_index,
                      "air quality readings were not kept");
    MESH_TEST_FAIL_IF(node->air_quality.has_co2 || node->air_quality.has_nox_index,
                      "an absent CO2 or NOx reading was taken as zero");
    /* A second variant must not have wiped the first: they are separate groups. */
    MESH_TEST_FAIL_IF(!node->power.valid, "air quality replaced the power readings");

    /* Health. */
    telemetry = (meshtastic_Telemetry)meshtastic_Telemetry_init_default;
    telemetry.time = 1750000200U;
    telemetry.which_variant = meshtastic_Telemetry_health_metrics_tag;
    telemetry.variant.health_metrics.has_heart_bpm = true;
    telemetry.variant.health_metrics.heart_bpm = 62U;
    telemetry.variant.health_metrics.has_spO2 = true;
    telemetry.variant.health_metrics.spO2 = 98U;
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Telemetry_fields, &telemetry) ||
                          !mesh_test_session_feed_app_packet(&session, node_id,
                                                             meshtastic_PortNum_TELEMETRY_APP,
                                                             payload, stream.bytes_written),
                      "feed health metrics failed");
    node = mesh_test_session_find_node(&session, node_id);
    MESH_TEST_FAIL_IF(!node->health.valid || node->health.heart_bpm != 62U ||
                          node->health.spo2 != 98U || node->health.has_temperature,
                      "health readings were not kept");

    /*
     * HostMetrics has no optional fields on the wire, so the flags come from what a running
     * host cannot report: no uptime and no free memory mean the sender did not fill them in.
     * A load average of zero is a real reading, so the trio is flagged together.
     */
    telemetry = (meshtastic_Telemetry)meshtastic_Telemetry_init_default;
    telemetry.time = 1750000300U;
    telemetry.which_variant = meshtastic_Telemetry_host_metrics_tag;
    telemetry.variant.host_metrics.uptime_seconds = 90061U;
    telemetry.variant.host_metrics.freemem_bytes = 512ULL * 1024ULL * 1024ULL;
    telemetry.variant.host_metrics.diskfree1_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    telemetry.variant.host_metrics.load1 = 42U;
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Telemetry_fields, &telemetry) ||
                          !mesh_test_session_feed_app_packet(&session, node_id,
                                                             meshtastic_PortNum_TELEMETRY_APP,
                                                             payload, stream.bytes_written),
                      "feed host metrics failed");
    node = mesh_test_session_find_node(&session, node_id);
    MESH_TEST_FAIL_IF(!node->host.valid || !node->host.has_uptime ||
                          node->host.uptime_seconds != 90061U,
                      "host uptime was not kept");
    MESH_TEST_FAIL_IF(!node->host.has_freemem || node->host.freemem_kib != 512U * 1024U,
                      "free memory was not scaled to kibibytes");
    MESH_TEST_FAIL_IF(!node->host.has_diskfree || node->host.diskfree_mib != 4096U,
                      "free disk was not scaled to mebibytes");
    MESH_TEST_FAIL_IF(!node->host.has_load || node->host.load1 != 42U || node->host.load5 != 0U,
                      "the load trio was not flagged from the one non-zero average");

    /* A host that reported nothing must not read as a host with an empty disk. */
    telemetry = (meshtastic_Telemetry)meshtastic_Telemetry_init_default;
    telemetry.time = 1750000400U;
    telemetry.which_variant = meshtastic_Telemetry_host_metrics_tag;
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_Telemetry_fields, &telemetry) ||
                          !mesh_test_session_feed_app_packet(&session, 0x9002U,
                                                             meshtastic_PortNum_TELEMETRY_APP,
                                                             payload, stream.bytes_written),
                      "feed empty host metrics failed");
    node = mesh_test_session_find_node(&session, 0x9002U);
    MESH_TEST_FAIL_IF(node->host.has_uptime || node->host.has_freemem || node->host.has_diskfree ||
                          node->host.has_load,
                      "an empty HostMetrics was read as a host reporting zeroes");

    record_success(test_name);
}

/*
 * Asking a node for a reading now, rather than at its next broadcast.
 *
 * The wire shape is the whole of it: an empty payload on the port with `want_response` set. The
 * two things worth pinning are that the payload really is empty - a Position or a Telemetry
 * with fields in it would assert those fields at the far end - and that no want_ack rides
 * along, because the answer is the acknowledgement and a want_ack would double what this costs
 * the mesh for a question that reports its own success.
 */
MESH_TEST_CASE(session_request_readings, unit) {
    struct mesh_session session;
    mesh_session_init(&session);
    struct mesh_test_trace_capture capture;
    memset(&capture, 0, sizeof capture);

    /* Nothing goes out without a link, and nothing goes out before we know our own number:
       an AdminMessage-free request still needs to know which node is not the target. */
    MESH_TEST_FAIL_IF(mesh_session_request_position(&session, 0x4001U) != -ENOTCONN,
                      "a position request without a link should be refused");

    mesh_session_attach(&session, mesh_test_trace_capture_fn, &capture);
    MESH_TEST_FAIL_IF(mesh_session_request_telemetry(&session, 0x4001U) != -ENOTCONN,
                      "a request before MyNodeInfo should be refused");

    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = 0x4000U;
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &my_info),
                      "encode my_info failed");

    MESH_TEST_FAIL_IF(mesh_session_request_position(&session, 0x4000U) != -EINVAL,
                      "asking ourselves where we are is not a question");
    MESH_TEST_FAIL_IF(mesh_session_request_position(&session, MESH_MESSAGE_BROADCAST_ADDR) !=
                          -EINVAL,
                      "a broadcast request would ask the whole mesh at once");

    const struct {
        const char *label;
        int (*send)(struct mesh_session *, uint32_t);
        meshtastic_PortNum portnum;
    } cases[] = {
        {"position", mesh_session_request_position, meshtastic_PortNum_POSITION_APP},
        {"telemetry", mesh_session_request_telemetry, meshtastic_PortNum_TELEMETRY_APP},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        memset(&capture, 0, sizeof capture);
        MESH_TEST_FAIL_IF(cases[i].send(&session, 0x4001U) != 0,
                          "the request should have gone out");
        MESH_TEST_FAIL_IF(capture.calls != 1U || capture.len == 0U,
                          "exactly one ToRadio should have been sent");

        meshtastic_ToRadio sent = meshtastic_ToRadio_init_default;
        pb_istream_t stream = pb_istream_from_buffer(capture.packet, capture.len);
        MESH_TEST_FAIL_IF(!pb_decode(&stream, meshtastic_ToRadio_fields, &sent),
                          "the request did not decode as a ToRadio");
        MESH_TEST_FAIL_IF(sent.which_payload_variant != meshtastic_ToRadio_packet_tag,
                          "the request should be a packet");

        const meshtastic_MeshPacket *packet = &sent.packet;
        char message[128];
        if (packet->to != 0x4001U || packet->id == 0U) {
            snprintf(message, sizeof message, "the %s request is not addressed to the node",
                     cases[i].label);
            record_failure(test_name, message);
            return;
        }
        if (packet->decoded.portnum != cases[i].portnum || !packet->decoded.want_response) {
            snprintf(message, sizeof message, "the %s request is not a want_response on its port",
                     cases[i].label);
            record_failure(test_name, message);
            return;
        }
        if (packet->decoded.payload.size != 0U) {
            snprintf(message, sizeof message, "the %s request carries a payload it should not",
                     cases[i].label);
            record_failure(test_name, message);
            return;
        }
        if (packet->want_ack) {
            snprintf(message, sizeof message, "the %s request asks for an ack as well as a reply",
                     cases[i].label);
            record_failure(test_name, message);
            return;
        }
    }

    record_success(test_name);
}

/*
 * NeighborInfo: the only thing on the wire that describes the mesh as a graph rather than as a
 * collection of one-hop readings.
 *
 * The attribution is what has to be right. A NeighborInfo is forwarded across the mesh and
 * `last_sent_by_id` names whoever relayed it, so a client that filed the list under the packet's
 * `from` would draw one node's neighbours on another node's screen - wrong in a way that looks
 * entirely plausible.
 */
MESH_TEST_CASE(session_neighbor_info, unit) {
    struct mesh_session session;
    mesh_session_init(&session);

    uint8_t payload[256];
    meshtastic_NeighborInfo info = meshtastic_NeighborInfo_init_default;
    info.node_id = 0xA001U;
    info.last_sent_by_id = 0xA002U;
    info.node_broadcast_interval_secs = 14400U;
    info.neighbors_count = 3U;
    info.neighbors[0].node_id = 0xA002U;
    info.neighbors[0].snr = 8.25F;
    info.neighbors[1].node_id = 0xA003U;
    info.neighbors[1].snr = -3.5F;
    /* A zero id is not a node; it must not take a slot and leave the count claiming it. */
    info.neighbors[2].node_id = 0U;
    info.neighbors[2].snr = 1.0F;

    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_NeighborInfo_fields, &info),
                      "encode NeighborInfo failed");

    /* Relayed by 0xA002: the list belongs to 0xA001, which is what the payload says. */
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_app_packet(&session, 0xA002U,
                                                         meshtastic_PortNum_NEIGHBORINFO_APP,
                                                         payload, stream.bytes_written),
                      "feed NeighborInfo failed");

    const struct mesh_node_summary *reporter = mesh_test_session_find_node(&session, 0xA001U);
    MESH_TEST_FAIL_IF(reporter == NULL || !reporter->neighbors.valid,
                      "the list was not filed under the node that reported it");
    MESH_TEST_FAIL_IF(reporter->neighbors.count != 2U,
                      "the zero-id neighbour should not have taken a slot");
    MESH_TEST_FAIL_IF(reporter->neighbors.entries[0].node_id != 0xA002U ||
                          reporter->neighbors.entries[1].node_id != 0xA003U,
                      "the neighbours were not kept in order");
    MESH_TEST_FAIL_IF(reporter->neighbors.broadcast_interval_secs != 14400U,
                      "the reporting node's broadcast interval was not kept");

    const struct mesh_node_summary *relayer = mesh_test_session_find_node(&session, 0xA002U);
    MESH_TEST_FAIL_IF(relayer != NULL && relayer->neighbors.valid,
                      "the relayer was credited with the reporting node's neighbours");

    /* A node that hears nobody is a real state - it is what a repeater that has dropped off the
       mesh looks like - so an empty report replaces the list rather than being ignored. */
    info.neighbors_count = 0U;
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_NeighborInfo_fields, &info) ||
                          !mesh_test_session_feed_app_packet(&session, 0xA001U,
                                                             meshtastic_PortNum_NEIGHBORINFO_APP,
                                                             payload, stream.bytes_written),
                      "feed empty NeighborInfo failed");
    reporter = mesh_test_session_find_node(&session, 0xA001U);
    MESH_TEST_FAIL_IF(!reporter->neighbors.valid || reporter->neighbors.count != 0U,
                      "an empty report should stand rather than leave the old list in place");

    /* More neighbours than the wire allows cannot overrun the record. */
    info.neighbors_count = (pb_size_t)(sizeof info.neighbors / sizeof info.neighbors[0]);
    for (pb_size_t i = 0; i < info.neighbors_count; ++i) {
        info.neighbors[i].node_id = 0xB000U + i;
        info.neighbors[i].snr = 1.0F;
    }
    stream = pb_ostream_from_buffer(payload, sizeof payload);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_NeighborInfo_fields, &info) ||
                          !mesh_test_session_feed_app_packet(&session, 0xA001U,
                                                             meshtastic_PortNum_NEIGHBORINFO_APP,
                                                             payload, stream.bytes_written),
                      "feed a full NeighborInfo failed");
    reporter = mesh_test_session_find_node(&session, 0xA001U);
    MESH_TEST_FAIL_IF(reporter->neighbors.count > MESH_NODE_MAX_NEIGHBORS,
                      "a full neighbour list overran the record");

    record_success(test_name);
}
