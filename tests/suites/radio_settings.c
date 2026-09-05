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
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio),
                      "request is not a ToRadio");
    MESH_TEST_FAIL_IF(to_radio.which_payload_variant != meshtastic_ToRadio_packet_tag ||
                          to_radio.packet.to != my_node || to_radio.packet.id != 77U ||
                          to_radio.packet.which_payload_variant !=
                              meshtastic_MeshPacket_decoded_tag ||
                          to_radio.packet.decoded.portnum != meshtastic_PortNum_ADMIN_APP ||
                          !to_radio.packet.decoded.want_response,
                      "request packet header is wrong");
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    MESH_TEST_FAIL_IF(
        !pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
            admin.which_payload_variant != meshtastic_AdminMessage_get_config_request_tag ||
            admin.get_config_request != meshtastic_AdminMessage_ConfigType_LORA_CONFIG ||
            admin.session_passkey.size != 0U,
        "request AdminMessage is wrong");

    /* The reply: LoRa config plus a session passkey, quoting our request id. */
    mesh_radio_settings_mark_sent(&settings, 77U, 1000U);
    MESH_TEST_FAIL_IF(!mesh_radio_settings_busy(&settings), "should be busy after mark_sent");
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
    MESH_TEST_FAIL_IF(!mesh_test_make_admin_reply(my_node, 77U, &admin, &reply),
                      "encode reply failed");
    MESH_TEST_FAIL_IF(mesh_radio_settings_ingest(&settings, &reply) != 1,
                      "admin reply should be consumed");
    MESH_TEST_FAIL_IF(!settings.has_lora ||
                          settings.lora.region != meshtastic_Config_LoRaConfig_RegionCode_US ||
                          settings.lora.hop_limit != 3U || !settings.has_session_passkey ||
                          settings.session_passkey_len != sizeof k_passkey ||
                          memcmp(settings.session_passkey, k_passkey, sizeof k_passkey) != 0 ||
                          settings.admin_replies != 1U || mesh_radio_settings_busy(&settings) ||
                          !mesh_radio_settings_loaded(&settings),
                      "reply was not folded in");

    /* The next request carries the passkey. */
    request.kind = MESH_ADMIN_GET_OWNER;
    request.packet_id = 78U;
    MESH_TEST_FAIL_IF(
        mesh_radio_settings_encode_request(&settings, &request, wire, sizeof wire, &wire_len) != 0,
        "second encode failed");
    to_radio = (meshtastic_ToRadio)meshtastic_ToRadio_init_default;
    in = pb_istream_from_buffer(wire, wire_len);
    admin = (meshtastic_AdminMessage)meshtastic_AdminMessage_init_default;
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio),
                      "second request is not a ToRadio");
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
                          admin.which_payload_variant !=
                              meshtastic_AdminMessage_get_owner_request_tag ||
                          admin.session_passkey.size != sizeof k_passkey ||
                          memcmp(admin.session_passkey.bytes, k_passkey, sizeof k_passkey) != 0,
                      "second request should carry the passkey");

    /* A text message is not ours to consume. */
    meshtastic_MeshPacket text = mesh_test_make_decoded_packet(
        0x1234U, my_node, 0U, 99U, meshtastic_PortNum_TEXT_MESSAGE_APP, "hi", 2U);
    MESH_TEST_FAIL_IF(mesh_radio_settings_ingest(&settings, &text) != 0,
                      "text packet should be left alone");

    record_success(test_name);
}

/*
 * The module table: one row per ModuleConfig section, each pairing an admin ModuleConfigType
 * with the union tag and the storage it belongs to.
 *
 * The pairing is what this guards. Before the table, a section's type and its tag were typed
 * out separately in the write builder, and a mismatched pair wrote correct bytes into the wrong
 * module - which the radio accepts without an error, so nothing downstream would have caught
 * it. Every row is round-tripped here: bytes in through apply, bytes out through load, with the
 * tag checked on the way out and every other module checked to have stayed empty.
 */
MESH_TEST_CASE(radio_module_table, unit) {
    const size_t count = mesh_radio_module_count();
    MESH_TEST_FAIL_IF(count == 0U, "the module table should not be empty");

    /*
     * What each row must actually say, written out here from meshtastic/module_config.proto and
     * meshtastic/admin.proto rather than read back out of the table under test - the same rule
     * message_encode_text_golden follows. A round trip that takes its input from the row and
     * compares the output to that same row proves only that the table is used consistently; it
     * passes a row whose tag is wrong but unique, which is the mistake worth catching.
     *
     * Numbers are spelled out beside the symbols so a row can be checked against the .proto by
     * eye: the ModuleConfigType enum counts from 0 and the ModuleConfig field numbers from 1.
     */
    static const struct {
        uint32_t admin_type;
        uint32_t variant_tag;
    } k_expected[] = {
        {0U, 1U},   /* MQTT_CONFIG            <-> mqtt = 1 */
        {3U, 4U},   /* STOREFORWARD_CONFIG    <-> store_forward = 4 */
        {5U, 6U},   /* TELEMETRY_CONFIG       <-> telemetry = 6 */
        {9U, 10U},  /* NEIGHBORINFO_CONFIG    <-> neighbor_info = 10 */
        {4U, 5U},   /* RANGETEST_CONFIG       <-> range_test = 5 */
        {12U, 13U}, /* PAXCOUNTER_CONFIG      <-> paxcounter = 13 */
        {15U, 16U}, /* TAK_CONFIG             <-> tak = 16 */
        {10U, 11U}, /* AMBIENTLIGHTING_CONFIG <-> ambient_lighting = 11 */
        {13U, 14U}, /* STATUSMESSAGE_CONFIG   <-> statusmessage = 14 */
    };
    MESH_TEST_FAIL_IF(count != sizeof k_expected / sizeof k_expected[0],
                      "a module was added or removed without updating the expected pairs");
    for (size_t i = 0; i < count; ++i) {
        const uint32_t admin = k_expected[i].admin_type;
        const struct mesh_module_binding *row = mesh_radio_module_for_type(admin);
        MESH_TEST_FAIL_IF(row == NULL, "an expected module has no row");
        MESH_TEST_FAIL_IF(row->variant_tag != k_expected[i].variant_tag,
                          "a module row pairs its admin type with the wrong union tag");
    }

    /*
     * And a cross-check that costs nothing and covers a row added later without this test being
     * updated: upstream orders the ModuleConfigType enum to match the ModuleConfig field numbers,
     * so the tag is always the admin type plus one, for all seventeen modules - not only the ones
     * kept here. It is an ordering convention rather than a guarantee, so if it ever fires the
     * answer is to check the new module against the .proto by hand, not to delete this.
     */
    for (size_t i = 0; i < count; ++i) {
        const struct mesh_module_binding *row = mesh_radio_module_at(i);
        MESH_TEST_FAIL_IF(row->variant_tag != row->admin_type + 1U,
                          "a module row breaks the upstream admin-type/field-number ordering");
    }

    /* No two rows may claim the same admin type or the same union tag: either would make one
       module unreachable and silently shadow it with another. */
    for (size_t i = 0; i < count; ++i) {
        const struct mesh_module_binding *a = mesh_radio_module_at(i);
        MESH_TEST_FAIL_IF(a == NULL || a->size == 0U, "a module row should name real storage");
        MESH_TEST_FAIL_IF(mesh_radio_module_for_type(a->admin_type) != a,
                          "a module row should be the one found for its own admin type");
        for (size_t j = i + 1U; j < count; ++j) {
            const struct mesh_module_binding *b = mesh_radio_module_at(j);
            MESH_TEST_FAIL_IF(a->admin_type == b->admin_type,
                              "two module rows share an admin ModuleConfigType");
            MESH_TEST_FAIL_IF(a->variant_tag == b->variant_tag,
                              "two module rows share a payload_variant tag");
            MESH_TEST_FAIL_IF(a->has_offset == b->has_offset || a->store_offset == b->store_offset,
                              "two module rows share storage");
        }
    }

    /* Each row, one at a time: a recognisable payload in, the same payload and tag back out,
       and nothing else in the struct touched. */
    for (size_t i = 0; i < count; ++i) {
        const struct mesh_module_binding *binding = mesh_radio_module_at(i);
        struct mesh_radio_settings radio;
        mesh_radio_settings_reset(&radio);

        meshtastic_ModuleConfig config;
        memset(&config, 0, sizeof config);
        config.which_payload_variant = (pb_size_t)binding->variant_tag;
        uint8_t pattern[sizeof config.payload_variant];
        for (size_t b = 0; b < binding->size; ++b) {
            pattern[b] = (uint8_t)(0x40U + i + b);
        }
        memcpy(&config.payload_variant, pattern, binding->size);

        mesh_radio_settings_apply_module_config(&radio, &config);
        MESH_TEST_FAIL_IF(!mesh_radio_module_held(&radio, binding),
                          "applying a module config should mark that module held");
        MESH_TEST_FAIL_IF(!mesh_radio_settings_loaded(&radio),
                          "one module on its own should count as loaded");
        for (size_t j = 0; j < count; ++j) {
            if (j == i) {
                continue;
            }
            MESH_TEST_FAIL_IF(mesh_radio_module_held(&radio, mesh_radio_module_at(j)),
                              "applying one module should not mark another held");
        }

        meshtastic_ModuleConfig out;
        MESH_TEST_FAIL_IF(!mesh_radio_module_load(&radio, binding, &out),
                          "a held module should load back");
        MESH_TEST_FAIL_IF((uint32_t)out.which_payload_variant != binding->variant_tag,
                          "a module should load back under its own tag");
        MESH_TEST_FAIL_IF(memcmp(&out.payload_variant, pattern, binding->size) != 0,
                          "a module should load back the bytes it was given");
    }

    /* And a module nobody has sent does not load, which is the -ENOENT a save reports. */
    struct mesh_radio_settings empty;
    mesh_radio_settings_reset(&empty);
    meshtastic_ModuleConfig out;
    MESH_TEST_FAIL_IF(mesh_radio_module_load(&empty, mesh_radio_module_at(0U), &out),
                      "a module the radio has not sent should not load");
    MESH_TEST_FAIL_IF(mesh_radio_module_for_type(0xFFFFU) != NULL,
                      "an admin type this client does not keep should have no row");
    record_success(test_name);
}

MESH_TEST_CASE(radio_settings_fetch_queue, unit) {
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);

    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_probe(&settings) != 2U ||
                          mesh_radio_settings_queue_probe(&settings) != 0U,
                      "probe should queue two requests once");

    struct mesh_admin_request request;
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 0U, &request) ||
                          request.kind != MESH_ADMIN_GET_METADATA,
                      "first request should be metadata");
    mesh_radio_settings_mark_sent(&settings, 1U, 0U);
    MESH_TEST_FAIL_IF(mesh_radio_settings_next_request(&settings, 100U, &request),
                      "nothing should go out while a reply is awaited");
    /* Reply never comes: after the timeout the queue moves on. */
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(
                          &settings, MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS + 1U, &request) ||
                          request.kind != MESH_ADMIN_GET_OWNER || settings.timeouts != 1U,
                      "timed-out request should be skipped");
    mesh_radio_settings_mark_sent(&settings, 2U, 6000U);

    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    admin.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    snprintf(admin.get_owner_response.short_name, sizeof admin.get_owner_response.short_name, "%s",
             "0ad8");
    meshtastic_MeshPacket reply;
    MESH_TEST_FAIL_IF(!mesh_test_make_admin_reply(0x10U, 2U, &admin, &reply) ||
                          mesh_radio_settings_ingest(&settings, &reply) != 1 ||
                          mesh_radio_settings_busy(&settings) || !settings.has_owner ||
                          strcmp(settings.owner.short_name, "0ad8") != 0,
                      "owner reply should release the queue");

    /* Everything, and each thing once: the probe pair, then one per Config section, one per
       ModuleConfig section this client keeps, and one per channel slot. Spelled out as the sum
       rather than as a bare number, because the module count is what every phase moves. */
    const size_t expected = 2U   /* metadata + owner */
                            + 8U /* Config sections */
                            + 9U /* ModuleConfig sections */
                            + MESH_RADIO_SETTINGS_MAX_CHANNELS;
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_all(&settings) != expected ||
                          settings.queue_len != expected,
                      "queue_all should add one request per section and channel");
    MESH_TEST_FAIL_IF(expected > MESH_RADIO_SETTINGS_FETCH_MAX,
                      "a full refresh must fit the queue: enqueue drops silently when it is full");
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_all(&settings) != 0U,
                      "queue_all again should add nothing");
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
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_write(&settings, &bad) != -EINVAL,
                      "a get is not a write");
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_write(&settings, &write) != 3 ||
                          settings.queue_len != 3U || !mesh_radio_settings_write_pending(&settings),
                      "a write should queue refresh, set and read-back");

    struct mesh_admin_request next;
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 1000U, &next) ||
                          next.kind != MESH_ADMIN_GET_OWNER || settings.pending_is_write,
                      "the passkey refresh should go first");
    mesh_radio_settings_mark_sent(&settings, 41U, 1000U);
    meshtastic_AdminMessage reply = meshtastic_AdminMessage_init_default;
    reply.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    reply.session_passkey.size = 8U;
    memset(reply.session_passkey.bytes, 0xC3, 8U);
    meshtastic_MeshPacket packet;
    MESH_TEST_FAIL_IF(!mesh_test_make_admin_reply(0x1234U, 41U, &reply, &packet) ||
                          mesh_radio_settings_ingest(&settings, &packet) != 1 ||
                          mesh_radio_settings_busy(&settings),
                      "the owner reply should release the queue");

    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 1100U, &next) ||
                          next.kind != MESH_ADMIN_SET_CONFIG || !settings.pending_is_write,
                      "the set_config should follow");
    next.my_node = 0x1234U;
    next.packet_id = 42U;
    uint8_t buffer[512];
    size_t written = 0U;
    MESH_TEST_FAIL_IF(
        mesh_radio_settings_encode_request(&settings, &next, buffer, sizeof buffer, &written) != 0,
        "set_config should encode");
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio) ||
                          to_radio.packet.to != 0x1234U || to_radio.packet.id != 42U ||
                          !to_radio.packet.decoded.want_response ||
                          to_radio.packet.decoded.portnum != meshtastic_PortNum_ADMIN_APP,
                      "set_config packet header is wrong");
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
                          admin.which_payload_variant != meshtastic_AdminMessage_set_config_tag ||
                          admin.set_config.which_payload_variant != meshtastic_Config_display_tag ||
                          admin.set_config.payload_variant.display.screen_on_secs != 300U ||
                          !admin.set_config.payload_variant.display.use_12h_clock ||
                          admin.session_passkey.size != 8U ||
                          admin.session_passkey.bytes[0] != 0xC3U,
                      "set_config should carry the section and the fresh passkey");
    mesh_radio_settings_mark_sent(&settings, 42U, 1100U);
    MESH_TEST_FAIL_IF(settings.writes_sent != 1U || !mesh_radio_settings_busy(&settings),
                      "the write should be counted as sent and pending");

    /* A stray routing reply for another id is not ours; the ack for 42 is. */
    meshtastic_MeshPacket stray = mesh_test_make_routing_reply(7U, meshtastic_Routing_Error_NONE);
    MESH_TEST_FAIL_IF(mesh_radio_settings_ingest(&settings, &stray) != 0 ||
                          !mesh_radio_settings_busy(&settings),
                      "an unrelated routing reply must be left alone");
    meshtastic_MeshPacket ack = mesh_test_make_routing_reply(42U, meshtastic_Routing_Error_NONE);
    MESH_TEST_FAIL_IF(
        mesh_radio_settings_ingest(&settings, &ack) != 1 || mesh_radio_settings_busy(&settings) ||
            settings.writes_acked != 1U || settings.writes_failed != 0U ||
            mesh_radio_settings_write_pending(&settings) /* only the read-back is left */,
        "the ack should settle the write");
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 1200U, &next) ||
                          next.kind != MESH_ADMIN_GET_CONFIG ||
                          next.type != meshtastic_AdminMessage_ConfigType_DISPLAY_CONFIG ||
                          settings.pending_is_write,
                      "the read-back should be last");
    mesh_radio_settings_mark_sent(&settings, 43U, 1200U);
    MESH_TEST_FAIL_IF(mesh_radio_settings_write_pending(&settings),
                      "nothing is pending once the read-back is out");

    /* A rejection is counted with its reason; a timeout with the sentinel. */
    struct mesh_admin_request owner_write;
    memset(&owner_write, 0, sizeof owner_write);
    owner_write.kind = MESH_ADMIN_SET_OWNER;
    snprintf(owner_write.payload.owner.long_name, sizeof owner_write.payload.owner.long_name, "%s",
             "Brick");
    mesh_radio_settings_reset(&settings);
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_write(&settings, &owner_write) != 2,
                      "set_owner should share its read-back with the passkey refresh");
    mesh_radio_settings_next_request(&settings, 1U, &next);
    mesh_radio_settings_mark_sent(&settings, 50U, 1U);
    reply.session_passkey.size = 8U;
    mesh_test_make_admin_reply(0x1234U, 50U, &reply, &packet);
    mesh_radio_settings_ingest(&settings, &packet);
    mesh_radio_settings_next_request(&settings, 2U, &next);
    mesh_radio_settings_mark_sent(&settings, 51U, 2U);
    meshtastic_MeshPacket nak =
        mesh_test_make_routing_reply(51U, meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY);
    MESH_TEST_FAIL_IF(
        mesh_radio_settings_ingest(&settings, &nak) != 1 || settings.writes_failed != 1U ||
            settings.last_write_error != (int32_t)meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY,
        "a rejection should carry its reason");
    mesh_radio_settings_queue_write(&settings, &owner_write);
    mesh_radio_settings_next_request(&settings, 3U, &next); /* get_owner */
    mesh_radio_settings_mark_sent(&settings, 52U, 3U);
    mesh_test_make_admin_reply(0x1234U, 52U, &reply, &packet);
    mesh_radio_settings_ingest(&settings, &packet);
    mesh_radio_settings_next_request(&settings, 4U, &next); /* set_owner */
    mesh_radio_settings_mark_sent(&settings, 53U, 4U);
    MESH_TEST_FAIL_IF(mesh_radio_settings_next_request(
                          &settings, 4U + MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS, &next) ||
                          settings.writes_failed != 2U ||
                          settings.last_write_error != MESH_RADIO_SETTINGS_WRITE_TIMEOUT,
                      "a silent radio should fail the write with the timeout code");
    record_success(test_name);
}

MESH_TEST_CASE(radio_settings_clock_push, unit) {
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);

    MESH_TEST_FAIL_IF(
        mesh_radio_settings_queue_time(&settings, 0U) != -EINVAL ||
            mesh_radio_settings_queue_time(&settings, MESH_RADIO_CLOCK_MIN_EPOCH - 1U) != -EINVAL ||
            settings.queue_len != 0U,
        "a clock we do not believe must not be pushed");

    const uint32_t epoch = 1788545372U; /* 2026-09-04T17:09:32Z */
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_time(&settings, epoch) != 2 ||
                          settings.queue_len != 2U,
                      "a clock push should queue the passkey refresh and the set");
    MESH_TEST_FAIL_IF(mesh_radio_settings_write_pending(&settings),
                      "a clock push is not a settings write");

    struct mesh_admin_request next;
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 1000U, &next) ||
                          next.kind != MESH_ADMIN_GET_OWNER,
                      "the passkey refresh should go first");
    mesh_radio_settings_mark_sent(&settings, 61U, 1000U);
    meshtastic_AdminMessage reply = meshtastic_AdminMessage_init_default;
    reply.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    reply.session_passkey.size = 8U;
    memset(reply.session_passkey.bytes, 0xA5, 8U);
    meshtastic_MeshPacket packet;
    MESH_TEST_FAIL_IF(!mesh_test_make_admin_reply(0x1234U, 61U, &reply, &packet) ||
                          mesh_radio_settings_ingest(&settings, &packet) != 1,
                      "the owner reply should release the queue");

    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 1100U, &next) ||
                          next.kind != MESH_ADMIN_SET_TIME || next.type != epoch ||
                          settings.pending_is_write,
                      "the set_time should follow, uncounted");
    next.my_node = 0x1234U;
    next.packet_id = 62U;
    uint8_t buffer[512];
    size_t written = 0U;
    MESH_TEST_FAIL_IF(
        mesh_radio_settings_encode_request(&settings, &next, buffer, sizeof buffer, &written) != 0,
        "set_time should encode");
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio) ||
                          to_radio.packet.to != 0x1234U || to_radio.packet.id != 62U ||
                          to_radio.packet.decoded.portnum != meshtastic_PortNum_ADMIN_APP,
                      "set_time packet header is wrong");
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
                          admin.which_payload_variant !=
                              meshtastic_AdminMessage_set_time_only_tag ||
                          admin.set_time_only != epoch || admin.session_passkey.size != 8U ||
                          admin.session_passkey.bytes[0] != 0xA5U,
                      "set_time_only should carry the epoch and the fresh passkey");
    mesh_radio_settings_mark_sent(&settings, 62U, 1100U);
    MESH_TEST_FAIL_IF(settings.writes_sent != 0U, "a clock push must not count as a write sent");

    /* The firmware acks it like any other set_*; the queue moves on and the counters the save
       toast watches stay exactly where they were. */
    meshtastic_MeshPacket ack = mesh_test_make_routing_reply(62U, meshtastic_Routing_Error_NONE);
    MESH_TEST_FAIL_IF(mesh_radio_settings_ingest(&settings, &ack) != 1 ||
                          mesh_radio_settings_busy(&settings) || settings.writes_acked != 0U ||
                          settings.writes_failed != 0U,
                      "the ack should release the queue without counting a save");

    /* And a radio that never answers costs us a timeout, not a "save failed". A second push
       refreshes the passkey again: the firmware rotates it every 150 s. */
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_time(&settings, epoch + 60U) != 2 ||
                          !mesh_radio_settings_next_request(&settings, 2000U, &next) ||
                          next.kind != MESH_ADMIN_GET_OWNER,
                      "a second push should refresh the passkey again");
    mesh_radio_settings_mark_sent(&settings, 63U, 2000U);
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(
                          &settings, 2000U + MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS + 1U, &next) ||
                          next.kind != MESH_ADMIN_SET_TIME,
                      "the set_time should follow the timed-out refresh");
    mesh_radio_settings_mark_sent(&settings, 64U, 8000U);
    mesh_radio_settings_next_request(&settings, 8000U + MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS + 1U,
                                     &next);
    MESH_TEST_FAIL_IF(settings.timeouts != 2U || settings.writes_failed != 0U,
                      "a silent radio should not fail a save that never happened");
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
    MESH_TEST_FAIL_IF(!settings.has_channel[1] || settings.has_channel[0] ||
                          strcmp(settings.channels[1].settings.name, "Team") != 0 ||
                          !mesh_radio_settings_loaded(&settings),
                      "apply_channel should keep the slot and ignore bad indices");

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
    MESH_TEST_FAIL_IF(
        mesh_radio_settings_encode_request(&settings, &get, buffer, sizeof buffer, &written) != 0,
        "get_channel should encode");
    in = pb_istream_from_buffer(buffer, written);
    pb_decode(&in, meshtastic_ToRadio_fields, &to_radio);
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
                          admin.which_payload_variant !=
                              meshtastic_AdminMessage_get_channel_request_tag ||
                          admin.get_channel_request != 2U,
                      "get_channel_request is index + 1 on the wire");

    struct mesh_admin_request write;
    memset(&write, 0, sizeof write);
    write.kind = MESH_ADMIN_SET_CHANNEL;
    write.type = 1U;
    write.payload.channel = settings.channels[1];
    write.payload.channel.settings.uplink_enabled = true;
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_write(&settings, &write) != 3,
                      "a channel write should queue refresh, set and read-back");
    struct mesh_admin_request next;
    mesh_radio_settings_next_request(&settings, 1U, &next); /* get_owner */
    mesh_radio_settings_mark_sent(&settings, 10U, 1U);
    meshtastic_AdminMessage reply = meshtastic_AdminMessage_init_default;
    reply.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    reply.session_passkey.size = 8U;
    meshtastic_MeshPacket packet;
    mesh_test_make_admin_reply(0x1234U, 10U, &reply, &packet);
    mesh_radio_settings_ingest(&settings, &packet);
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 2U, &next) ||
                          next.kind != MESH_ADMIN_SET_CHANNEL,
                      "set_channel should follow the refresh");
    next.my_node = 0x1234U;
    next.packet_id = 11U;
    MESH_TEST_FAIL_IF(
        mesh_radio_settings_encode_request(&settings, &next, buffer, sizeof buffer, &written) != 0,
        "set_channel should encode");
    in = pb_istream_from_buffer(buffer, written);
    to_radio = (meshtastic_ToRadio)meshtastic_ToRadio_init_default;
    pb_decode(&in, meshtastic_ToRadio_fields, &to_radio);
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    admin = (meshtastic_AdminMessage)meshtastic_AdminMessage_init_default;
    MESH_TEST_FAIL_IF(
        !pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
            admin.which_payload_variant != meshtastic_AdminMessage_set_channel_tag ||
            admin.set_channel.index != 1 || !admin.set_channel.settings.uplink_enabled ||
            admin.set_channel.settings.psk.size != 16U ||
            admin.set_channel.settings.id != 0x1234U || admin.session_passkey.size != 8U,
        "set_channel should carry the whole channel and the passkey");
    mesh_radio_settings_mark_sent(&settings, 11U, 2U);
    meshtastic_MeshPacket ack = mesh_test_make_routing_reply(11U, meshtastic_Routing_Error_NONE);
    mesh_radio_settings_ingest(&settings, &ack);
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 3U, &next) ||
                          next.kind != MESH_ADMIN_GET_CHANNEL || next.type != 1U,
                      "the read-back should be get_channel for the same slot");
    /* A mismatched index is refused before it reaches the radio. */
    write.type = 2U;
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_write(&settings, &write) != -EINVAL,
                      "type and channel index must agree");
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

    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_favorite(&settings, 0U, true) != -EINVAL ||
                          settings.queue_len != 0U,
                      "node 0 is not a node");

    const uint32_t node_id = 0x7A1BU;
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_favorite(&settings, node_id, true) != 2 ||
                          settings.queue_len != 2U,
                      "a pin should queue the passkey refresh and the set");
    MESH_TEST_FAIL_IF(mesh_radio_settings_write_pending(&settings),
                      "a pin is not a settings write");

    struct mesh_admin_request next;
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 1000U, &next) ||
                          next.kind != MESH_ADMIN_GET_OWNER,
                      "the passkey refresh should go first");
    mesh_radio_settings_mark_sent(&settings, 71U, 1000U);
    meshtastic_AdminMessage reply = meshtastic_AdminMessage_init_default;
    reply.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    reply.session_passkey.size = 8U;
    memset(reply.session_passkey.bytes, 0x5A, 8U);
    meshtastic_MeshPacket packet;
    MESH_TEST_FAIL_IF(!mesh_test_make_admin_reply(0x1234U, 71U, &reply, &packet) ||
                          mesh_radio_settings_ingest(&settings, &packet) != 1,
                      "the owner reply should release the queue");

    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 1100U, &next) ||
                          next.kind != MESH_ADMIN_SET_FAVORITE || next.type != node_id ||
                          settings.pending_is_write,
                      "the set_favorite should follow, uncounted");
    next.my_node = 0x1234U;
    next.packet_id = 72U;
    uint8_t buffer[512];
    size_t written = 0U;
    MESH_TEST_FAIL_IF(
        mesh_radio_settings_encode_request(&settings, &next, buffer, sizeof buffer, &written) != 0,
        "set_favorite should encode");
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio) ||
                          to_radio.packet.decoded.portnum != meshtastic_PortNum_ADMIN_APP,
                      "set_favorite packet header is wrong");
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
                          admin.which_payload_variant !=
                              meshtastic_AdminMessage_set_favorite_node_tag ||
                          admin.set_favorite_node != node_id || admin.session_passkey.size != 8U ||
                          admin.session_passkey.bytes[0] != 0x5AU,
                      "set_favorite_node should carry the node and a fresh passkey");
    mesh_radio_settings_mark_sent(&settings, 72U, 1100U);

    meshtastic_MeshPacket ack = mesh_test_make_routing_reply(72U, meshtastic_Routing_Error_NONE);
    MESH_TEST_FAIL_IF(mesh_radio_settings_ingest(&settings, &ack) != 1 ||
                          mesh_radio_settings_busy(&settings) || settings.writes_acked != 0U ||
                          settings.writes_sent != 0U,
                      "the ack should release the queue without counting a save");

    /* Unpinning is the mirror image, and pinning a *different* node is not a duplicate of the
       first request even though both are set_favorite. */
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_favorite(&settings, node_id, false) != 2,
                      "an unpin should queue like a pin");
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_favorite(&settings, node_id + 1U, false) != 1,
                      "a second node should not collapse into the first request");

    record_success(test_name);
}

MESH_TEST_CASE(radio_settings_action_queue, unit) {
    struct mesh_radio_settings settings;
    mesh_radio_settings_reset(&settings);

    MESH_TEST_FAIL_IF(mesh_admin_request_is_write(MESH_ADMIN_REBOOT) ||
                          !mesh_admin_request_is_action(MESH_ADMIN_FACTORY_RESET_DEVICE) ||
                          mesh_admin_request_is_action(MESH_ADMIN_SET_CONFIG),
                      "an action is not a write and a write is not an action");
    MESH_TEST_FAIL_IF(
        mesh_radio_settings_queue_action(&settings, MESH_ADMIN_SET_CONFIG, 5U) != -EINVAL ||
            mesh_radio_settings_queue_action(&settings, MESH_ADMIN_REBOOT, 0U) != -EINVAL ||
            settings.queue_len != 0U,
        "only an action with a delay may be queued");

    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_action(&settings, MESH_ADMIN_REBOOT,
                                                       MESH_RADIO_ACTION_DELAY_SECONDS) != 2 ||
                          settings.queue_len != 2U,
                      "a reboot should queue the passkey refresh and the action");
    MESH_TEST_FAIL_IF(mesh_radio_settings_write_pending(&settings),
                      "a radio action is not a settings write");
    /* A second press before the first has gone out is the same request, not another one. */
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_action(&settings, MESH_ADMIN_REBOOT,
                                                       MESH_RADIO_ACTION_DELAY_SECONDS) != 0 ||
                          settings.queue_len != 2U,
                      "a repeated press should not queue a second reboot");

    struct mesh_admin_request next;
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 1000U, &next) ||
                          next.kind != MESH_ADMIN_GET_OWNER,
                      "the passkey refresh should go first");
    mesh_radio_settings_mark_sent(&settings, 81U, 1000U);
    meshtastic_AdminMessage reply = meshtastic_AdminMessage_init_default;
    reply.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    reply.session_passkey.size = 8U;
    memset(reply.session_passkey.bytes, 0x33, 8U);
    meshtastic_MeshPacket packet;
    MESH_TEST_FAIL_IF(!mesh_test_make_admin_reply(0x1234U, 81U, &reply, &packet) ||
                          mesh_radio_settings_ingest(&settings, &packet) != 1,
                      "the owner reply should release the queue");

    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 1100U, &next) ||
                          next.kind != MESH_ADMIN_REBOOT ||
                          next.type != MESH_RADIO_ACTION_DELAY_SECONDS || settings.pending_is_write,
                      "the reboot should follow, uncounted");
    next.my_node = 0x1234U;
    next.packet_id = 82U;
    uint8_t buffer[512];
    size_t written = 0U;
    MESH_TEST_FAIL_IF(
        mesh_radio_settings_encode_request(&settings, &next, buffer, sizeof buffer, &written) != 0,
        "the reboot should encode");
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio),
                      "the reboot ToRadio should decode");
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    MESH_TEST_FAIL_IF(
        !pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
            admin.which_payload_variant != meshtastic_AdminMessage_reboot_seconds_tag ||
            admin.reboot_seconds != (int32_t)MESH_RADIO_ACTION_DELAY_SECONDS ||
            admin.session_passkey.size != 8U || admin.session_passkey.bytes[0] != 0x33U,
        "reboot_seconds should carry the delay and a fresh passkey");
    mesh_radio_settings_mark_sent(&settings, 82U, 1100U);

    meshtastic_MeshPacket ack = mesh_test_make_routing_reply(82U, meshtastic_Routing_Error_NONE);
    MESH_TEST_FAIL_IF(mesh_radio_settings_ingest(&settings, &ack) != 1 ||
                          mesh_radio_settings_busy(&settings) || settings.writes_sent != 0U ||
                          settings.writes_acked != 0U,
                      "the ack should release the queue without counting a save");

    /* A radio that reboots before it answers is the action working, not a failed save: the
       queue moves on after the timeout and nothing is counted against the Settings tab. */
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_action(&settings, MESH_ADMIN_SHUTDOWN,
                                                       MESH_RADIO_ACTION_DELAY_SECONDS) != 2,
                      "a shutdown should queue like a reboot");
    (void)mesh_radio_settings_next_request(&settings, 2000U, &next);
    mesh_radio_settings_mark_sent(&settings, 83U, 2000U);
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(
                          &settings, 2000U + MESH_RADIO_SETTINGS_REPLY_TIMEOUT_MS + 1U, &next) ||
                          settings.writes_failed != 0U,
                      "a timed-out action should not count as a failed write");

    /* Each verb lands on its own field, and the two the firmware only tests for truth carry
       something true. */
    meshtastic_AdminMessage encoded;
    MESH_TEST_FAIL_IF(!test_action_encodes_as(MESH_ADMIN_SHUTDOWN, &encoded) ||
                          encoded.which_payload_variant !=
                              meshtastic_AdminMessage_shutdown_seconds_tag ||
                          encoded.shutdown_seconds != (int32_t)MESH_RADIO_ACTION_DELAY_SECONDS,
                      "shutdown_seconds is wrong");
    MESH_TEST_FAIL_IF(!test_action_encodes_as(MESH_ADMIN_RESET_NODEDB, &encoded) ||
                          encoded.which_payload_variant !=
                              meshtastic_AdminMessage_nodedb_reset_tag ||
                          !encoded.nodedb_reset,
                      "nodedb_reset is wrong");
    MESH_TEST_FAIL_IF(!test_action_encodes_as(MESH_ADMIN_FACTORY_RESET_CONFIG, &encoded) ||
                          encoded.which_payload_variant !=
                              meshtastic_AdminMessage_factory_reset_config_tag ||
                          encoded.factory_reset_config == 0,
                      "factory_reset_config is wrong");
    MESH_TEST_FAIL_IF(!test_action_encodes_as(MESH_ADMIN_FACTORY_RESET_DEVICE, &encoded) ||
                          encoded.which_payload_variant !=
                              meshtastic_AdminMessage_factory_reset_device_tag ||
                          encoded.factory_reset_device == 0,
                      "factory_reset_device is wrong");

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
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_remove_node(&settings, 0U) != -EINVAL ||
                          mesh_radio_settings_queue_toggle_muted(&settings, 0U) != -EINVAL ||
                          settings.queue_len != 0U,
                      "node 0 is not a node");
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_toggle_muted(&settings, node_id) != 2 ||
                          mesh_radio_settings_write_pending(&settings),
                      "a mute should queue the refresh and the verb, uncounted");
    /* A different node is not a duplicate of the first, but the same one is. */
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_toggle_muted(&settings, node_id) != 0 ||
                          mesh_radio_settings_queue_toggle_muted(&settings, node_id + 1U) != 1,
                      "mute deduplication is wrong");

    meshtastic_AdminMessage admin;
    MESH_TEST_FAIL_IF(!test_admin_encodes(&settings, MESH_ADMIN_TOGGLE_MUTED, node_id, &admin) ||
                          admin.which_payload_variant !=
                              meshtastic_AdminMessage_toggle_muted_node_tag ||
                          admin.toggle_muted_node != node_id,
                      "toggle_muted_node is wrong");
    MESH_TEST_FAIL_IF(!test_admin_encodes(&settings, MESH_ADMIN_REMOVE_NODE, node_id, &admin) ||
                          admin.which_payload_variant !=
                              meshtastic_AdminMessage_remove_by_nodenum_tag ||
                          admin.remove_by_nodenum != node_id,
                      "remove_by_nodenum is wrong");

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
    MESH_TEST_FAIL_IF(!mesh_test_session_feed_from_radio(&session, &my_info) ||
                          !mesh_test_session_feed_from_radio(&session, &one) ||
                          !mesh_test_session_feed_from_radio(&session, &two),
                      "seeding the session failed");

    const struct mesh_node_summary *node = mesh_test_session_find_node(&session, 0x2222U);
    MESH_TEST_FAIL_IF(node == NULL || !node->is_muted, "NodeInfo.is_muted should reach the cache");
    MESH_TEST_FAIL_IF(mesh_session_toggle_node_muted(&session, 0x2222U) <= 0,
                      "the mute toggle was not queued");
    node = mesh_test_session_find_node(&session, 0x2222U);
    MESH_TEST_FAIL_IF(node == NULL || node->is_muted,
                      "the cached muted flag should follow the toggle");

    /* Pressed again before the first has gone out: the queue deduplicates it, so the cached
       flag must not move a second time. One toggle on the wire, one flip here. */
    MESH_TEST_FAIL_IF(mesh_session_toggle_node_muted(&session, 0x2222U) != 0,
                      "a repeated mute should deduplicate rather than queue again");
    node = mesh_test_session_find_node(&session, 0x2222U);
    MESH_TEST_FAIL_IF(node == NULL || node->is_muted,
                      "a deduplicated mute should leave the cached flag alone");

    MESH_TEST_FAIL_IF(mesh_session_remove_node(&session, 0x1111U) != -EINVAL ||
                          mesh_session_remove_node(&session, 0x9999U) != -ENOENT,
                      "removing ourselves or an unknown node should be refused");
    const size_t before = mesh_session_handshake(&session)->node_count;
    MESH_TEST_FAIL_IF(mesh_session_remove_node(&session, 0x2222U) <= 0,
                      "the remove was not queued");
    /* The one behind it moves up rather than leaving a hole in the middle of the list. */
    MESH_TEST_FAIL_IF(mesh_session_handshake(&session)->node_count != before - 1U ||
                          mesh_test_session_find_node(&session, 0x2222U) != NULL ||
                          mesh_test_session_find_node(&session, 0x3333U) == NULL,
                      "the removed node should leave the cache compacted");

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

    MESH_TEST_FAIL_IF(!mesh_admin_request_is_write(MESH_ADMIN_SET_FIXED_POSITION) ||
                          !mesh_admin_request_is_write(MESH_ADMIN_REMOVE_FIXED_POSITION) ||
                          mesh_admin_request_is_action(MESH_ADMIN_SET_FIXED_POSITION),
                      "fixed position is a write, not a radio action");

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
    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_write(&settings, &wrong) != -EINVAL,
                      "the read-back must be the Position section");

    MESH_TEST_FAIL_IF(mesh_radio_settings_queue_write(&settings, &write) != 3 ||
                          !mesh_radio_settings_write_pending(&settings),
                      "a fixed position should queue refresh, write and read-back");
    struct mesh_admin_request next;
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 1000U, &next) ||
                          next.kind != MESH_ADMIN_GET_OWNER,
                      "the passkey refresh should go first");
    mesh_radio_settings_mark_sent(&settings, 91U, 1000U);
    meshtastic_AdminMessage reply = meshtastic_AdminMessage_init_default;
    reply.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    reply.session_passkey.size = 8U;
    memset(reply.session_passkey.bytes, 0x7EU, 8U);
    meshtastic_MeshPacket packet;
    MESH_TEST_FAIL_IF(!mesh_test_make_admin_reply(0x1234U, 91U, &reply, &packet) ||
                          mesh_radio_settings_ingest(&settings, &packet) != 1,
                      "the owner reply should release the queue");
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 1100U, &next) ||
                          next.kind != MESH_ADMIN_SET_FIXED_POSITION || !settings.pending_is_write,
                      "the write should follow, counted as a save");
    next.my_node = 0x1234U;
    next.packet_id = 92U;
    uint8_t buffer[512];
    size_t written = 0U;
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    MESH_TEST_FAIL_IF(
        mesh_radio_settings_encode_request(&settings, &next, buffer, sizeof buffer, &written) != 0,
        "set_fixed_position should encode");
    pb_istream_t in = pb_istream_from_buffer(buffer, written);
    MESH_TEST_FAIL_IF(!pb_decode(&in, meshtastic_ToRadio_fields, &to_radio),
                      "the ToRadio should decode");
    in = pb_istream_from_buffer(to_radio.packet.decoded.payload.bytes,
                                to_radio.packet.decoded.payload.size);
    MESH_TEST_FAIL_IF(
        !pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
            admin.which_payload_variant != meshtastic_AdminMessage_set_fixed_position_tag ||
            admin.set_fixed_position.latitude_i != 446488000 ||
            admin.set_fixed_position.longitude_i != -635752000 ||
            admin.set_fixed_position.altitude != 12 ||
            admin.set_fixed_position.location_source != meshtastic_Position_LocSource_LOC_MANUAL ||
            admin.session_passkey.size != 8U,
        "set_fixed_position should carry the fix and a fresh passkey");
    /* Third in the queue: the section whose flag the firmware moved behind our back. */
    mesh_radio_settings_mark_sent(&settings, 92U, 1100U);
    meshtastic_MeshPacket ack = mesh_test_make_routing_reply(92U, meshtastic_Routing_Error_NONE);
    MESH_TEST_FAIL_IF(mesh_radio_settings_ingest(&settings, &ack) != 1 ||
                          settings.writes_acked != 1U,
                      "the ack should count as a save");
    MESH_TEST_FAIL_IF(!mesh_radio_settings_next_request(&settings, 1200U, &next) ||
                          next.kind != MESH_ADMIN_GET_CONFIG ||
                          next.type != (uint32_t)meshtastic_AdminMessage_ConfigType_POSITION_CONFIG,
                      "the read-back should be get_config POSITION");

    /* A position with no coordinates would turn fixed position on with nothing behind it. */
    struct mesh_admin_request empty;
    memset(&empty, 0, sizeof empty);
    empty.kind = MESH_ADMIN_SET_FIXED_POSITION;
    empty.type = (uint32_t)meshtastic_AdminMessage_ConfigType_POSITION_CONFIG;
    empty.my_node = 0x1234U;
    empty.packet_id = 93U;
    MESH_TEST_FAIL_IF(mesh_radio_settings_encode_request(&settings, &empty, buffer, sizeof buffer,
                                                         &written) != -EINVAL,
                      "a fix with no coordinates should be refused");

    record_success(test_name);
}
