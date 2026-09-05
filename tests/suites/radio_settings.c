#define _POSIX_C_SOURCE 200809L

/* The AdminMessage get/set queue and the verbs built on it. */

#include "framework/mesh_test.h"
#include "support/proto_fixture.h"
#include "support/session_fixture.h"

#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"

#include <pb_decode.h>

#include "meshtastic/admin.pb.h"
#include "meshtastic/channel.pb.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/*
 * The Radio actions section's wire side. Shaped like the favorite pair - a passkey refresh,
 * the action itself, no read-back - and, like them, deliberately not a "settings write": the
 * radio stops answering in the middle of doing what it was asked, so an ack that never lands
 * must not surface as a rejected save.
 */
static bool test_admin_encodes(const struct mesh_radio_settings *settings,
                               enum mesh_admin_request_kind kind, uint32_t type,
                               meshtastic_AdminMessage *out_admin) {
    struct mesh_admin_request request;
    memset(&request, 0, sizeof request);
    request.kind = kind;
    request.type = type;
    request.my_node = 0x1234U;
    request.packet_id = 90U;

    uint8_t buffer[512];
    size_t written = 0U;
    if (mesh_radio_settings_encode_request(settings, &request, buffer, sizeof buffer, &written) !=
        0) {
        return false;
    }
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    if (!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio) ||
        to_radio.packet.decoded.portnum != meshtastic_PortNum_ADMIN_APP ||
        to_radio.packet.to != 0x1234U) {
        return false;
    }
    *out_admin = (meshtastic_AdminMessage)meshtastic_AdminMessage_init_default;
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    return pb_decode(&in, meshtastic_AdminMessage_fields, out_admin);
}

static bool test_action_encodes_as(enum mesh_admin_request_kind kind,
                                   meshtastic_AdminMessage *out_admin) {
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);
    return test_admin_encodes(&settings, kind, MESH_RADIO_ACTION_DELAY_SECONDS, out_admin);
}

MESH_TEST_CASE(radio_settings_admin_roundtrip, unit) {
    const uint32_t my_node = 0x9E9D0AD8U;

    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);

    /* A get_config request: to ourselves, ADMIN_APP, wants a response, no passkey yet. */
    struct mesh_admin_request request = {
        .kind = MESH_ADMIN_GET_CONFIG,
        .type = meshtastic_AdminMessage_ConfigType_LORA_CONFIG,
        .my_node = my_node,
        .packet_id = 77U,
    };
    uint8_t wire[256];
    size_t wire_len = 0U;
    if (mesh_radio_settings_encode_request(&settings, &request, wire, sizeof wire, &wire_len) !=
        0) {
        record_failure(test_name, "encode request failed");
        return;
    }
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(wire, wire_len);
    if (!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio)) {
        record_failure(test_name, "request is not a ToRadio");
        return;
    }
    if (to_radio.which_payload_variant != meshtastic_ToRadio_packet_tag ||
        to_radio.packet.to != my_node || to_radio.packet.id != 77U ||
        to_radio.packet.which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        to_radio.packet.decoded.portnum != meshtastic_PortNum_ADMIN_APP ||
        !to_radio.packet.decoded.want_response) {
        record_failure(test_name, "request packet header is wrong");
        return;
    }
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    if (!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
        admin.which_payload_variant != meshtastic_AdminMessage_get_config_request_tag ||
        admin.get_config_request != meshtastic_AdminMessage_ConfigType_LORA_CONFIG ||
        admin.session_passkey.size != 0U) {
        record_failure(test_name, "request AdminMessage is wrong");
        return;
    }

    /* The reply: LoRa config plus a session passkey, quoting our request id. */
    mesh_radio_settings_mark_sent(&settings, 77U, 1000U);
    if (!mesh_radio_settings_busy(&settings)) {
        record_failure(test_name, "should be busy after mark_sent");
        return;
    }
    admin = (meshtastic_AdminMessage)meshtastic_AdminMessage_init_default;
    admin.which_payload_variant = meshtastic_AdminMessage_get_config_response_tag;
    admin.get_config_response.which_payload_variant = meshtastic_Config_lora_tag;
    admin.get_config_response.payload_variant.lora.region =
        meshtastic_Config_LoRaConfig_RegionCode_US;
    admin.get_config_response.payload_variant.lora.use_preset = true;
    admin.get_config_response.payload_variant.lora.modem_preset =
        meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    admin.get_config_response.payload_variant.lora.hop_limit = 3U;
    static const uint8_t k_passkey[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    admin.session_passkey.size = sizeof k_passkey;
    memcpy(admin.session_passkey.bytes, k_passkey, sizeof k_passkey);

    meshtastic_MeshPacket reply;
    if (!mesh_test_make_admin_reply(my_node, 77U, &admin, &reply)) {
        record_failure(test_name, "encode reply failed");
        return;
    }
    if (mesh_radio_settings_ingest(&settings, &reply) != 1) {
        record_failure(test_name, "admin reply should be consumed");
        return;
    }
    if (!settings.has_lora || settings.lora.region != meshtastic_Config_LoRaConfig_RegionCode_US ||
        settings.lora.hop_limit != 3U || !settings.has_session_passkey ||
        settings.session_passkey_len != sizeof k_passkey ||
        memcmp(settings.session_passkey, k_passkey, sizeof k_passkey) != 0 ||
        settings.admin_replies != 1U || mesh_radio_settings_busy(&settings) ||
        !mesh_radio_settings_loaded(&settings)) {
        record_failure(test_name, "reply was not folded in");
        return;
    }

    /* The next request carries the passkey. */
    request.kind = MESH_ADMIN_GET_OWNER;
    request.packet_id = 78U;
    if (mesh_radio_settings_encode_request(&settings, &request, wire, sizeof wire, &wire_len) !=
        0) {
        record_failure(test_name, "second encode failed");
        return;
    }
    to_radio = (meshtastic_ToRadio)meshtastic_ToRadio_init_default;
    in = pb_istream_from_buffer(wire, wire_len);
    admin = (meshtastic_AdminMessage)meshtastic_AdminMessage_init_default;
    if (!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio)) {
        record_failure(test_name, "second request is not a ToRadio");
        return;
    }
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    if (!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
        admin.which_payload_variant != meshtastic_AdminMessage_get_owner_request_tag ||
        admin.session_passkey.size != sizeof k_passkey ||
        memcmp(admin.session_passkey.bytes, k_passkey, sizeof k_passkey) != 0) {
        record_failure(test_name, "second request should carry the passkey");
        return;
    }

    /* A text message is not ours to consume. */
    meshtastic_MeshPacket text = mesh_test_make_decoded_packet(
        0x1234U, my_node, 0U, 99U, meshtastic_PortNum_TEXT_MESSAGE_APP, "hi", 2U);
    if (mesh_radio_settings_ingest(&settings, &text) != 0) {
        record_failure(test_name, "text packet should be left alone");
        return;
    }

    record_success(test_name);
}

MESH_TEST_CASE(radio_settings_fetch_queue, unit) {
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);

    if (mesh_radio_settings_queue_probe(&settings) != 2U ||
        mesh_radio_settings_queue_probe(&settings) != 0U) {
        record_failure(test_name, "probe should queue two requests once");
        return;
    }

    struct mesh_admin_request request;
    if (!mesh_radio_settings_next_request(&settings, 0U, &request) ||
        request.kind != MESH_ADMIN_GET_METADATA) {
        record_failure(test_name, "first request should be metadata");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 1U, 0U);
    if (mesh_radio_settings_next_request(&settings, 100U, &request)) {
        record_failure(test_name, "nothing should go out while a reply is awaited");
        return;
    }
    /* Reply never comes: after the timeout the queue moves on. */
    if (!mesh_radio_settings_next_request(&settings, MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS + 1U,
                                          &request) ||
        request.kind != MESH_ADMIN_GET_OWNER || settings.timeouts != 1U) {
        record_failure(test_name, "timed-out request should be skipped");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 2U, 6000U);

    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    admin.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    snprintf(admin.get_owner_response.short_name, sizeof admin.get_owner_response.short_name, "%s",
             "0ad8");
    meshtastic_MeshPacket reply;
    if (!mesh_test_make_admin_reply(0x10U, 2U, &admin, &reply) ||
        mesh_radio_settings_ingest(&settings, &reply) != 1 || mesh_radio_settings_busy(&settings) ||
        !settings.has_owner || strcmp(settings.owner.short_name, "0ad8") != 0) {
        record_failure(test_name, "owner reply should release the queue");
        return;
    }

    /* Everything: the probe pair plus eight configs and three module configs, no repeats. */
    if (mesh_radio_settings_queue_all(&settings) != 21U || settings.queue_len != 21U) {
        record_failure(test_name, "queue_all should add thirteen requests");
        return;
    }
    if (mesh_radio_settings_queue_all(&settings) != 0U) {
        record_failure(test_name, "queue_all again should add nothing");
        return;
    }
    record_success(test_name);
}

/* The write path in isolation: a save queues passkey refresh, set_*, read-back; the set_*
   carries the full section and the passkey; a Routing reply quoting the id settles it. */
MESH_TEST_CASE(radio_settings_write_queue, unit) {
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);
    settings.has_session_passkey = true;
    settings.session_passkey_len = 8U;
    memset(settings.session_passkey, 0x5AU, 8U);

    struct mesh_admin_request write;
    memset(&write, 0, sizeof write);
    write.kind = MESH_ADMIN_SET_CONFIG;
    write.type = meshtastic_AdminMessage_ConfigType_DISPLAY_CONFIG;
    write.payload.config.which_payload_variant = meshtastic_Config_display_tag;
    write.payload.config.payload_variant.display.screen_on_secs = 300U;
    write.payload.config.payload_variant.display.use_12h_clock = true;

    struct mesh_admin_request bad = write;
    bad.kind = MESH_ADMIN_GET_CONFIG;
    if (mesh_radio_settings_queue_write(&settings, &bad) != -EINVAL) {
        record_failure(test_name, "a get is not a write");
        return;
    }
    if (mesh_radio_settings_queue_write(&settings, &write) != 3 || settings.queue_len != 3U ||
        !mesh_radio_settings_write_pending(&settings)) {
        record_failure(test_name, "a write should queue refresh, set and read-back");
        return;
    }

    struct mesh_admin_request next;
    if (!mesh_radio_settings_next_request(&settings, 1000U, &next) ||
        next.kind != MESH_ADMIN_GET_OWNER || settings.pending_is_write) {
        record_failure(test_name, "the passkey refresh should go first");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 41U, 1000U);
    meshtastic_AdminMessage reply = meshtastic_AdminMessage_init_default;
    reply.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    reply.session_passkey.size = 8U;
    memset(reply.session_passkey.bytes, 0xC3, 8U);
    meshtastic_MeshPacket packet;
    if (!mesh_test_make_admin_reply(0x1234U, 41U, &reply, &packet) ||
        mesh_radio_settings_ingest(&settings, &packet) != 1 ||
        mesh_radio_settings_busy(&settings)) {
        record_failure(test_name, "the owner reply should release the queue");
        return;
    }

    if (!mesh_radio_settings_next_request(&settings, 1100U, &next) ||
        next.kind != MESH_ADMIN_SET_CONFIG || !settings.pending_is_write) {
        record_failure(test_name, "the set_config should follow");
        return;
    }
    next.my_node = 0x1234U;
    next.packet_id = 42U;
    uint8_t buffer[512];
    size_t written = 0U;
    if (mesh_radio_settings_encode_request(&settings, &next, buffer, sizeof buffer, &written) !=
        0) {
        record_failure(test_name, "set_config should encode");
        return;
    }
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    if (!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio) || to_radio.packet.to != 0x1234U ||
        to_radio.packet.id != 42U || !to_radio.packet.decoded.want_response ||
        to_radio.packet.decoded.portnum != meshtastic_PortNum_ADMIN_APP) {
        record_failure(test_name, "set_config packet header is wrong");
        return;
    }
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    if (!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
        admin.which_payload_variant != meshtastic_AdminMessage_set_config_tag ||
        admin.set_config.which_payload_variant != meshtastic_Config_display_tag ||
        admin.set_config.payload_variant.display.screen_on_secs != 300U ||
        !admin.set_config.payload_variant.display.use_12h_clock ||
        admin.session_passkey.size != 8U || admin.session_passkey.bytes[0] != 0xC3U) {
        record_failure(test_name, "set_config should carry the section and the fresh passkey");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 42U, 1100U);
    if (settings.writes_sent != 1U || !mesh_radio_settings_busy(&settings)) {
        record_failure(test_name, "the write should be counted as sent and pending");
        return;
    }

    /* A stray routing reply for another id is not ours; the ack for 42 is. */
    meshtastic_MeshPacket stray = mesh_test_make_routing_reply(7U, meshtastic_Routing_Error_NONE);
    if (mesh_radio_settings_ingest(&settings, &stray) != 0 ||
        !mesh_radio_settings_busy(&settings)) {
        record_failure(test_name, "an unrelated routing reply must be left alone");
        return;
    }
    meshtastic_MeshPacket ack = mesh_test_make_routing_reply(42U, meshtastic_Routing_Error_NONE);
    if (mesh_radio_settings_ingest(&settings, &ack) != 1 || mesh_radio_settings_busy(&settings) ||
        settings.writes_acked != 1U || settings.writes_failed != 0U ||
        mesh_radio_settings_write_pending(&settings) /* only the read-back is left */) {
        record_failure(test_name, "the ack should settle the write");
        return;
    }
    if (!mesh_radio_settings_next_request(&settings, 1200U, &next) ||
        next.kind != MESH_ADMIN_GET_CONFIG ||
        next.type != meshtastic_AdminMessage_ConfigType_DISPLAY_CONFIG ||
        settings.pending_is_write) {
        record_failure(test_name, "the read-back should be last");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 43U, 1200U);
    if (mesh_radio_settings_write_pending(&settings)) {
        record_failure(test_name, "nothing is pending once the read-back is out");
        return;
    }

    /* A rejection is counted with its reason; a timeout with the sentinel. */
    struct mesh_admin_request owner_write;
    memset(&owner_write, 0, sizeof owner_write);
    owner_write.kind = MESH_ADMIN_SET_OWNER;
    snprintf(owner_write.payload.owner.long_name, sizeof owner_write.payload.owner.long_name, "%s",
             "Brick");
    mesh_radio_settings_reset(&settings);
    if (mesh_radio_settings_queue_write(&settings, &owner_write) != 2) {
        record_failure(test_name, "set_owner should share its read-back with the passkey refresh");
        return;
    }
    mesh_radio_settings_next_request(&settings, 1U, &next);
    mesh_radio_settings_mark_sent(&settings, 50U, 1U);
    reply.session_passkey.size = 8U;
    mesh_test_make_admin_reply(0x1234U, 50U, &reply, &packet);
    mesh_radio_settings_ingest(&settings, &packet);
    mesh_radio_settings_next_request(&settings, 2U, &next);
    mesh_radio_settings_mark_sent(&settings, 51U, 2U);
    meshtastic_MeshPacket nak =
        mesh_test_make_routing_reply(51U, meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY);
    if (mesh_radio_settings_ingest(&settings, &nak) != 1 || settings.writes_failed != 1U ||
        settings.last_write_error != (int32_t)meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY) {
        record_failure(test_name, "a rejection should carry its reason");
        return;
    }
    mesh_radio_settings_queue_write(&settings, &owner_write);
    mesh_radio_settings_next_request(&settings, 3U, &next); /* get_owner */
    mesh_radio_settings_mark_sent(&settings, 52U, 3U);
    mesh_test_make_admin_reply(0x1234U, 52U, &reply, &packet);
    mesh_radio_settings_ingest(&settings, &packet);
    mesh_radio_settings_next_request(&settings, 4U, &next); /* set_owner */
    mesh_radio_settings_mark_sent(&settings, 53U, 4U);
    if (mesh_radio_settings_next_request(&settings, 4U + MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS,
                                         &next) ||
        settings.writes_failed != 2U ||
        settings.last_write_error != MESH_RADIO_SETTINGS_WRITE_TIMEOUT) {
        record_failure(test_name, "a silent radio should fail the write with the timeout code");
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(radio_settings_clock_push, unit) {
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);

    if (mesh_radio_settings_queue_time(&settings, 0U) != -EINVAL ||
        mesh_radio_settings_queue_time(&settings, MESH_RADIO_CLOCK_MIN_EPOCH - 1U) != -EINVAL ||
        settings.queue_len != 0U) {
        record_failure(test_name, "a clock we do not believe must not be pushed");
        return;
    }

    const uint32_t epoch = 1788545372U; /* 2026-09-04T17:09:32Z */
    if (mesh_radio_settings_queue_time(&settings, epoch) != 2 || settings.queue_len != 2U) {
        record_failure(test_name, "a clock push should queue the passkey refresh and the set");
        return;
    }
    if (mesh_radio_settings_write_pending(&settings)) {
        record_failure(test_name, "a clock push is not a settings write");
        return;
    }

    struct mesh_admin_request next;
    if (!mesh_radio_settings_next_request(&settings, 1000U, &next) ||
        next.kind != MESH_ADMIN_GET_OWNER) {
        record_failure(test_name, "the passkey refresh should go first");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 61U, 1000U);
    meshtastic_AdminMessage reply = meshtastic_AdminMessage_init_default;
    reply.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    reply.session_passkey.size = 8U;
    memset(reply.session_passkey.bytes, 0xA5, 8U);
    meshtastic_MeshPacket packet;
    if (!mesh_test_make_admin_reply(0x1234U, 61U, &reply, &packet) ||
        mesh_radio_settings_ingest(&settings, &packet) != 1) {
        record_failure(test_name, "the owner reply should release the queue");
        return;
    }

    if (!mesh_radio_settings_next_request(&settings, 1100U, &next) ||
        next.kind != MESH_ADMIN_SET_TIME || next.type != epoch || settings.pending_is_write) {
        record_failure(test_name, "the set_time should follow, uncounted");
        return;
    }
    next.my_node = 0x1234U;
    next.packet_id = 62U;
    uint8_t buffer[512];
    size_t written = 0U;
    if (mesh_radio_settings_encode_request(&settings, &next, buffer, sizeof buffer, &written) !=
        0) {
        record_failure(test_name, "set_time should encode");
        return;
    }
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    if (!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio) || to_radio.packet.to != 0x1234U ||
        to_radio.packet.id != 62U ||
        to_radio.packet.decoded.portnum != meshtastic_PortNum_ADMIN_APP) {
        record_failure(test_name, "set_time packet header is wrong");
        return;
    }
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    if (!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
        admin.which_payload_variant != meshtastic_AdminMessage_set_time_only_tag ||
        admin.set_time_only != epoch || admin.session_passkey.size != 8U ||
        admin.session_passkey.bytes[0] != 0xA5U) {
        record_failure(test_name, "set_time_only should carry the epoch and the fresh passkey");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 62U, 1100U);
    if (settings.writes_sent != 0U) {
        record_failure(test_name, "a clock push must not count as a write sent");
        return;
    }

    /* The firmware acks it like any other set_*; the queue moves on and the counters the save
       toast watches stay exactly where they were. */
    meshtastic_MeshPacket ack = mesh_test_make_routing_reply(62U, meshtastic_Routing_Error_NONE);
    if (mesh_radio_settings_ingest(&settings, &ack) != 1 || mesh_radio_settings_busy(&settings) ||
        settings.writes_acked != 0U || settings.writes_failed != 0U) {
        record_failure(test_name, "the ack should release the queue without counting a save");
        return;
    }

    /* And a radio that never answers costs us a timeout, not a "save failed". A second push
       refreshes the passkey again: the firmware rotates it every 150 s. */
    if (mesh_radio_settings_queue_time(&settings, epoch + 60U) != 2 ||
        !mesh_radio_settings_next_request(&settings, 2000U, &next) ||
        next.kind != MESH_ADMIN_GET_OWNER) {
        record_failure(test_name, "a second push should refresh the passkey again");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 63U, 2000U);
    if (!mesh_radio_settings_next_request(
            &settings, 2000U + MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS + 1U, &next) ||
        next.kind != MESH_ADMIN_SET_TIME) {
        record_failure(test_name, "the set_time should follow the timed-out refresh");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 64U, 8000U);
    mesh_radio_settings_next_request(&settings, 8000U + MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS + 1U,
                                     &next);
    if (settings.timeouts != 2U || settings.writes_failed != 0U) {
        record_failure(test_name, "a silent radio should not fail a save that never happened");
        return;
    }
    record_success(test_name);
}

/* Channels in the core: the table is kept whole, get_channel is one-based on the wire, and a
   set_channel write reads its slot back. */
MESH_TEST_CASE(radio_settings_channel_write, unit) {
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);

    meshtastic_Channel channel = meshtastic_Channel_init_default;
    channel.index = 1;
    channel.role = meshtastic_Channel_Role_SECONDARY;
    channel.has_settings = true;
    snprintf(channel.settings.name, sizeof channel.settings.name, "%s", "Team");
    channel.settings.psk.size = 16U;
    memset(channel.settings.psk.bytes, 0xAB, 16U);
    channel.settings.id = 0x1234U;
    mesh_radio_settings_apply_channel(&settings, &channel);
    meshtastic_Channel bad = channel;
    bad.index = 9;
    mesh_radio_settings_apply_channel(&settings, &bad);
    if (!settings.has_channel[1] || settings.has_channel[0] ||
        strcmp(settings.channels[1].settings.name, "Team") != 0 ||
        !mesh_radio_settings_loaded(&settings)) {
        record_failure(test_name, "apply_channel should keep the slot and ignore bad indices");
        return;
    }

    struct mesh_admin_request get;
    memset(&get, 0, sizeof get);
    get.kind = MESH_ADMIN_GET_CHANNEL;
    get.type = 1U;
    get.my_node = 0x1234U;
    get.packet_id = 7U;
    uint8_t buffer[512];
    size_t written = 0U;
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    pb_istream_t in;
    if (mesh_radio_settings_encode_request(&settings, &get, buffer, sizeof buffer, &written) != 0) {
        record_failure(test_name, "get_channel should encode");
        return;
    }
    in = pb_istream_from_buffer(buffer, written);
    pb_decode(&in, meshtastic_ToRadio_fields, &to_radio);
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    if (!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
        admin.which_payload_variant != meshtastic_AdminMessage_get_channel_request_tag ||
        admin.get_channel_request != 2U) {
        record_failure(test_name, "get_channel_request is index + 1 on the wire");
        return;
    }

    struct mesh_admin_request write;
    memset(&write, 0, sizeof write);
    write.kind = MESH_ADMIN_SET_CHANNEL;
    write.type = 1U;
    write.payload.channel = settings.channels[1];
    write.payload.channel.settings.uplink_enabled = true;
    if (mesh_radio_settings_queue_write(&settings, &write) != 3) {
        record_failure(test_name, "a channel write should queue refresh, set and read-back");
        return;
    }
    struct mesh_admin_request next;
    mesh_radio_settings_next_request(&settings, 1U, &next); /* get_owner */
    mesh_radio_settings_mark_sent(&settings, 10U, 1U);
    meshtastic_AdminMessage reply = meshtastic_AdminMessage_init_default;
    reply.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    reply.session_passkey.size = 8U;
    meshtastic_MeshPacket packet;
    mesh_test_make_admin_reply(0x1234U, 10U, &reply, &packet);
    mesh_radio_settings_ingest(&settings, &packet);
    if (!mesh_radio_settings_next_request(&settings, 2U, &next) ||
        next.kind != MESH_ADMIN_SET_CHANNEL) {
        record_failure(test_name, "set_channel should follow the refresh");
        return;
    }
    next.my_node = 0x1234U;
    next.packet_id = 11U;
    if (mesh_radio_settings_encode_request(&settings, &next, buffer, sizeof buffer, &written) !=
        0) {
        record_failure(test_name, "set_channel should encode");
        return;
    }
    in = pb_istream_from_buffer(buffer, written);
    to_radio = (meshtastic_ToRadio)meshtastic_ToRadio_init_default;
    pb_decode(&in, meshtastic_ToRadio_fields, &to_radio);
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    admin = (meshtastic_AdminMessage)meshtastic_AdminMessage_init_default;
    if (!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
        admin.which_payload_variant != meshtastic_AdminMessage_set_channel_tag ||
        admin.set_channel.index != 1 || !admin.set_channel.settings.uplink_enabled ||
        admin.set_channel.settings.psk.size != 16U || admin.set_channel.settings.id != 0x1234U ||
        admin.session_passkey.size != 8U) {
        record_failure(test_name, "set_channel should carry the whole channel and the passkey");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 11U, 2U);
    meshtastic_MeshPacket ack = mesh_test_make_routing_reply(11U, meshtastic_Routing_Error_NONE);
    mesh_radio_settings_ingest(&settings, &ack);
    if (!mesh_radio_settings_next_request(&settings, 3U, &next) ||
        next.kind != MESH_ADMIN_GET_CHANNEL || next.type != 1U) {
        record_failure(test_name, "the read-back should be get_channel for the same slot");
        return;
    }
    /* A mismatched index is refused before it reaches the radio. */
    write.type = 2U;
    if (mesh_radio_settings_queue_write(&settings, &write) != -EINVAL) {
        record_failure(test_name, "type and channel index must agree");
        return;
    }
    record_success(test_name);
}

/*
 * Pinning a node in the radio's NodeDB. Shaped like the clock push - a passkey refresh then
 * the set, no read-back because there is no get_favorite - and, like the clock push,
 * deliberately not a "settings write": it is a press on the Nodes tab and must not make the
 * Settings tab claim an unsaved section is in flight.
 */
MESH_TEST_CASE(radio_settings_favorite_queue, unit) {
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);

    if (mesh_radio_settings_queue_favorite(&settings, 0U, true) != -EINVAL ||
        settings.queue_len != 0U) {
        record_failure(test_name, "node 0 is not a node");
        return;
    }

    const uint32_t node_id = 0x7A1BU;
    if (mesh_radio_settings_queue_favorite(&settings, node_id, true) != 2 ||
        settings.queue_len != 2U) {
        record_failure(test_name, "a pin should queue the passkey refresh and the set");
        return;
    }
    if (mesh_radio_settings_write_pending(&settings)) {
        record_failure(test_name, "a pin is not a settings write");
        return;
    }

    struct mesh_admin_request next;
    if (!mesh_radio_settings_next_request(&settings, 1000U, &next) ||
        next.kind != MESH_ADMIN_GET_OWNER) {
        record_failure(test_name, "the passkey refresh should go first");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 71U, 1000U);
    meshtastic_AdminMessage reply = meshtastic_AdminMessage_init_default;
    reply.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    reply.session_passkey.size = 8U;
    memset(reply.session_passkey.bytes, 0x5A, 8U);
    meshtastic_MeshPacket packet;
    if (!mesh_test_make_admin_reply(0x1234U, 71U, &reply, &packet) ||
        mesh_radio_settings_ingest(&settings, &packet) != 1) {
        record_failure(test_name, "the owner reply should release the queue");
        return;
    }

    if (!mesh_radio_settings_next_request(&settings, 1100U, &next) ||
        next.kind != MESH_ADMIN_SET_FAVORITE || next.type != node_id || settings.pending_is_write) {
        record_failure(test_name, "the set_favorite should follow, uncounted");
        return;
    }
    next.my_node = 0x1234U;
    next.packet_id = 72U;
    uint8_t buffer[512];
    size_t written = 0U;
    if (mesh_radio_settings_encode_request(&settings, &next, buffer, sizeof buffer, &written) !=
        0) {
        record_failure(test_name, "set_favorite should encode");
        return;
    }
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    if (!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio) ||
        to_radio.packet.decoded.portnum != meshtastic_PortNum_ADMIN_APP) {
        record_failure(test_name, "set_favorite packet header is wrong");
        return;
    }
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    if (!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
        admin.which_payload_variant != meshtastic_AdminMessage_set_favorite_node_tag ||
        admin.set_favorite_node != node_id || admin.session_passkey.size != 8U ||
        admin.session_passkey.bytes[0] != 0x5AU) {
        record_failure(test_name, "set_favorite_node should carry the node and a fresh passkey");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 72U, 1100U);

    meshtastic_MeshPacket ack = mesh_test_make_routing_reply(72U, meshtastic_Routing_Error_NONE);
    if (mesh_radio_settings_ingest(&settings, &ack) != 1 || mesh_radio_settings_busy(&settings) ||
        settings.writes_acked != 0U || settings.writes_sent != 0U) {
        record_failure(test_name, "the ack should release the queue without counting a save");
        return;
    }

    /* Unpinning is the mirror image, and pinning a *different* node is not a duplicate of the
       first request even though both are set_favorite. */
    if (mesh_radio_settings_queue_favorite(&settings, node_id, false) != 2) {
        record_failure(test_name, "an unpin should queue like a pin");
        return;
    }
    if (mesh_radio_settings_queue_favorite(&settings, node_id + 1U, false) != 1) {
        record_failure(test_name, "a second node should not collapse into the first request");
        return;
    }

    record_success(test_name);
}

MESH_TEST_CASE(radio_settings_action_queue, unit) {
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);

    if (mesh_admin_request_is_write(MESH_ADMIN_REBOOT) ||
        !mesh_admin_request_is_action(MESH_ADMIN_FACTORY_RESET_DEVICE) ||
        mesh_admin_request_is_action(MESH_ADMIN_SET_CONFIG)) {
        record_failure(test_name, "an action is not a write and a write is not an action");
        return;
    }
    if (mesh_radio_settings_queue_action(&settings, MESH_ADMIN_SET_CONFIG, 5U) != -EINVAL ||
        mesh_radio_settings_queue_action(&settings, MESH_ADMIN_REBOOT, 0U) != -EINVAL ||
        settings.queue_len != 0U) {
        record_failure(test_name, "only an action with a delay may be queued");
        return;
    }

    if (mesh_radio_settings_queue_action(&settings, MESH_ADMIN_REBOOT,
                                         MESH_RADIO_ACTION_DELAY_SECONDS) != 2 ||
        settings.queue_len != 2U) {
        record_failure(test_name, "a reboot should queue the passkey refresh and the action");
        return;
    }
    if (mesh_radio_settings_write_pending(&settings)) {
        record_failure(test_name, "a radio action is not a settings write");
        return;
    }
    /* A second press before the first has gone out is the same request, not another one. */
    if (mesh_radio_settings_queue_action(&settings, MESH_ADMIN_REBOOT,
                                         MESH_RADIO_ACTION_DELAY_SECONDS) != 0 ||
        settings.queue_len != 2U) {
        record_failure(test_name, "a repeated press should not queue a second reboot");
        return;
    }

    struct mesh_admin_request next;
    if (!mesh_radio_settings_next_request(&settings, 1000U, &next) ||
        next.kind != MESH_ADMIN_GET_OWNER) {
        record_failure(test_name, "the passkey refresh should go first");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 81U, 1000U);
    meshtastic_AdminMessage reply = meshtastic_AdminMessage_init_default;
    reply.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    reply.session_passkey.size = 8U;
    memset(reply.session_passkey.bytes, 0x33, 8U);
    meshtastic_MeshPacket packet;
    if (!mesh_test_make_admin_reply(0x1234U, 81U, &reply, &packet) ||
        mesh_radio_settings_ingest(&settings, &packet) != 1) {
        record_failure(test_name, "the owner reply should release the queue");
        return;
    }

    if (!mesh_radio_settings_next_request(&settings, 1100U, &next) ||
        next.kind != MESH_ADMIN_REBOOT || next.type != MESH_RADIO_ACTION_DELAY_SECONDS ||
        settings.pending_is_write) {
        record_failure(test_name, "the reboot should follow, uncounted");
        return;
    }
    next.my_node = 0x1234U;
    next.packet_id = 82U;
    uint8_t buffer[512];
    size_t written = 0U;
    if (mesh_radio_settings_encode_request(&settings, &next, buffer, sizeof buffer, &written) !=
        0) {
        record_failure(test_name, "the reboot should encode");
        return;
    }
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    if (!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio)) {
        record_failure(test_name, "the reboot ToRadio should decode");
        return;
    }
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    if (!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
        admin.which_payload_variant != meshtastic_AdminMessage_reboot_seconds_tag ||
        admin.reboot_seconds != (int32_t)MESH_RADIO_ACTION_DELAY_SECONDS ||
        admin.session_passkey.size != 8U || admin.session_passkey.bytes[0] != 0x33U) {
        record_failure(test_name, "reboot_seconds should carry the delay and a fresh passkey");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 82U, 1100U);

    meshtastic_MeshPacket ack = mesh_test_make_routing_reply(82U, meshtastic_Routing_Error_NONE);
    if (mesh_radio_settings_ingest(&settings, &ack) != 1 || mesh_radio_settings_busy(&settings) ||
        settings.writes_sent != 0U || settings.writes_acked != 0U) {
        record_failure(test_name, "the ack should release the queue without counting a save");
        return;
    }

    /* A radio that reboots before it answers is the action working, not a failed save: the
       queue moves on after the timeout and nothing is counted against the Settings tab. */
    if (mesh_radio_settings_queue_action(&settings, MESH_ADMIN_SHUTDOWN,
                                         MESH_RADIO_ACTION_DELAY_SECONDS) != 2) {
        record_failure(test_name, "a shutdown should queue like a reboot");
        return;
    }
    (void)mesh_radio_settings_next_request(&settings, 2000U, &next);
    mesh_radio_settings_mark_sent(&settings, 83U, 2000U);
    if (!mesh_radio_settings_next_request(
            &settings, 2000U + MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS + 1U, &next) ||
        settings.writes_failed != 0U) {
        record_failure(test_name, "a timed-out action should not count as a failed write");
        return;
    }

    /* Each verb lands on its own field, and the two the firmware only tests for truth carry
       something true. */
    meshtastic_AdminMessage encoded;
    if (!test_action_encodes_as(MESH_ADMIN_SHUTDOWN, &encoded) ||
        encoded.which_payload_variant != meshtastic_AdminMessage_shutdown_seconds_tag ||
        encoded.shutdown_seconds != (int32_t)MESH_RADIO_ACTION_DELAY_SECONDS) {
        record_failure(test_name, "shutdown_seconds is wrong");
        return;
    }
    if (!test_action_encodes_as(MESH_ADMIN_RESET_NODEDB, &encoded) ||
        encoded.which_payload_variant != meshtastic_AdminMessage_nodedb_reset_tag ||
        !encoded.nodedb_reset) {
        record_failure(test_name, "nodedb_reset is wrong");
        return;
    }
    if (!test_action_encodes_as(MESH_ADMIN_FACTORY_RESET_CONFIG, &encoded) ||
        encoded.which_payload_variant != meshtastic_AdminMessage_factory_reset_config_tag ||
        encoded.factory_reset_config == 0) {
        record_failure(test_name, "factory_reset_config is wrong");
        return;
    }
    if (!test_action_encodes_as(MESH_ADMIN_FACTORY_RESET_DEVICE, &encoded) ||
        encoded.which_payload_variant != meshtastic_AdminMessage_factory_reset_device_tag ||
        encoded.factory_reset_device == 0) {
        record_failure(test_name, "factory_reset_device is wrong");
        return;
    }

    record_success(test_name);
}

/*
 * The NodeDB verbs that arrived with mute and remove, and the pair of them at the session
 * level. `toggle_muted_node` is the one node verb with no wanted state to send, and
 * `remove_by_nodenum` is the one that takes the cached record with it.
 */
MESH_TEST_CASE(radio_settings_node_ops, unit) {
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);

    const uint32_t node_id = 0x2222U;
    if (mesh_radio_settings_queue_remove_node(&settings, 0U) != -EINVAL ||
        mesh_radio_settings_queue_toggle_muted(&settings, 0U) != -EINVAL ||
        settings.queue_len != 0U) {
        record_failure(test_name, "node 0 is not a node");
        return;
    }
    if (mesh_radio_settings_queue_toggle_muted(&settings, node_id) != 2 ||
        mesh_radio_settings_write_pending(&settings)) {
        record_failure(test_name, "a mute should queue the refresh and the verb, uncounted");
        return;
    }
    /* A different node is not a duplicate of the first, but the same one is. */
    if (mesh_radio_settings_queue_toggle_muted(&settings, node_id) != 0 ||
        mesh_radio_settings_queue_toggle_muted(&settings, node_id + 1U) != 1) {
        record_failure(test_name, "mute deduplication is wrong");
        return;
    }

    meshtastic_AdminMessage admin;
    if (!test_admin_encodes(&settings, MESH_ADMIN_TOGGLE_MUTED, node_id, &admin) ||
        admin.which_payload_variant != meshtastic_AdminMessage_toggle_muted_node_tag ||
        admin.toggle_muted_node != node_id) {
        record_failure(test_name, "toggle_muted_node is wrong");
        return;
    }
    if (!test_admin_encodes(&settings, MESH_ADMIN_REMOVE_NODE, node_id, &admin) ||
        admin.which_payload_variant != meshtastic_AdminMessage_remove_by_nodenum_tag ||
        admin.remove_by_nodenum != node_id) {
        record_failure(test_name, "remove_by_nodenum is wrong");
        return;
    }

    /* At the session level: removing a node takes it out of the cache, because there is no
       read-back that would ever take it out again. */
    struct mesh_session session;
    mesh_session_init(&session);
    struct mesh_test_trace_capture capture;
    memset(&capture, 0, sizeof capture);
    mesh_session_attach(&session, mesh_test_trace_capture_fn, &capture);

    meshtastic_FromRadio my_info = meshtastic_FromRadio_init_default;
    my_info.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    my_info.my_info.my_node_num = 0x1111U;
    meshtastic_FromRadio one = meshtastic_FromRadio_init_default;
    one.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    one.node_info.num = 0x2222U;
    one.node_info.is_muted = true;
    meshtastic_FromRadio two = meshtastic_FromRadio_init_default;
    two.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    two.node_info.num = 0x3333U;
    if (!mesh_test_session_feed_from_radio(&session, &my_info) ||
        !mesh_test_session_feed_from_radio(&session, &one) ||
        !mesh_test_session_feed_from_radio(&session, &two)) {
        record_failure(test_name, "seeding the session failed");
        return;
    }

    const struct mesh_node_summary *node = mesh_test_session_find_node(&session, 0x2222U);
    if (node == NULL || !node->is_muted) {
        record_failure(test_name, "NodeInfo.is_muted should reach the cache");
        return;
    }
    if (mesh_session_toggle_node_muted(&session, 0x2222U) <= 0) {
        record_failure(test_name, "the mute toggle was not queued");
        return;
    }
    node = mesh_test_session_find_node(&session, 0x2222U);
    if (node == NULL || node->is_muted) {
        record_failure(test_name, "the cached muted flag should follow the toggle");
        return;
    }

    /* Pressed again before the first has gone out: the queue deduplicates it, so the cached
       flag must not move a second time. One toggle on the wire, one flip here. */
    if (mesh_session_toggle_node_muted(&session, 0x2222U) != 0) {
        record_failure(test_name, "a repeated mute should deduplicate rather than queue again");
        return;
    }
    node = mesh_test_session_find_node(&session, 0x2222U);
    if (node == NULL || node->is_muted) {
        record_failure(test_name, "a deduplicated mute should leave the cached flag alone");
        return;
    }

    if (mesh_session_remove_node(&session, 0x1111U) != -EINVAL ||
        mesh_session_remove_node(&session, 0x9999U) != -ENOENT) {
        record_failure(test_name, "removing ourselves or an unknown node should be refused");
        return;
    }
    const size_t before = mesh_session_handshake(&session)->node_count;
    if (mesh_session_remove_node(&session, 0x2222U) <= 0) {
        record_failure(test_name, "the remove was not queued");
        return;
    }
    /* The one behind it moves up rather than leaving a hole in the middle of the list. */
    if (mesh_session_handshake(&session)->node_count != before - 1U ||
        mesh_test_session_find_node(&session, 0x2222U) != NULL ||
        mesh_test_session_find_node(&session, 0x3333U) == NULL) {
        record_failure(test_name, "the removed node should leave the cache compacted");
        return;
    }

    record_success(test_name);
}

/*
 * Fixed position. The one pair of admin verbs that is a write without being a set_config: the
 * firmware sets PositionConfig's own flag as a side effect, which is exactly why the read-back
 * behind them is a get_config POSITION.
 */
MESH_TEST_CASE(radio_settings_fixed_position, unit) {
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);

    if (!mesh_admin_request_is_write(MESH_ADMIN_SET_FIXED_POSITION) ||
        !mesh_admin_request_is_write(MESH_ADMIN_REMOVE_FIXED_POSITION) ||
        mesh_admin_request_is_action(MESH_ADMIN_SET_FIXED_POSITION)) {
        record_failure(test_name, "fixed position is a write, not a radio action");
        return;
    }

    struct mesh_admin_request write;
    memset(&write, 0, sizeof write);
    write.kind = MESH_ADMIN_SET_FIXED_POSITION;
    write.type = (uint32_t)meshtastic_AdminMessage_ConfigType_POSITION_CONFIG;
    write.payload.position.has_latitude_i = true;
    write.payload.position.latitude_i = 446488000;
    write.payload.position.has_longitude_i = true;
    write.payload.position.longitude_i = -635752000;
    write.payload.position.has_altitude = true;
    write.payload.position.altitude = 12;
    write.payload.position.location_source = meshtastic_Position_LocSource_LOC_MANUAL;

    /* A read-back for the wrong section would leave the flag the firmware just moved unread. */
    struct mesh_admin_request wrong = write;
    wrong.type = (uint32_t)meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG;
    if (mesh_radio_settings_queue_write(&settings, &wrong) != -EINVAL) {
        record_failure(test_name, "the read-back must be the Position section");
        return;
    }

    if (mesh_radio_settings_queue_write(&settings, &write) != 3 ||
        !mesh_radio_settings_write_pending(&settings)) {
        record_failure(test_name, "a fixed position should queue refresh, write and read-back");
        return;
    }
    struct mesh_admin_request next;
    if (!mesh_radio_settings_next_request(&settings, 1000U, &next) ||
        next.kind != MESH_ADMIN_GET_OWNER) {
        record_failure(test_name, "the passkey refresh should go first");
        return;
    }
    mesh_radio_settings_mark_sent(&settings, 91U, 1000U);
    meshtastic_AdminMessage reply = meshtastic_AdminMessage_init_default;
    reply.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    reply.session_passkey.size = 8U;
    memset(reply.session_passkey.bytes, 0x7EU, 8U);
    meshtastic_MeshPacket packet;
    if (!mesh_test_make_admin_reply(0x1234U, 91U, &reply, &packet) ||
        mesh_radio_settings_ingest(&settings, &packet) != 1) {
        record_failure(test_name, "the owner reply should release the queue");
        return;
    }
    if (!mesh_radio_settings_next_request(&settings, 1100U, &next) ||
        next.kind != MESH_ADMIN_SET_FIXED_POSITION || !settings.pending_is_write) {
        record_failure(test_name, "the write should follow, counted as a save");
        return;
    }
    next.my_node = 0x1234U;
    next.packet_id = 92U;
    uint8_t buffer[512];
    size_t written = 0U;
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    if (mesh_radio_settings_encode_request(&settings, &next, buffer, sizeof buffer, &written) !=
        0) {
        record_failure(test_name, "set_fixed_position should encode");
        return;
    }
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    if (!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio)) {
        record_failure(test_name, "the ToRadio should decode");
        return;
    }
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    if (!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
        admin.which_payload_variant != meshtastic_AdminMessage_set_fixed_position_tag ||
        admin.set_fixed_position.latitude_i != 446488000 ||
        admin.set_fixed_position.longitude_i != -635752000 ||
        admin.set_fixed_position.altitude != 12 ||
        admin.set_fixed_position.location_source != meshtastic_Position_LocSource_LOC_MANUAL ||
        admin.session_passkey.size != 8U) {
        record_failure(test_name, "set_fixed_position should carry the fix and a fresh passkey");
        return;
    }
    /* Third in the queue: the section whose flag the firmware moved behind our back. */
    mesh_radio_settings_mark_sent(&settings, 92U, 1100U);
    meshtastic_MeshPacket ack = mesh_test_make_routing_reply(92U, meshtastic_Routing_Error_NONE);
    if (mesh_radio_settings_ingest(&settings, &ack) != 1 || settings.writes_acked != 1U) {
        record_failure(test_name, "the ack should count as a save");
        return;
    }
    if (!mesh_radio_settings_next_request(&settings, 1200U, &next) ||
        next.kind != MESH_ADMIN_GET_CONFIG ||
        next.type != (uint32_t)meshtastic_AdminMessage_ConfigType_POSITION_CONFIG) {
        record_failure(test_name, "the read-back should be get_config POSITION");
        return;
    }

    /* A position with no coordinates would turn fixed position on with nothing behind it. */
    struct mesh_admin_request empty;
    memset(&empty, 0, sizeof empty);
    empty.kind = MESH_ADMIN_SET_FIXED_POSITION;
    empty.type = (uint32_t)meshtastic_AdminMessage_ConfigType_POSITION_CONFIG;
    empty.my_node = 0x1234U;
    empty.packet_id = 93U;
    if (mesh_radio_settings_encode_request(&settings, &empty, buffer, sizeof buffer, &written) !=
        -EINVAL) {
        record_failure(test_name, "a fix with no coordinates should be refused");
        return;
    }

    record_success(test_name);
}
