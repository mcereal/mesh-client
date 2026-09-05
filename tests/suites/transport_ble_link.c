#define _POSIX_C_SOURCE 200809L

/* Bringing a BLE link up and watching it fall over: discovery, connect, drops. */

#include "framework/mesh_test.h"

#include "mesh/core/config.h"
#include "mesh/core/event_loop.h"
#include "mesh/core/message.h"
#include "mesh/core/session.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/transport.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/mesh.pb.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool string_matches_any(const char *value, const char *const options[],
                               size_t option_count) {
    if (value == NULL) {
        return false;
    }
    for (size_t i = 0; i < option_count; ++i) {
        if (strcmp(value, options[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void test_sleep_ms(unsigned ms) {
    struct timespec ts = {.tv_sec = ms / 1000U, .tv_nsec = (long)(ms % 1000U) * 1000000L};
    nanosleep(&ts, NULL);
}

MESH_TEST_CASE(ble_transport_status_transitions, unit) {
    struct mesh_transport *ble = mesh_ble_transport();

    const char *initial = ble->ops->status(ble);
    if (strcmp(initial, "disabled") != 0 && strcmp(initial, "inactive") != 0) {
        record_failure(test_name, "unexpected initial status");
        return;
    }

    struct mesh_app_config config = mesh_app_config_default();
    config.enable_ble = false;

    struct mesh_event_loop loop;
    mesh_event_loop_init(&loop);

    ble->ops->start(ble, &config, &loop);
    const char *disabled_status = ble->ops->status(ble);
    if (strcmp(disabled_status, "disabled") != 0) {
        record_failure(test_name, "status should report disabled when transport is off");
        ble->ops->stop(ble);
        return;
    }

    config.enable_ble = true;
    ble->ops->start(ble, &config, &loop);
    const char *running_status = ble->ops->status(ble);
    const char *expected_states[] = {"running", "waiting-for-bluez", "waiting-for-adapter",
                                     "inactive"};
    if (!string_matches_any(running_status, expected_states,
                            sizeof(expected_states) / sizeof(expected_states[0]))) {
        record_failure(test_name, "unexpected status after enabling BLE");
        ble->ops->stop(ble);
        return;
    }

    ble->ops->stop(ble);
    mesh_event_loop_shutdown(&loop);
    record_success(test_name);
}

MESH_TEST_CASE(ble_transport_discovery_mock, unit) {
    struct mesh_transport *ble = mesh_ble_transport();

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:01", .name = "NodeOne", .rssi = -45, .paired = true},
        {.address = "AA:BB:CC:DD:EE:02", .name = "NodeTwo", .rssi = -60, .paired = true},
    };

    struct mesh_bluez_mock_config mock_config = {
        .init_result = 0,
        .check_ready_result = 0,
        .find_adapter_result = 0,
        .adapter_path = "/org/bluez/hci0",
        .start_discovery_result = 0,
        .stop_discovery_result = 0,
        .devices = mock_devices,
        .device_count = sizeof(mock_devices) / sizeof(mock_devices[0]),
        .list_result = 0,
    };

    mesh_bluez_client_mock_enable(&mock_config);

    struct mesh_app_config config = mesh_app_config_default();
    struct mesh_event_loop loop;
    mesh_event_loop_init(&loop);

    int result = ble->ops->start(ble, &config, &loop);
    if (result != 0) {
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "ble start should succeed with mock");
        return;
    }

    struct mesh_bluez_device_info discovered[4];
    size_t count =
        mesh_ble_transport_get_devices(ble, discovered, sizeof(discovered) / sizeof(discovered[0]));
    if (count != mock_config.device_count) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "unexpected discovered device count");
        return;
    }

    if (strcmp(discovered[0].name, "NodeOne") != 0 ||
        strcmp(discovered[1].address, "AA:BB:CC:DD:EE:02") != 0) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "device details mismatch");
        return;
    }

    mock_devices[0].rssi = -35;
    mock_config.device_count = 1;
    mock_config.devices = mock_devices;
    mesh_bluez_client_mock_enable(&mock_config);
    size_t refreshed = mesh_ble_transport_refresh_devices(ble);
    if (refreshed != 1U) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "refresh should update device list");
        return;
    }

    ble->ops->stop(ble);
    mesh_event_loop_shutdown(&loop);
    mesh_bluez_client_mock_disable();
    record_success(test_name);
}

MESH_TEST_CASE(ble_transport_connect_mock, unit) {
    struct mesh_transport *ble = mesh_ble_transport();

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:03", .name = "NodeThree", .rssi = -40, .paired = true},
    };

    uint8_t write_capture[64];
    memset(write_capture, 0, sizeof(write_capture));
    size_t write_len = 0;
    char write_path[128];
    memset(write_path, 0, sizeof(write_path));
    size_t write_call_count = 0U;
    size_t write_lengths[8];
    memset(write_lengths, 0, sizeof(write_lengths));

    /*
     * Scripted FromRadio reads; the transport drains until it sees an empty read. Five payloads
     * exceed the per-turn read budget, so this also exercises the eventfd continuation.
     */
    uint8_t read_buffers[5][256];
    const uint8_t *read_payloads[5] = {read_buffers[0], read_buffers[1], read_buffers[2],
                                       read_buffers[3], read_buffers[4]};
    size_t read_payload_lengths[5] = {0U, 0U, 0U, 0U, 0U};
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
        .toradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_03/service000a/char000b",
        .fromradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_03/service000a/char000d",
        .fromnum_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_03/service000a/char000f",
        .read_payloads = read_payloads,
        .read_payload_lengths = read_payload_lengths,
        .read_payload_count = 5U, /* lengths stay 0 (empty FIFO) until scripted below */
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

    if (ble->ops->start(ble, &config, &loop) != 0) {
        mesh_bluez_client_mock_disable();
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "ble start failed");
        return;
    }

    mesh_ble_transport_refresh_devices(ble);

    if (mesh_ble_transport_connect(ble, mock_devices[0].address) != 0) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "connect should succeed");
        return;
    }

    if (write_len == 0U) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "expected want_config write");
        return;
    }

    if (strcmp(write_path, mock_config.toradio_char_path) != 0) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "want_config write path mismatch");
        return;
    }

    /* BLE ToRadio writes carry the bare protobuf: no varint length prefix. */
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t to_radio_stream = pb_istream_from_buffer(write_capture, write_len);
    if (!pb_decode(&to_radio_stream, meshtastic_ToRadio_fields, &to_radio)) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "failed to decode want_config payload");
        return;
    }

    if (to_radio.which_payload_variant != meshtastic_ToRadio_want_config_id_tag) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "unexpected ToRadio payload");
        return;
    }

    struct mesh_handshake_status handshake = mesh_ble_transport_handshake_status(ble);
    if (!handshake.request_in_flight || handshake.request_id != to_radio.want_config_id) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "handshake state not initialised");
        return;
    }

    /* Script the node's FromRadio FIFO: my_info, node_info, config_complete, then empty. */
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    from_radio.my_info.my_node_num = 0x01020304U;
    from_radio.my_info.nodedb_count = 2U;

    pb_ostream_t encode_stream = pb_ostream_from_buffer(read_buffers[0], sizeof(read_buffers[0]));
    if (!pb_encode(&encode_stream, meshtastic_FromRadio_fields, &from_radio)) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "failed to encode my_info");
        return;
    }
    read_payload_lengths[0] = encode_stream.bytes_written;
    const uint32_t expected_node_num = from_radio.my_info.my_node_num;

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    from_radio.node_info.num = 0x01020305U;
    from_radio.node_info.has_user = true;
    snprintf(from_radio.node_info.user.long_name, sizeof(from_radio.node_info.user.long_name), "%s",
             "Alice Example");
    snprintf(from_radio.node_info.user.short_name, sizeof(from_radio.node_info.user.short_name),
             "%s", "AE");
    from_radio.node_info.last_heard = 1234U;
    from_radio.node_info.snr = 12.5f;
    from_radio.node_info.via_mqtt = true;
    from_radio.node_info.has_hops_away = true;
    from_radio.node_info.hops_away = 2U;

    encode_stream = pb_ostream_from_buffer(read_buffers[1], sizeof(read_buffers[1]));
    if (!pb_encode(&encode_stream, meshtastic_FromRadio_fields, &from_radio)) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "failed to encode node_info");
        return;
    }
    read_payload_lengths[1] = encode_stream.bytes_written;
    const uint32_t expected_peer_num = from_radio.node_info.num;

    /* Two more peers so the FIFO is longer than one turn's read budget. */
    for (size_t extra = 0; extra < 2U; ++extra) {
        from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
        from_radio.which_payload_variant = meshtastic_FromRadio_node_info_tag;
        from_radio.node_info.num = 0x01020306U + (uint32_t)extra;
        from_radio.node_info.has_user = true;
        snprintf(from_radio.node_info.user.short_name, sizeof(from_radio.node_info.user.short_name),
                 "P%zu", extra);
        encode_stream =
            pb_ostream_from_buffer(read_buffers[2 + extra], sizeof(read_buffers[2 + extra]));
        if (!pb_encode(&encode_stream, meshtastic_FromRadio_fields, &from_radio)) {
            ble->ops->stop(ble);
            mesh_event_loop_shutdown(&loop);
            mesh_bluez_client_mock_disable();
            record_failure(test_name, "failed to encode extra node_info");
            return;
        }
        read_payload_lengths[2 + extra] = encode_stream.bytes_written;
    }

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
    from_radio.config_complete_id = to_radio.want_config_id;

    encode_stream = pb_ostream_from_buffer(read_buffers[4], sizeof(read_buffers[4]));
    if (!pb_encode(&encode_stream, meshtastic_FromRadio_fields, &from_radio)) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "failed to encode config_complete");
        return;
    }
    read_payload_lengths[4] = encode_stream.bytes_written;

    /* Rewind the scripted FIFO and poke FromNum. */
    read_index = 0U;
    const uint8_t from_num[4] = {5U, 0U, 0U, 0U};
    mesh_bluez_client_mock_emit_notification(mock_config.fromnum_char_path, from_num,
                                             sizeof(from_num));

    /* The first turn reads its budget and must stop short of the end; the loop wake finishes it. */
    if (read_index >= 6U) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "drain did not yield to the event loop between read batches");
        return;
    }
    for (int spin = 0; spin < 20 && read_index < 6U; ++spin) {
        mesh_event_loop_run(&loop, 10);
        ble->ops->tick(ble);
    }

    handshake = mesh_ble_transport_handshake_status(ble);
    if (!handshake.has_my_info || handshake.my_info.my_node_num != expected_node_num) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "my_info not cached");
        return;
    }

    if (handshake.node_count != 3U || handshake.nodes[0].node_id != expected_peer_num) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "node info cache incorrect");
        return;
    }

    if (handshake.request_in_flight || !handshake.config_complete ||
        handshake.config_complete_id != to_radio.want_config_id) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "config handshake did not complete");
        return;
    }

    /* Five payloads plus the terminating empty read. */
    if (read_index != 6U) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "FromRadio drain did not read until empty");
        return;
    }

    struct mesh_ble_transport_stats stats = mesh_ble_transport_stats(ble);
    if (stats.frames_received != 5U) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "unexpected frame count after handshake");
        return;
    }

    /* Outbound packets go out as exactly one write each, unchunked. */
    size_t handshake_write_calls = write_call_count;
    uint8_t outbound_packet[300];
    for (size_t i = 0; i < sizeof(outbound_packet); ++i) {
        outbound_packet[i] = (uint8_t)i;
    }

    if (mesh_ble_transport_send_packet(ble, outbound_packet, sizeof(outbound_packet)) != 0) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "failed to queue outbound packet");
        return;
    }

    if (write_call_count - handshake_write_calls != 1U ||
        write_lengths[handshake_write_calls] != sizeof(outbound_packet)) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "outbound packet was not written as a single ToRadio write");
        return;
    }

    uint8_t oversized[MESH_BLE_MAX_PACKET_SIZE + 1U];
    memset(oversized, 0xAB, sizeof(oversized));
    if (mesh_ble_transport_send_packet(ble, oversized, sizeof(oversized)) != -EMSGSIZE) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "oversized packet should be rejected");
        return;
    }

    if (mesh_ble_transport_connect(ble, mock_devices[0].address) != -EALREADY) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "duplicate connect should return -EALREADY");
        return;
    }

    if (mesh_ble_transport_disconnect(ble) != 0) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "disconnect should succeed");
        return;
    }

    handshake = mesh_ble_transport_handshake_status(ble);
    if (handshake.request_in_flight || handshake.node_count != 0U || handshake.has_my_info ||
        handshake.config_complete) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "handshake state not cleared on disconnect");
        return;
    }

    if (mesh_ble_transport_disconnect(ble) != -ENOTCONN) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "second disconnect should return -ENOTCONN");
        return;
    }

    ble->ops->stop(ble);
    mesh_event_loop_shutdown(&loop);
    mesh_bluez_client_mock_disable();
    record_success(test_name);
}

/* Mirrors what a real node does on a fresh BlueZ cache: Connect returns, but the GATT
   characteristics only appear a few polls later. The connect must stay pending, not fail. */
MESH_TEST_CASE(ble_transport_connect_deferred_services, unit) {
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:05", .name = "NodeFive", .rssi = -50, .paired = true},
    };

    uint8_t write_capture[64];
    memset(write_capture, 0, sizeof(write_capture));
    size_t write_len = 0U;
    char write_path[128];
    memset(write_path, 0, sizeof(write_path));

    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .services_resolved_after_polls = 2U,
        .toradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_05/service000a/char000b",
        .fromradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_05/service000a/char000d",
        .fromnum_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_05/service000a/char000f",
        .devices = mock_devices,
        .device_count = 1U,
        .write_capture_buffer = write_capture,
        .write_capture_capacity = sizeof(write_capture),
        .write_capture_length = &write_len,
        .write_capture_path = write_path,
        .write_capture_path_capacity = sizeof(write_path),
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
    if (!mesh_ble_transport_is_connecting(ble)) {
        failure = "link should be connecting while services are unresolved";
        goto cleanup;
    }
    if (mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "must not report connected before service discovery";
        goto cleanup;
    }
    if (write_len != 0U) {
        failure = "want_config must wait for the characteristics";
        goto cleanup;
    }
    if (mesh_ble_transport_connect(ble, mock_devices[0].address) != -EINPROGRESS) {
        failure = "repeat connect should report in progress";
        goto cleanup;
    }
    if (mesh_ble_transport_connect(ble, "AA:BB:CC:DD:EE:99") != -EBUSY) {
        failure = "connect to another node should report busy";
        goto cleanup;
    }

    /* Second poll: still unresolved. Third poll: resolved, connect completes. */
    test_sleep_ms(300U);
    ble->ops->tick(ble);
    if (!mesh_ble_transport_is_connecting(ble) || write_len != 0U) {
        failure = "connect completed before the mock resolved services";
        goto cleanup;
    }

    test_sleep_ms(300U);
    ble->ops->tick(ble);
    if (mesh_ble_transport_is_connecting(ble)) {
        failure = "connect should have completed";
        goto cleanup;
    }
    const char *connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[0].address) != 0) {
        failure = "connected address mismatch after deferred connect";
        goto cleanup;
    }
    if (write_len == 0U || strcmp(write_path, mock_config.toradio_char_path) != 0) {
        failure = "want_config write missing after deferred connect";
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

/* Device1.Connect answers asynchronously; the link stays "connecting" (and the loop free)
   until the reply lands, and a refused connect drops back to disconnected. */
MESH_TEST_CASE(ble_transport_connect_async_reply, unit) {
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:08", .name = "NodeEight", .rssi = -50, .paired = true},
    };
    uint8_t write_capture[64];
    size_t write_len = 0U;
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .connect_pending_polls = 2U,
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
    if (!mesh_ble_transport_is_connecting(ble) || write_len != 0U) {
        failure = "must stay connecting until the Connect reply";
        goto cleanup;
    }
    if (strcmp(ble->ops->status(ble), "connecting") != 0) {
        failure = "status should read connecting";
        goto cleanup;
    }

    ble->ops->tick(ble); /* poll 2: still pending */
    if (!mesh_ble_transport_is_connecting(ble) || write_len != 0U) {
        failure = "reply arrived too early";
        goto cleanup;
    }
    ble->ops->tick(ble); /* poll 3: reply, services already resolved, handshake sent */
    if (mesh_ble_transport_connected_address(ble) == NULL || write_len == 0U) {
        failure = "connect should complete once the reply lands";
        goto cleanup;
    }
    if (strcmp(ble->ops->status(ble), "connected") != 0) {
        failure = "status should read connected";
        goto cleanup;
    }

    /* A refused Connect must leave the link disconnected. */
    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "disconnect failed";
        goto cleanup;
    }
    ble->ops->stop(ble);
    mock_config.connect_result = -EIO;
    mock_config.connect_pending_polls = 1U;
    mesh_bluez_client_mock_enable(&mock_config);
    if (ble->ops->start(ble, &config, &loop) != 0) {
        failure = "ble restart failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);
    if (mesh_ble_transport_connect(ble, mock_devices[0].address) != 0) {
        failure = "second connect should be accepted";
        goto cleanup;
    }
    ble->ops->tick(ble);
    if (mesh_ble_transport_is_connecting(ble) ||
        mesh_ble_transport_connected_address(ble) != NULL ||
        strcmp(ble->ops->status(ble), "running") != 0) {
        failure = "refused connect should drop back to running/disconnected";
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

/* BlueZ says the device is gone: the link resets, the UI sees "running", and auto-connect can
   try again. Checked via the explicit probe tick() runs every couple of seconds. */
MESH_TEST_CASE(ble_transport_link_drop, unit) {
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:0A", .name = "NodeTen", .rssi = -50, .paired = true},
    };
    uint8_t write_capture[64];
    size_t write_len = 0U;
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .connected_drops_after_polls = 2U, /* tick() already probes once on connect */
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
    ble->ops->tick(ble);
    if (mesh_ble_transport_connected_address(ble) == NULL) {
        failure = "expected a connected link";
        goto cleanup;
    }

    /* A message in flight when the link drops ends up FAILED, not PENDING forever. */
    uint32_t packet_id = 0U;
    if (mesh_ble_transport_send_text(ble, 0x11223344U, 0U, "hello", true, &packet_id) != 0) {
        failure = "send should succeed while connected";
        goto cleanup;
    }

    if (mesh_ble_transport_check_link(ble) != 1) {
        failure = "first probe should find the link up";
        goto cleanup;
    }
    if (mesh_ble_transport_check_link(ble) != 0) {
        failure = "second probe should find the link down and reset it";
        goto cleanup;
    }
    if (mesh_ble_transport_connected_address(ble) != NULL ||
        mesh_ble_transport_is_connecting(ble) || strcmp(ble->ops->status(ble), "running") != 0) {
        failure = "link should be reset after the drop";
        goto cleanup;
    }
    if (mesh_ble_transport_check_link(ble) != -ENOTCONN) {
        failure = "probe while disconnected should say so";
        goto cleanup;
    }
    /* The message was already written, so it stays pending (the radio may still ack it);
       the conversation itself survives the reset. */
    const struct mesh_message_log *log = mesh_ble_transport_messages(ble);
    if (log == NULL || log->count != 1U || mesh_message_log_at(log, 0)->packet_id != packet_id ||
        mesh_message_log_at(log, 0)->ack != MESH_MESSAGE_ACK_PENDING) {
        failure = "message log should survive a link reset";
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

/* A failing GATT write is a dead link: send_text reports the error, marks the message FAILED
   and the link resets instead of pretending the message went out. */
MESH_TEST_CASE(ble_transport_write_failure, unit) {
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:0B", .name = "NodeEleven", .rssi = -50, .paired = true},
    };
    uint8_t write_capture[64];
    size_t write_len = 0U;
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .write_fail_after_calls = 1U, /* the handshake write succeeds, the message does not */
        .write_result_late = -EIO,
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
    ble->ops->tick(ble);
    if (mesh_ble_transport_connected_address(ble) == NULL) {
        failure = "expected a connected link";
        goto cleanup;
    }

    uint32_t packet_id = 0U;
    const int result =
        mesh_ble_transport_send_text(ble, 0x11223344U, 0U, "hello", true, &packet_id);
    if (result != -EIO) {
        failure = "send_text should surface the write error";
        goto cleanup;
    }
    if (mesh_ble_transport_connected_address(ble) != NULL ||
        strcmp(ble->ops->status(ble), "running") != 0) {
        failure = "a failed write should drop the link";
        goto cleanup;
    }
    const struct mesh_message_log *log = mesh_ble_transport_messages(ble);
    if (log == NULL || log->count != 1U ||
        mesh_message_log_at(log, 0)->ack != MESH_MESSAGE_ACK_FAILED) {
        failure = "the unsent message should be marked failed";
        goto cleanup;
    }
    if (mesh_ble_transport_send_text(ble, 0x11223344U, 0U, "again", true, NULL) != -ENOTCONN) {
        failure = "sending while disconnected should say so";
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
