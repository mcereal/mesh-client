#define _POSIX_C_SOURCE 200809L

/* What flows once a BLE link is up: packets, channels, admin, settings writes. */

#include "framework/mesh_test.h"
#include "support/proto_fixture.h"

#include "mesh/core/app.h"
#include "mesh/core/config.h"
#include "mesh/core/event_loop.h"
#include "mesh/core/message.h"
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/transport.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/admin.pb.h"
#include "meshtastic/channel.pb.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/module_config.pb.h"
#include "meshtastic/portnums.pb.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Answers whatever admin request the mock last captured, the way the radio would: a get with
   the matching response (carrying a passkey), a set with a Routing ack. Returns the kind. */
static int test_answer_admin_write(const uint8_t *capture, size_t capture_len, uint32_t my_node,
                                   meshtastic_Routing_Error set_result, uint8_t *out,
                                   size_t out_cap, size_t *out_len, meshtastic_AdminMessage *seen) {
    meshtastic_ToRadio sent = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(capture, capture_len);
    if (!pb_decode(&in, meshtastic_ToRadio_fields, &sent) ||
        sent.which_payload_variant != meshtastic_ToRadio_packet_tag ||
        sent.packet.decoded.portnum != meshtastic_PortNum_ADMIN_APP) {
        return -1;
    }
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    in =
        pb_istream_from_buffer(sent.packet.decoded.payload.bytes, sent.packet.decoded.payload.size);
    if (!pb_decode(&in, meshtastic_AdminMessage_fields, &admin)) {
        return -2;
    }
    if (seen != NULL) {
        *seen = admin;
    }
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_packet_tag;
    meshtastic_AdminMessage reply = meshtastic_AdminMessage_init_default;
    reply.session_passkey.size = 8U;
    memset(reply.session_passkey.bytes, 0x77, 8U);
    switch (admin.which_payload_variant) {
    case meshtastic_AdminMessage_get_owner_request_tag:
        reply.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
        snprintf(reply.get_owner_response.short_name, sizeof reply.get_owner_response.short_name,
                 "%s", "0ad8");
        mesh_test_make_admin_reply(my_node, sent.packet.id, &reply, &from_radio.packet);
        break;
    case meshtastic_AdminMessage_get_device_metadata_request_tag:
        reply.which_payload_variant = meshtastic_AdminMessage_get_device_metadata_response_tag;
        mesh_test_make_admin_reply(my_node, sent.packet.id, &reply, &from_radio.packet);
        break;
    case meshtastic_AdminMessage_get_module_config_request_tag:
        reply.which_payload_variant = meshtastic_AdminMessage_get_module_config_response_tag;
        reply.get_module_config_response.which_payload_variant =
            meshtastic_ModuleConfig_store_forward_tag;
        reply.get_module_config_response.payload_variant.store_forward.enabled = true;
        reply.get_module_config_response.payload_variant.store_forward.heartbeat = true;
        mesh_test_make_admin_reply(my_node, sent.packet.id, &reply, &from_radio.packet);
        break;
    case meshtastic_AdminMessage_set_module_config_tag:
    case meshtastic_AdminMessage_set_config_tag:
    case meshtastic_AdminMessage_set_owner_tag:
        from_radio.packet = mesh_test_make_routing_reply(sent.packet.id, set_result);
        from_radio.packet.from = my_node;
        from_radio.packet.to = my_node;
        break;
    default:
        return -3;
    }
    if (!mesh_test_encode_from_radio(&from_radio, out, out_cap, out_len)) {
        return -4;
    }
    return (int)admin.which_payload_variant;
}

/*
 * End-to-end through the transport: a scripted FromRadio text packet must reach the message
 * log, an outbound send must leave as a single ToRadio write, and a Routing reply arriving the
 * same way must settle the outbound message's ack state.
 *
 * Uses a single cleanup path rather than repeating the teardown at every check: this test has
 * far more failure points than the others in this file.
 */
MESH_TEST_CASE(ble_transport_messaging_mock, unit) {
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:04", .name = "NodeFour", .rssi = -35, .paired = true},
    };

    uint8_t write_capture[256];
    memset(write_capture, 0, sizeof(write_capture));
    size_t write_len = 0U;
    char write_path[128];
    memset(write_path, 0, sizeof(write_path));
    size_t write_call_count = 0U;
    size_t write_lengths[8];
    memset(write_lengths, 0, sizeof(write_lengths));

    uint8_t read_buffers[3][256];
    const uint8_t *read_payloads[3] = {read_buffers[0], read_buffers[1], read_buffers[2]};
    size_t read_payload_lengths[3] = {0U, 0U, 0U};
    size_t read_index = 0U;

    struct mesh_bluez_mock_config mock_config = {
        .init_result = 0,
        .check_ready_result = 0,
        .find_adapter_result = 0,
        .adapter_path = "/org/bluez/hci0",
        .start_discovery_result = 0,
        .stop_discovery_result = 0,
        .connect_result = 0,
        .disconnect_result = 0,
        .subscribe_result = 0,
        .write_result = 0,
        .toradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_04/service000a/char000b",
        .fromradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_04/service000a/char000d",
        .fromnum_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_04/service000a/char000f",
        .read_payloads = read_payloads,
        .read_payload_lengths = read_payload_lengths,
        .read_payload_count = 3U,
        .read_index = &read_index,
        .devices = mock_devices,
        .device_count = sizeof(mock_devices) / sizeof(mock_devices[0]),
        .list_result = 0,
        .write_capture_buffer = write_capture,
        .write_capture_capacity = sizeof(write_capture),
        .write_capture_length = &write_len,
        .write_capture_path = write_path,
        .write_capture_path_capacity = sizeof(write_path),
        .write_call_count = &write_call_count,
        .write_lengths = write_lengths,
        .write_lengths_capacity = sizeof(write_lengths) / sizeof(write_lengths[0]),
    };

    mesh_bluez_client_mock_enable(&mock_config);

    struct mesh_app_config config = mesh_app_config_default();
    struct mesh_event_loop loop;
    mesh_event_loop_init(&loop);

    const uint32_t my_node = 0x0A0B0C0DU;
    const uint32_t peer_node = 0x0A0B0C0EU;
    const uint8_t from_num[4] = {1U, 0U, 0U, 0U};

    if (ble->ops->start(ble, &config, &loop) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }

    mesh_ble_transport_refresh_devices(ble);

    if (mesh_ble_transport_connect(ble, mock_devices[0].address) != 0) {
        failure = "connect should succeed";
        goto cleanup;
    }

    /* Script my_info (so the transport knows its own node number) then an inbound text. */
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    from_radio.my_info.my_node_num = my_node;

    pb_ostream_t encode_stream = pb_ostream_from_buffer(read_buffers[0], sizeof(read_buffers[0]));
    if (!pb_encode(&encode_stream, meshtastic_FromRadio_fields, &from_radio)) {
        failure = "failed to encode my_info";
        goto cleanup;
    }
    read_payload_lengths[0] = encode_stream.bytes_written;

    const char *inbound_text = "roger that";
    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_packet_tag;
    from_radio.packet = mesh_test_make_decoded_packet(peer_node, my_node, 0U, 4242U,
                                                      meshtastic_PortNum_TEXT_MESSAGE_APP,
                                                      inbound_text, strlen(inbound_text));

    encode_stream = pb_ostream_from_buffer(read_buffers[1], sizeof(read_buffers[1]));
    if (!pb_encode(&encode_stream, meshtastic_FromRadio_fields, &from_radio)) {
        failure = "failed to encode inbound text packet";
        goto cleanup;
    }
    read_payload_lengths[1] = encode_stream.bytes_written;
    read_payload_lengths[2] = 0U; /* empty read terminates the drain */

    read_index = 0U;
    mesh_bluez_client_mock_emit_notification(mock_config.fromnum_char_path, from_num,
                                             sizeof(from_num));
    for (int spin = 0; spin < 20 && read_index < 3U; ++spin) {
        mesh_event_loop_run(&loop, 10);
        ble->ops->tick(ble);
    }

    const struct mesh_message_log *log = mesh_ble_transport_messages(ble);
    if (log == NULL || log->count != 1U) {
        failure = "the inbound text packet did not reach the message log";
        goto cleanup;
    }

    const struct mesh_message *received = mesh_message_log_at(log, 0U);
    if (received == NULL || received->direction != MESH_MESSAGE_INBOUND ||
        received->from != peer_node || strcmp(received->text, inbound_text) != 0) {
        failure = "inbound message contents are wrong";
        goto cleanup;
    }

    /* Now send one, and check what actually goes out on the ToRadio characteristic. */
    size_t writes_before = write_call_count;
    uint32_t sent_id = 0U;
    const char *outbound_text = "on my way";
    if (mesh_ble_transport_send_text(ble, peer_node, 0U, outbound_text, true, &sent_id) != 0) {
        failure = "send_text failed";
        goto cleanup;
    }

    if (write_call_count - writes_before != 1U) {
        failure = "a text message should be exactly one ToRadio write";
        goto cleanup;
    }

    if (strcmp(write_path, mock_config.toradio_char_path) != 0) {
        failure = "text message written to the wrong characteristic";
        goto cleanup;
    }

    meshtastic_ToRadio sent = meshtastic_ToRadio_init_default;
    pb_istream_t sent_stream = pb_istream_from_buffer(write_capture, write_len);
    if (!pb_decode(&sent_stream, meshtastic_ToRadio_fields, &sent)) {
        failure = "failed to decode the sent ToRadio";
        goto cleanup;
    }

    if (sent.which_payload_variant != meshtastic_ToRadio_packet_tag ||
        sent.packet.which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        sent.packet.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) {
        failure = "the sent packet is not a decoded text message";
        goto cleanup;
    }

    if (sent.packet.to != peer_node || sent.packet.id != sent_id || !sent.packet.want_ack) {
        failure = "sent packet header fields are wrong";
        goto cleanup;
    }

    if (sent.packet.decoded.payload.size != strlen(outbound_text) ||
        memcmp(sent.packet.decoded.payload.bytes, outbound_text, strlen(outbound_text)) != 0) {
        failure = "sent packet text is wrong";
        goto cleanup;
    }

    log = mesh_ble_transport_messages(ble);
    const struct mesh_message *outbound = mesh_message_log_at(log, 1U);
    if (outbound == NULL || outbound->direction != MESH_MESSAGE_OUTBOUND ||
        outbound->ack != MESH_MESSAGE_ACK_PENDING) {
        failure = "the sent message should be logged as pending";
        goto cleanup;
    }

    /* Finally, deliver the ack the same way the radio would. */
    meshtastic_Routing routing = meshtastic_Routing_init_default;
    routing.which_variant = meshtastic_Routing_error_reason_tag;
    routing.error_reason = meshtastic_Routing_Error_NONE;

    uint8_t routing_payload[64];
    pb_ostream_t routing_stream = pb_ostream_from_buffer(routing_payload, sizeof routing_payload);
    if (!pb_encode(&routing_stream, meshtastic_Routing_fields, &routing)) {
        failure = "failed to encode the routing reply";
        goto cleanup;
    }

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_packet_tag;
    from_radio.packet =
        mesh_test_make_decoded_packet(peer_node, my_node, 0U, 5555U, meshtastic_PortNum_ROUTING_APP,
                                      routing_payload, routing_stream.bytes_written);
    from_radio.packet.decoded.request_id = sent_id;

    encode_stream = pb_ostream_from_buffer(read_buffers[0], sizeof(read_buffers[0]));
    if (!pb_encode(&encode_stream, meshtastic_FromRadio_fields, &from_radio)) {
        failure = "failed to encode the routing FromRadio";
        goto cleanup;
    }
    read_payload_lengths[0] = encode_stream.bytes_written;
    read_payload_lengths[1] = 0U;
    read_payload_lengths[2] = 0U;

    read_index = 0U;
    mesh_bluez_client_mock_emit_notification(mock_config.fromnum_char_path, from_num,
                                             sizeof(from_num));
    for (int spin = 0; spin < 20 && read_index < 2U; ++spin) {
        mesh_event_loop_run(&loop, 10);
        ble->ops->tick(ble);
    }

    log = mesh_ble_transport_messages(ble);
    outbound = mesh_message_log_at(log, 1U);
    if (outbound == NULL || outbound->ack != MESH_MESSAGE_ACK_DELIVERED) {
        failure = "the routing reply should have marked the message delivered";
        goto cleanup;
    }

    /* A routing reply is not itself a message. */
    if (log->count != 2U) {
        failure = "the routing reply should not have added a message";
        goto cleanup;
    }

cleanup:
    ble->ops->stop(ble);
    mesh_event_loop_shutdown(&loop);
    mesh_bluez_client_mock_disable();

    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}

/* The radio's Channel messages land in the handshake status, indexed by slot. */
MESH_TEST_CASE(ble_transport_channel_decode, unit) {
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:09", .name = "NodeNine", .rssi = -50, .paired = true},
    };
    uint8_t write_capture[64];
    size_t write_len = 0U;

    uint8_t read_buffers[2][128];
    const uint8_t *read_payloads[2] = {read_buffers[0], read_buffers[1]};
    size_t read_payload_lengths[2] = {0U, 0U};
    size_t read_index = 0U;

    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_channel_tag;
    from_radio.channel.index = 1;
    from_radio.channel.role = meshtastic_Channel_Role_SECONDARY;
    from_radio.channel.has_settings = true;
    snprintf(from_radio.channel.settings.name, sizeof from_radio.channel.settings.name, "%s",
             "Team");
    pb_ostream_t stream = pb_ostream_from_buffer(read_buffers[0], sizeof read_buffers[0]);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_FromRadio_fields, &from_radio),
                      "encode channel 1 failed");
    read_payload_lengths[0] = stream.bytes_written;

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_channel_tag;
    from_radio.channel.index = 0;
    from_radio.channel.role = meshtastic_Channel_Role_PRIMARY;
    from_radio.channel.has_settings = true; /* unnamed: the default primary */
    stream = pb_ostream_from_buffer(read_buffers[1], sizeof read_buffers[1]);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_FromRadio_fields, &from_radio),
                      "encode channel 0 failed");
    read_payload_lengths[1] = stream.bytes_written;

    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .toradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_09/service000a/char000b",
        .fromradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_09/service000a/char000d",
        .fromnum_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_09/service000a/char000f",
        .read_payloads = read_payloads,
        .read_payload_lengths = read_payload_lengths,
        .read_payload_count = 2U,
        .read_index = &read_index,
        .devices = mock_devices,
        .device_count = 1U,
        .write_capture_buffer = write_capture,
        .write_capture_capacity = sizeof(write_capture),
        .write_capture_length = &write_len,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    struct mesh_app_config config = mesh_app_config_default();
    struct mesh_event_loop loop;
    mesh_event_loop_init(&loop);
    if (ble->ops->start(ble, &config, &loop) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);
    if (mesh_ble_transport_connect(ble, mock_devices[0].address) != 0) {
        failure = "connect should be accepted";
        goto cleanup;
    }
    for (int spin = 0; spin < 20 && read_index < 3U; ++spin) {
        ble->ops->tick(ble);
        mesh_event_loop_run(&loop, 10);
    }

    struct mesh_handshake_status status = mesh_ble_transport_handshake_status(ble);
    if (status.channel_count != 2U) {
        failure = "channel_count should cover slots 0 and 1";
        goto cleanup;
    }
    if (status.channels[1].index != 1U || status.channels[1].role != 2U ||
        strcmp(status.channels[1].name, "Team") != 0) {
        failure = "secondary channel not decoded";
        goto cleanup;
    }
    if (status.channels[0].index != 0U || status.channels[0].role != 1U ||
        status.channels[0].name[0] != '\0') {
        failure = "primary channel not decoded";
        goto cleanup;
    }

cleanup:
    ble->ops->stop(ble);
    mesh_event_loop_shutdown(&loop);
    mesh_bluez_client_mock_disable();
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/* A packet from a node refreshes its last_heard/SNR; one from a node the sync never delivered
   adds it, so the UI can name and target whoever is actually talking to us. */
MESH_TEST_CASE(ble_transport_packet_touches_node, unit) {
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:0C", .name = "NodeTwelve", .rssi = -50, .paired = true},
    };
    uint8_t write_capture[64];
    size_t write_len = 0U;

    uint8_t read_buffers[3][160];
    const uint8_t *read_payloads[3] = {read_buffers[0], read_buffers[1], read_buffers[2]};
    size_t read_payload_lengths[3] = {0U, 0U, 0U};
    size_t read_index = 0U;

    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    from_radio.node_info.num = 0x7c376ddaU;
    from_radio.node_info.has_user = true;
    snprintf(from_radio.node_info.user.short_name, sizeof from_radio.node_info.user.short_name,
             "%s", "6dda");
    from_radio.node_info.last_heard = 1000U;
    from_radio.node_info.snr = 1.0f;
    pb_ostream_t stream = pb_ostream_from_buffer(read_buffers[0], sizeof read_buffers[0]);
    if (!pb_encode(&stream, meshtastic_FromRadio_fields, &from_radio)) {
        record_failure(test_name, "encode node_info failed");
        return;
    }
    read_payload_lengths[0] = stream.bytes_written;

    /* A text from that node, timestamped well after the sync. */
    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_packet_tag;
    from_radio.packet.from = 0x7c376ddaU;
    from_radio.packet.to = 0x11111111U;
    from_radio.packet.id = 77U;
    from_radio.packet.has_rx_time = true;
    from_radio.packet.rx_time = 5000U;
    from_radio.packet.rx_snr = 7.5f;
    from_radio.packet.hop_start = 3U;
    from_radio.packet.hop_limit = 2U;
    from_radio.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    from_radio.packet.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    from_radio.packet.decoded.payload.size = 2U;
    memcpy(from_radio.packet.decoded.payload.bytes, "hi", 2U);
    stream = pb_ostream_from_buffer(read_buffers[1], sizeof read_buffers[1]);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_FromRadio_fields, &from_radio),
                      "encode packet failed");
    read_payload_lengths[1] = stream.bytes_written;

    /* A position packet from a node we never got NodeInfo for. */
    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_packet_tag;
    from_radio.packet.from = 0x0badf00dU;
    from_radio.packet.to = MESH_MESSAGE_BROADCAST_ADDR;
    from_radio.packet.id = 78U;
    from_radio.packet.has_rx_time = true;
    from_radio.packet.rx_time = 6000U;
    from_radio.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    from_radio.packet.decoded.portnum = meshtastic_PortNum_POSITION_APP;
    stream = pb_ostream_from_buffer(read_buffers[2], sizeof read_buffers[2]);
    MESH_TEST_FAIL_IF(!pb_encode(&stream, meshtastic_FromRadio_fields, &from_radio),
                      "encode position failed");
    read_payload_lengths[2] = stream.bytes_written;

    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .toradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0C/service000a/char000b",
        .fromradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0C/service000a/char000d",
        .fromnum_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0C/service000a/char000f",
        .read_payloads = read_payloads,
        .read_payload_lengths = read_payload_lengths,
        .read_payload_count = 3U,
        .read_index = &read_index,
        .devices = mock_devices,
        .device_count = 1U,
        .write_capture_buffer = write_capture,
        .write_capture_capacity = sizeof(write_capture),
        .write_capture_length = &write_len,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    struct mesh_app_config config = mesh_app_config_default();
    struct mesh_event_loop loop;
    mesh_event_loop_init(&loop);
    if (ble->ops->start(ble, &config, &loop) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);
    if (mesh_ble_transport_connect(ble, mock_devices[0].address) != 0) {
        failure = "connect should be accepted";
        goto cleanup;
    }
    for (int spin = 0; spin < 20 && read_index < 4U; ++spin) {
        ble->ops->tick(ble);
        mesh_event_loop_run(&loop, 10);
    }

    struct mesh_handshake_status status = mesh_ble_transport_handshake_status(ble);
    if (status.node_count != 2U) {
        failure = "expected the synced node plus the one heard without NodeInfo";
        goto cleanup;
    }
    const struct mesh_node_summary *known = &status.nodes[0];
    if (known->node_id != 0x7c376ddaU || known->last_heard != 5000U || known->snr != 7.5f ||
        !known->has_hops_away || known->hops_away != 1U || strcmp(known->short_name, "6dda") != 0) {
        failure = "packet did not refresh the known node";
        goto cleanup;
    }
    /* No NodeInfo for this one yet, so it carries the identity the apps derive from the node
       number rather than an empty name. */
    if (status.nodes[1].node_id != 0x0badf00dU || status.nodes[1].last_heard != 6000U ||
        strcmp(status.nodes[1].short_name, "f00d") != 0 || status.nodes[1].has_user) {
        failure = "unknown sender should be added by id";
        goto cleanup;
    }
    const struct mesh_message_log *log = mesh_ble_transport_messages(ble);
    if (log == NULL || log->count != 1U) {
        failure = "the text should still be ingested";
        goto cleanup;
    }

cleanup:
    ble->ops->stop(ble);
    mesh_event_loop_shutdown(&loop);
    mesh_bluez_client_mock_disable();
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/* The whole path on the mock bus: handshake fragments land in the settings, the completed
   handshake triggers the admin probe, the reply is consumed (not logged as a message) and
   the passkey is held for the next request. */
MESH_TEST_CASE(ble_transport_admin_probe, unit) {
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:0A", .name = "NodeAdmin", .rssi = -40, .paired = true},
    };
    uint8_t write_capture[256];
    size_t write_len = 0U;
    size_t write_call_count = 0U;
    size_t write_lengths[16];
    memset(write_lengths, 0, sizeof write_lengths);

    enum { READ_SLOTS = 12 };
    uint8_t read_buffers[READ_SLOTS][300];
    const uint8_t *read_payloads[READ_SLOTS];
    size_t read_payload_lengths[READ_SLOTS];
    for (size_t i = 0; i < READ_SLOTS; ++i) {
        read_payloads[i] = read_buffers[i];
        read_payload_lengths[i] = 0U;
    }
    size_t read_index = 0U;

    const uint32_t my_node = 0x9E9D0AD8U;
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    from_radio.my_info.my_node_num = my_node;
    mesh_test_encode_from_radio(&from_radio, read_buffers[0], sizeof read_buffers[0],
                                &read_payload_lengths[0]);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_config_tag;
    from_radio.config.which_payload_variant = meshtastic_Config_lora_tag;
    from_radio.config.payload_variant.lora.region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
    from_radio.config.payload_variant.lora.hop_limit = 5U;
    mesh_test_encode_from_radio(&from_radio, read_buffers[1], sizeof read_buffers[1],
                                &read_payload_lengths[1]);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_moduleConfig_tag;
    from_radio.moduleConfig.which_payload_variant = meshtastic_ModuleConfig_store_forward_tag;
    from_radio.moduleConfig.payload_variant.store_forward.enabled = true;
    mesh_test_encode_from_radio(&from_radio, read_buffers[2], sizeof read_buffers[2],
                                &read_payload_lengths[2]);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    from_radio.node_info.num = my_node;
    from_radio.node_info.has_user = true;
    snprintf(from_radio.node_info.user.short_name, sizeof from_radio.node_info.user.short_name,
             "%s", "0ad8");
    snprintf(from_radio.node_info.user.long_name, sizeof from_radio.node_info.user.long_name, "%s",
             "Meshtastic 0ad8");
    mesh_test_encode_from_radio(&from_radio, read_buffers[3], sizeof read_buffers[3],
                                &read_payload_lengths[3]);
    /* read_buffers[4] stays empty: ends the first drain. */

    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .toradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0A/service000a/char000b",
        .fromradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0A/service000a/char000d",
        .fromnum_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0A/service000a/char000f",
        .read_payloads = read_payloads,
        .read_payload_lengths = read_payload_lengths,
        .read_payload_count = READ_SLOTS,
        .read_index = &read_index,
        .devices = mock_devices,
        .device_count = 1U,
        .write_capture_buffer = write_capture,
        .write_capture_capacity = sizeof write_capture,
        .write_capture_length = &write_len,
        .write_call_count = &write_call_count,
        .write_lengths = write_lengths,
        .write_lengths_capacity = sizeof write_lengths / sizeof write_lengths[0],
    };
    mesh_bluez_client_mock_enable(&mock_config);

    struct mesh_app_config config = mesh_app_config_default();
    struct mesh_event_loop loop;
    mesh_event_loop_init(&loop);
    if (ble->ops->start(ble, &config, &loop) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);
    if (mesh_ble_transport_connect(ble, mock_devices[0].address) != 0) {
        failure = "connect should be accepted";
        goto cleanup;
    }
    const uint8_t from_num[4] = {1U, 0U, 0U, 0U};
    for (int spin = 0; spin < 30 && read_index < 5U; ++spin) {
        ble->ops->tick(ble);
        mesh_event_loop_run(&loop, 10);
        if (mesh_ble_transport_connected_address(ble) != NULL && spin % 5 == 0) {
            mesh_bluez_client_mock_emit_notification(mock_config.fromnum_char_path, from_num,
                                                     sizeof from_num);
        }
    }

    const struct mesh_radio_settings *settings = mesh_ble_transport_settings(ble);
    if (settings == NULL || !settings->has_lora ||
        settings->lora.region != meshtastic_Config_LoRaConfig_RegionCode_EU_868 ||
        !settings->has_store_forward || !settings->store_forward.enabled || !settings->has_owner ||
        strcmp(settings->owner.short_name, "0ad8") != 0) {
        failure = "handshake fragments should land in the settings";
        goto cleanup;
    }
    if (settings->admin_replies != 0U || mesh_radio_settings_busy(settings)) {
        failure = "no admin traffic before the handshake completes";
        goto cleanup;
    }

    /* Complete the handshake; the probe should go out on the next tick. */
    struct mesh_handshake_status status = mesh_ble_transport_handshake_status(ble);
    if (!status.request_in_flight || status.request_id == 0U) {
        failure = "want_config should be in flight";
        goto cleanup;
    }
    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
    from_radio.config_complete_id = status.request_id;
    read_index = 5U;
    mesh_test_encode_from_radio(&from_radio, read_buffers[5], sizeof read_buffers[5],
                                &read_payload_lengths[5]);
    const size_t writes_before = write_call_count;
    mesh_bluez_client_mock_emit_notification(mock_config.fromnum_char_path, from_num,
                                             sizeof from_num);
    for (int spin = 0; spin < 20 && write_call_count == writes_before; ++spin) {
        mesh_event_loop_run(&loop, 10);
        ble->ops->tick(ble);
    }
    if (write_call_count != writes_before + 1U) {
        failure = "completing the handshake should send exactly one admin request";
        goto cleanup;
    }
    meshtastic_ToRadio sent = meshtastic_ToRadio_init_default;
    pb_istream_t in = pb_istream_from_buffer(write_capture, write_len);
    if (!pb_decode(&in, meshtastic_ToRadio_fields, &sent) ||
        sent.which_payload_variant != meshtastic_ToRadio_packet_tag || sent.packet.to != my_node ||
        sent.packet.decoded.portnum != meshtastic_PortNum_ADMIN_APP ||
        !sent.packet.decoded.want_response) {
        failure = "the probe is not an admin packet to ourselves";
        goto cleanup;
    }
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_default;
    in =
        pb_istream_from_buffer(sent.packet.decoded.payload.bytes, sent.packet.decoded.payload.size);
    if (!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
        admin.which_payload_variant != meshtastic_AdminMessage_get_device_metadata_request_tag) {
        failure = "the probe should ask for the device metadata first";
        goto cleanup;
    }
    settings = mesh_ble_transport_settings(ble);
    if (!mesh_radio_settings_busy(settings) || settings->pending_request_id != sent.packet.id) {
        failure = "the probe should be recorded as pending";
        goto cleanup;
    }

    /* Answer it the way the radio would. */
    admin = (meshtastic_AdminMessage)meshtastic_AdminMessage_init_default;
    admin.which_payload_variant = meshtastic_AdminMessage_get_device_metadata_response_tag;
    snprintf(admin.get_device_metadata_response.firmware_version,
             sizeof admin.get_device_metadata_response.firmware_version, "%s", "2.8.0.abcdef");
    admin.get_device_metadata_response.hw_model = meshtastic_HardwareModel_TBEAM;
    admin.get_device_metadata_response.hasBluetooth = true;
    admin.session_passkey.size = 8U;
    memset(admin.session_passkey.bytes, 0xA5, 8U);
    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_packet_tag;
    if (!mesh_test_make_admin_reply(my_node, sent.packet.id, &admin, &from_radio.packet)) {
        failure = "encode metadata reply failed";
        goto cleanup;
    }
    read_index = 7U;
    mesh_test_encode_from_radio(&from_radio, read_buffers[7], sizeof read_buffers[7],
                                &read_payload_lengths[7]);
    const size_t writes_before_reply = write_call_count;
    mesh_bluez_client_mock_emit_notification(mock_config.fromnum_char_path, from_num,
                                             sizeof from_num);
    for (int spin = 0; spin < 20 && write_call_count == writes_before_reply; ++spin) {
        mesh_event_loop_run(&loop, 10);
        ble->ops->tick(ble);
    }
    settings = mesh_ble_transport_settings(ble);
    if (settings->admin_replies != 1U || !settings->has_session_passkey ||
        settings->session_passkey_len != 8U || !settings->has_metadata ||
        strcmp(settings->metadata.firmware_version, "2.8.0.abcdef") != 0) {
        failure = "the metadata reply should be folded in with its passkey";
        goto cleanup;
    }
    const struct mesh_message_log *log = mesh_ble_transport_messages(ble);
    if (log == NULL || log->count != 0U) {
        failure = "an admin reply must not appear in the message log";
        goto cleanup;
    }
    /* The queue moved on to the owner request, and it now carries the passkey. */
    if (write_call_count != writes_before_reply + 1U) {
        failure = "the owner request should follow the metadata reply";
        goto cleanup;
    }
    sent = (meshtastic_ToRadio)meshtastic_ToRadio_init_default;
    in = pb_istream_from_buffer(write_capture, write_len);
    admin = (meshtastic_AdminMessage)meshtastic_AdminMessage_init_default;
    if (!pb_decode(&in, meshtastic_ToRadio_fields, &sent)) {
        failure = "second request is not a ToRadio";
        goto cleanup;
    }
    in =
        pb_istream_from_buffer(sent.packet.decoded.payload.bytes, sent.packet.decoded.payload.size);
    if (!pb_decode(&in, meshtastic_AdminMessage_fields, &admin) ||
        admin.which_payload_variant != meshtastic_AdminMessage_get_owner_request_tag ||
        admin.session_passkey.size != 8U || admin.session_passkey.bytes[0] != 0xA5U) {
        failure = "the owner request should carry the passkey";
        goto cleanup;
    }

    /* A manual refresh re-reads everything (the probe pair included, since they have left
       the queue); nothing goes out until the owner reply lands or times out. */
    const int queued = mesh_ble_transport_refresh_settings(ble);
    if (queued != 2 + 8 + (int)mesh_radio_module_count() + (int)MESH_RADIO_SETTINGS_MAX_CHANNELS) {
        failure = "refresh should queue every section and every channel slot";
        goto cleanup;
    }
    if (write_call_count != writes_before_reply + 1U) {
        failure = "refresh must not write while a reply is awaited";
        goto cleanup;
    }

cleanup:
    ble->ops->stop(ble);
    mesh_event_loop_shutdown(&loop);
    mesh_bluez_client_mock_disable();
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/* End to end on the mock bus: a Store & Forward save goes out as passkey refresh, set with
   the passkey and the full section, read-back; the Routing ack is consumed rather than logged,
   the counters move, and the read-back updates the view. */
MESH_TEST_CASE(ble_transport_settings_write, unit) {
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:0B", .name = "NodeWrite", .rssi = -40, .paired = true},
    };
    uint8_t write_capture[512];
    size_t write_len = 0U;
    size_t write_call_count = 0U;
    size_t write_lengths[32];
    memset(write_lengths, 0, sizeof write_lengths);

    enum { READ_SLOTS = 24 };
    static uint8_t read_buffers[READ_SLOTS][300];
    const uint8_t *read_payloads[READ_SLOTS];
    size_t read_payload_lengths[READ_SLOTS];
    for (size_t i = 0; i < READ_SLOTS; ++i) {
        read_payloads[i] = read_buffers[i];
        read_payload_lengths[i] = 0U;
    }
    size_t read_index = 0U;

    const uint32_t my_node = 0x9E9D0AD8U;
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    from_radio.my_info.my_node_num = my_node;
    mesh_test_encode_from_radio(&from_radio, read_buffers[0], sizeof read_buffers[0],
                                &read_payload_lengths[0]);
    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_moduleConfig_tag;
    from_radio.moduleConfig.which_payload_variant = meshtastic_ModuleConfig_store_forward_tag;
    from_radio.moduleConfig.payload_variant.store_forward.enabled = false;
    from_radio.moduleConfig.payload_variant.store_forward.records = 500U;
    mesh_test_encode_from_radio(&from_radio, read_buffers[1], sizeof read_buffers[1],
                                &read_payload_lengths[1]);
    /* read_buffers[2] empty: ends the first drain. */

    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .toradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0B/service000a/char000b",
        .fromradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0B/service000a/char000d",
        .fromnum_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0B/service000a/char000f",
        .read_payloads = read_payloads,
        .read_payload_lengths = read_payload_lengths,
        .read_payload_count = READ_SLOTS,
        .read_index = &read_index,
        .devices = mock_devices,
        .device_count = 1U,
        .write_capture_buffer = write_capture,
        .write_capture_capacity = sizeof write_capture,
        .write_capture_length = &write_len,
        .write_call_count = &write_call_count,
        .write_lengths = write_lengths,
        .write_lengths_capacity = sizeof write_lengths / sizeof write_lengths[0],
    };
    mesh_bluez_client_mock_enable(&mock_config);

    struct mesh_app_config config = mesh_app_config_default();
    struct mesh_event_loop loop;
    mesh_event_loop_init(&loop);
    if (ble->ops->start(ble, &config, &loop) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);
    if (mesh_ble_transport_connect(ble, mock_devices[0].address) != 0) {
        failure = "connect should be accepted";
        goto cleanup;
    }
    const uint8_t from_num[4] = {1U, 0U, 0U, 0U};
    for (int spin = 0; spin < 30 && read_index < 3U; ++spin) {
        ble->ops->tick(ble);
        mesh_event_loop_run(&loop, 10);
        if (mesh_ble_transport_connected_address(ble) != NULL && spin % 5 == 0) {
            mesh_bluez_client_mock_emit_notification(mock_config.fromnum_char_path, from_num,
                                                     sizeof from_num);
        }
    }
    const struct mesh_radio_settings *settings = mesh_ble_transport_settings(ble);
    if (settings == NULL || !settings->has_store_forward || settings->store_forward.enabled) {
        failure = "the store & forward section should have arrived";
        goto cleanup;
    }

    /* Before the handshake completes there is no probe, so the write is the first admin
       traffic: get_owner, set_module_config, get_module_config. */
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_STORE_FORWARD;
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_SF_ENABLED;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_SF_HEARTBEAT;
    action.edits[1].number = 1U;
    struct mesh_admin_request write;
    if (mesh_app_build_settings_write(settings, &action, &write) != 0 ||
        mesh_ble_transport_write_settings(ble, &write) != 3) {
        failure = "the write should queue three requests";
        goto cleanup;
    }

    size_t slot = 3U;
    const int expected[] = {
        (int)meshtastic_AdminMessage_get_owner_request_tag,
        (int)meshtastic_AdminMessage_set_module_config_tag,
        (int)meshtastic_AdminMessage_get_module_config_request_tag,
    };
    meshtastic_AdminMessage seen;
    /* Each reply releases the next request during the settle spins, so count from a fixed
       base rather than per step. */
    const size_t base = write_call_count;
    for (size_t step = 0; step < 3U; ++step) {
        for (int spin = 0; spin < 20 && write_call_count < base + step + 1U; ++spin) {
            ble->ops->tick(ble);
            mesh_event_loop_run(&loop, 10);
        }
        if (write_call_count != base + step + 1U) {
            failure = "each request should go out once the previous one is answered";
            goto cleanup;
        }
        memset(&seen, 0, sizeof seen);
        const int kind = test_answer_admin_write(
            write_capture, write_len, my_node, meshtastic_Routing_Error_NONE, read_buffers[slot],
            sizeof read_buffers[slot], &read_payload_lengths[slot], &seen);
        if (kind != expected[step]) {
            failure = "requests should go out as refresh, set, read-back";
            goto cleanup;
        }
        if (step == 1U) {
            const meshtastic_ModuleConfig_StoreForwardConfig *sf =
                &seen.set_module_config.payload_variant.store_forward;
            if (seen.set_module_config.which_payload_variant !=
                    meshtastic_ModuleConfig_store_forward_tag ||
                !sf->enabled || !sf->heartbeat || sf->records != 500U ||
                seen.session_passkey.size != 8U || seen.session_passkey.bytes[0] != 0x77U) {
                failure = "the set should carry the whole section, the edits and the passkey";
                goto cleanup;
            }
        }
        read_index = slot;
        slot += 2U; /* leave an empty slot to end each drain */
        mesh_bluez_client_mock_emit_notification(mock_config.fromnum_char_path, from_num,
                                                 sizeof from_num);
        for (int spin = 0; spin < 5; ++spin) {
            mesh_event_loop_run(&loop, 10);
            ble->ops->tick(ble);
        }
    }

    settings = mesh_ble_transport_settings(ble);
    if (settings->writes_sent != 1U || settings->writes_acked != 1U ||
        settings->writes_failed != 0U || mesh_radio_settings_write_pending(settings) ||
        mesh_radio_settings_busy(settings)) {
        failure = "the ack should be counted and nothing left pending";
        goto cleanup;
    }
    if (!settings->store_forward.enabled || !settings->store_forward.heartbeat) {
        failure = "the read-back should update the section";
        goto cleanup;
    }
    const struct mesh_message_log *log = mesh_ble_transport_messages(ble);
    if (log == NULL || log->count != 0U) {
        failure = "the routing ack must not appear in the message log";
        goto cleanup;
    }

    /* A rejected write: the same dance, answered with a bad-session-key error. */
    if (mesh_ble_transport_write_settings(ble, &write) != 3) {
        failure = "a second write should queue again";
        goto cleanup;
    }
    const size_t base2 = write_call_count;
    for (size_t step = 0; step < 2U; ++step) {
        for (int spin = 0; spin < 20 && write_call_count < base2 + step + 1U; ++spin) {
            ble->ops->tick(ble);
            mesh_event_loop_run(&loop, 10);
        }
        test_answer_admin_write(write_capture, write_len, my_node,
                                meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY, read_buffers[slot],
                                sizeof read_buffers[slot], &read_payload_lengths[slot], NULL);
        read_index = slot;
        slot += 2U;
        mesh_bluez_client_mock_emit_notification(mock_config.fromnum_char_path, from_num,
                                                 sizeof from_num);
        for (int spin = 0; spin < 5; ++spin) {
            mesh_event_loop_run(&loop, 10);
            ble->ops->tick(ble);
        }
    }
    settings = mesh_ble_transport_settings(ble);
    if (settings->writes_failed != 1U ||
        settings->last_write_error != (int32_t)meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY ||
        settings->writes_acked != 1U) {
        failure = "a rejection should be counted with its reason";
        goto cleanup;
    }

cleanup:
    ble->ops->stop(ble);
    mesh_event_loop_shutdown(&loop);
    mesh_bluez_client_mock_disable();
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}
