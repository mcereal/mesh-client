#define _POSIX_C_SOURCE 200809L

#include "mesh/config.h"
#include "mesh/event_loop.h"
#include "mesh/mesh_message.h"
#include "mesh/proto/framing.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/transport.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/backends/minui.h"
#include "mesh/ui/backends/stub.h"
#include "mesh/ui/controller.h"
#include "mesh/ui/input.h"
#include "mesh/ui/preferences.h"
#include "mesh/ui/store.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/mesh.pb.h"

#include <errno.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct test_case;

static int g_failures = 0;
static size_t g_successes = 0U;

static void record_failure(const char *test_name, const char *message) {
    fprintf(stderr, "[FAIL] %s: %s\n", test_name, message);
    ++g_failures;
}

static void record_success(const char *test_name) {
    (void)test_name;
    ++g_successes;
}

static void test_config_defaults(void) {
    const char *test_name = "config_defaults";
    struct mesh_app_config config = mesh_app_config_default();
    if (config.run_mode != MESH_APP_RUN_SINGLE_POLL) {
        record_failure(test_name, "run_mode should default to single poll");
        return;
    }
    if (!config.enable_ble) {
        record_failure(test_name, "BLE should be enabled by default");
        return;
    }
    if (config.idle_timeout_ms != 1000) {
        record_failure(test_name, "idle timeout should default to 1000 ms");
        return;
    }
    record_success(test_name);
}

static void test_transport_registry_registration(void) {
    const char *test_name = "transport_registry_registration";

    struct mesh_transport_registry registry;
    mesh_transport_registry_init(&registry);

    struct mesh_transport *ble = mesh_ble_transport();
    int result = mesh_transport_registry_register(&registry, ble);
    if (result != 0) {
        record_failure(test_name, "expected first BLE registration to succeed");
        return;
    }

    result = mesh_transport_registry_register(&registry, ble);
    if (result != -EEXIST) {
        record_failure(test_name, "duplicate registration should return -EEXIST");
        return;
    }

    record_success(test_name);
}

static void test_event_loop_init_shutdown(void) {
    const char *test_name = "event_loop_init_shutdown";

    struct mesh_event_loop loop;
    int result = mesh_event_loop_init(&loop);
    if (result < 0) {
        record_failure(test_name, "mesh_event_loop_init failed");
        return;
    }

    result = mesh_event_loop_run(&loop, 0);
    if (result < 0) {
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "mesh_event_loop_run should succeed with zero timeout");
        return;
    }

    mesh_event_loop_shutdown(&loop);
    record_success(test_name);
}

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

static void test_ble_transport_status_transitions(void) {
    const char *test_name = "ble_transport_status_transitions";

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

static void test_ble_transport_discovery_mock(void) {
    const char *test_name = "ble_transport_discovery_mock";

    struct mesh_transport *ble = mesh_ble_transport();

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:01", .name = "NodeOne", .rssi = -45},
        {.address = "AA:BB:CC:DD:EE:02", .name = "NodeTwo", .rssi = -60},
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

static void test_ble_transport_connect_mock(void) {
    const char *test_name = "ble_transport_connect_mock";

    struct mesh_transport *ble = mesh_ble_transport();

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:03", .name = "NodeThree", .rssi = -40},
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

    struct mesh_ble_handshake_status handshake = mesh_ble_transport_handshake_status(ble);
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

static void test_ui_store_basic(void) {
    const char *test_name = "ui_store_basic";

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }

    struct mesh_ui_device devices[2] = {
        {.identifier = "AA:BB:CC:DD:EE:01", .name = "NodeOne", .rssi = -45, .connected = false},
        {.identifier = "AA:BB:CC:DD:EE:02", .name = "NodeTwo", .rssi = -60, .connected = true},
    };

    mesh_ui_store_set_discovery(&store, devices, 2U);

    struct mesh_ui_snapshot snapshot;
    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "expected discovery update");
        return;
    }

    if ((snapshot.update_flags & MESH_UI_UPDATE_DISCOVERY) == 0U || snapshot.device_count != 2U) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "discovery data not reflected in snapshot");
        return;
    }

    mesh_ui_store_set_discovery(&store, devices, 2U);
    if (mesh_ui_store_consume_updates(&store, &snapshot)) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "duplicate discovery should not trigger update");
        return;
    }

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof(handshake));
    handshake.request_in_flight = true;
    handshake.request_id = 42U;
    handshake.config_complete = false;
    handshake.config_complete_id = 0U;
    handshake.node_count = 1U;
    handshake.has_my_info = true;
    handshake.my_info.node_num = 1234U;
    handshake.my_info.nodedb_entries = 5U;
    handshake.my_info.reboot_count = 2U;
    snprintf(handshake.my_short_name, sizeof(handshake.my_short_name), "%s", "ME");
    handshake.has_config = false;
    snprintf(handshake.primary_channel, sizeof(handshake.primary_channel), "%s", "LongRange");
    handshake.nodes[0].node_id = 1234U;
    snprintf(handshake.nodes[0].long_name, sizeof(handshake.nodes[0].long_name), "%s", "LocalNode");
    snprintf(handshake.nodes[0].short_name, sizeof(handshake.nodes[0].short_name), "%s", "ME");
    handshake.nodes[0].snr = 12.5f;
    handshake.nodes[0].last_heard = 99U;
    mesh_ui_store_set_handshake(&store, &handshake);

    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "expected handshake update");
        return;
    }

    if ((snapshot.update_flags & MESH_UI_UPDATE_HANDSHAKE) == 0U || !snapshot.handshake_valid) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "handshake data missing from snapshot");
        return;
    }

    if (snapshot.handshake.nodes[0].node_id != handshake.nodes[0].node_id ||
        snapshot.handshake.nodes[0].last_heard != handshake.nodes[0].last_heard ||
        snapshot.handshake.nodes[0].snr != handshake.nodes[0].snr) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "handshake node summary not propagated");
        return;
    }

    mesh_ui_store_set_handshake(&store, &handshake);
    if (mesh_ui_store_consume_updates(&store, &snapshot)) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "duplicate handshake should not trigger update");
        return;
    }

    mesh_ui_store_set_handshake(&store, NULL);
    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "expected handshake reset update");
        return;
    }

    if (snapshot.handshake_valid) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "handshake state did not clear");
        return;
    }

    mesh_ui_store_set_handshake(&store, NULL);
    if (mesh_ui_store_consume_updates(&store, &snapshot)) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "duplicate handshake reset should not trigger update");
        return;
    }

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

static void test_ui_store_persistence(void) {
    const char *test_name = "ui_store_persistence";

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof(handshake));
    handshake.request_in_flight = true;
    handshake.request_id = 11U;
    handshake.config_complete = true;
    handshake.config_complete_id = 77U;
    handshake.has_my_info = true;
    handshake.my_info.node_num = 4242U;
    handshake.my_info.nodedb_entries = 3U;
    handshake.my_info.reboot_count = 1U;
    snprintf(handshake.primary_channel, sizeof(handshake.primary_channel), "%s", "LongRange");
    snprintf(handshake.my_short_name, sizeof(handshake.my_short_name), "%s", "NODE");
    handshake.node_count = 1U;
    handshake.nodes[0].node_id = 4242U;
    snprintf(handshake.nodes[0].long_name, sizeof(handshake.nodes[0].long_name), "%s", "Primary");
    snprintf(handshake.nodes[0].short_name, sizeof(handshake.nodes[0].short_name), "%s", "PRIM");
    handshake.nodes[0].snr = 9.5f;
    handshake.nodes[0].last_heard = 123U;
    handshake.cached = true;
    mesh_ui_store_set_handshake(&store, &handshake);

    char cache_path[] = "/tmp/mesh_ui_storeXXXXXX";
    int fd = mkstemp(cache_path);
    if (fd < 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "mkstemp failed");
        return;
    }
    close(fd);

    if (mesh_ui_store_save(&store, cache_path) != 0) {
        unlink(cache_path);
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "save failed");
        return;
    }

    FILE *saved = fopen(cache_path, "r");
    bool cached_marker_found = false;
    if (saved != NULL) {
        char line[256];
        while (fgets(line, sizeof line, saved) != NULL) {
            if (strncmp(line, "handshake_cached=", (int)(sizeof "handshake_cached=") - 1) == 0) {
                cached_marker_found = (strstr(line, "=1") != NULL);
            }
        }
        fclose(saved);
    }
    if (!cached_marker_found) {
        unlink(cache_path);
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "handshake cache marker missing");
        return;
    }

    mesh_ui_store_shutdown(&store);

    if (mesh_ui_store_init(&store) != 0) {
        unlink(cache_path);
        record_failure(test_name, "store reinit failed");
        return;
    }

    if (mesh_ui_store_load(&store, cache_path) != 0) {
        unlink(cache_path);
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "load failed");
        return;
    }

    unlink(cache_path);

    if (!store.handshake_valid || store.handshake.request_id != handshake.request_id ||
        store.handshake.config_complete_id != handshake.config_complete_id ||
        store.handshake.node_count != handshake.node_count ||
        store.handshake.nodes[0].node_id != handshake.nodes[0].node_id) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "handshake mismatch after load");
        return;
    }

    if (!store.handshake.cached) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "handshake cache flag not set after load");
        return;
    }

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

static void test_ui_controller_dispatch(void) {
    const char *test_name = "ui_controller_dispatch";

    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        return;
    }

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "store init failed");
        return;
    }

    struct mesh_ui_backend_stub_context context;
    memset(&context, 0, sizeof context);

    struct mesh_ui_controller controller;
    if (mesh_ui_controller_init(&controller, &store, mesh_ui_backend_stub(), &context, &loop) !=
        0) {
        mesh_ui_store_shutdown(&store);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "controller init failed");
        return;
    }

    struct mesh_ui_device devices[1] = {
        {.identifier = "AA:BB:CC:DD:EE:01", .name = "NodeOne", .rssi = -50, .connected = false},
    };
    mesh_ui_store_set_discovery(&store, devices, 1U);
    mesh_event_loop_run(&loop, 0);

    if (!context.has_snapshot || context.present_calls == 0U) {
        mesh_ui_controller_shutdown(&controller);
        mesh_ui_store_shutdown(&store);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "backend did not receive discovery update");
        return;
    }

    if (context.last_snapshot.device_count != 1U ||
        (context.last_snapshot.update_flags & MESH_UI_UPDATE_DISCOVERY) == 0U) {
        mesh_ui_controller_shutdown(&controller);
        mesh_ui_store_shutdown(&store);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "snapshot content mismatch");
        return;
    }

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof(handshake));
    handshake.config_complete = true;
    handshake.config_complete_id = 7U;
    handshake.request_in_flight = false;
    handshake.request_id = 7U;
    handshake.node_count = 2U;
    handshake.has_my_info = false;
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_event_loop_run(&loop, 0);

    if (!context.has_snapshot ||
        (context.last_snapshot.update_flags & MESH_UI_UPDATE_HANDSHAKE) == 0U ||
        !context.last_snapshot.handshake_valid) {
        mesh_ui_controller_shutdown(&controller);
        mesh_ui_store_shutdown(&store);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "backend did not receive handshake update");
        return;
    }

    mesh_ui_controller_shutdown(&controller);
    mesh_ui_store_shutdown(&store);
    mesh_event_loop_shutdown(&loop);
    record_success(test_name);
}

static void test_ui_preferences_roundtrip(void) {
    const char *test_name = "ui_preferences_roundtrip";

    char prefab_path[128];
    snprintf(prefab_path, sizeof prefab_path, "/tmp/meshclient_prefs_%ld", (long)getpid());
    FILE *temp = fopen(prefab_path, "w");
    if (temp == NULL) {
        record_failure(test_name, "failed to create temp file");
        return;
    }
    fclose(temp);

    struct mesh_ui_preferences prefs;
    memset(&prefs, 0, sizeof prefs);
    snprintf(prefs.preferred_device, sizeof prefs.preferred_device, "%s", "AA:BB:CC:DD:EE:01");
    snprintf(prefs.preferred_channel, sizeof prefs.preferred_channel, "%s", "LongRange");

    if (mesh_ui_preferences_save(&prefs, prefab_path) != 0) {
        unlink(prefab_path);
        record_failure(test_name, "save failed");
        return;
    }

    struct mesh_ui_preferences loaded;
    if (mesh_ui_preferences_load(&loaded, prefab_path) != 0) {
        unlink(prefab_path);
        record_failure(test_name, "load failed");
        return;
    }

    if (strcmp(loaded.preferred_device, prefs.preferred_device) != 0 ||
        strcmp(loaded.preferred_channel, prefs.preferred_channel) != 0) {
        unlink(prefab_path);
        record_failure(test_name, "roundtrip mismatch");
        return;
    }

    unlink(prefab_path);
    record_success(test_name);
}

static void test_minui_format_menu(void) {
    const char *test_name = "minui_format_menu";

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);

    snapshot.device_count = 2U;
    snprintf(snapshot.devices[0].identifier, sizeof snapshot.devices[0].identifier, "%s",
             "AA:BB:CC:DD:EE:01");
    snprintf(snapshot.devices[0].name, sizeof snapshot.devices[0].name, "%s", "NodeOne");
    snapshot.devices[0].rssi = -42;
    snapshot.devices[0].connected = true;

    snprintf(snapshot.devices[1].identifier, sizeof snapshot.devices[1].identifier, "%s",
             "AA:BB:CC:DD:EE:02");
    snprintf(snapshot.devices[1].name, sizeof snapshot.devices[1].name, "%s", "NodeTwo");
    snapshot.devices[1].rssi = -60;
    snapshot.devices[1].connected = false;

    snapshot.handshake_valid = true;
    snapshot.handshake.request_in_flight = false;
    snapshot.handshake.request_id = 5U;
    snapshot.handshake.config_complete = true;
    snapshot.handshake.config_complete_id = 5U;
    snapshot.handshake.node_count = 2U;
    snprintf(snapshot.handshake.my_short_name, sizeof snapshot.handshake.my_short_name, "%s",
             "ABCD");
    snapshot.handshake.nodes[0].node_id = 101U;
    snprintf(snapshot.handshake.nodes[0].long_name, sizeof snapshot.handshake.nodes[0].long_name,
             "%s", "BaseStation");
    snprintf(snapshot.handshake.nodes[0].short_name, sizeof snapshot.handshake.nodes[0].short_name,
             "%s", "BASE");
    snapshot.handshake.nodes[0].snr = 8.5f;
    snapshot.handshake.nodes[1].node_id = 202U;
    snprintf(snapshot.handshake.nodes[1].long_name, sizeof snapshot.handshake.nodes[1].long_name,
             "%s", "FieldUnit");
    snprintf(snapshot.handshake.nodes[1].short_name, sizeof snapshot.handshake.nodes[1].short_name,
             "%s", "FILD");
    snapshot.handshake.nodes[1].snr = 4.0f;

    char buffer[1024];
    int result = mesh_ui_backend_minui_format_menu(&snapshot, buffer, sizeof buffer);
    if (result != 0) {
        record_failure(test_name, "formatting returned error");
        return;
    }

    if (strstr(buffer, "NodeOne") == NULL || strstr(buffer, "NodeTwo") == NULL) {
        record_failure(test_name, "device names missing from JSON");
        return;
    }

    if (strstr(buffer, "\"Status\"") == NULL) {
        record_failure(test_name, "status block missing expected fields");
        return;
    }

    if (strstr(buffer, "\"selected\":0") == NULL) {
        record_failure(test_name, "expected connected device to be selected");
        return;
    }

    if (strstr(buffer, "\"Nodes\"") == NULL || strstr(buffer, "BaseStation") == NULL) {
        record_failure(test_name, "nodes section missing from JSON");
        return;
    }

    record_success(test_name);
}

static void test_proto_varint_roundtrip(void) {
    const char *test_name = "proto_varint_roundtrip";
    struct {
        uint32_t value;
        size_t expected_len;
    } cases[] = {
        {0U, 1U}, {1U, 1U}, {127U, 1U}, {128U, 2U}, {16384U, 3U}, {268435455U, 4U},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t buffer[8];
        size_t written = 0;
        if (mesh_proto_varint_encode(cases[i].value, buffer, sizeof buffer, &written) != 0) {
            record_failure(test_name, "varint encode failed");
            return;
        }
        if (written != cases[i].expected_len) {
            record_failure(test_name, "unexpected encoded length");
            return;
        }

        uint32_t decoded = 0;
        size_t consumed = 0;
        if (mesh_proto_varint_decode(buffer, written, &decoded, &consumed) != 0) {
            record_failure(test_name, "varint decode failed");
            return;
        }
        if (decoded != cases[i].value || consumed != written) {
            record_failure(test_name, "varint roundtrip mismatch");
            return;
        }
    }

    record_success(test_name);
}

static void test_proto_frame_encode_decode(void) {
    const char *test_name = "proto_frame_encode_decode";
    const uint8_t payload[] = {0x08U, 0x96U, 0x01U};
    uint8_t frame[16];
    size_t written = 0;

    if (mesh_proto_frame_encode(payload, sizeof payload, frame, sizeof frame, &written) != 0) {
        record_failure(test_name, "frame encode failed");
        return;
    }

    size_t header_len = 0;
    size_t payload_len = 0;
    if (mesh_proto_frame_decode(frame, written, &header_len, &payload_len) != 0) {
        record_failure(test_name, "frame decode failed");
        return;
    }

    if (payload_len != sizeof payload) {
        record_failure(test_name, "decoded payload length mismatch");
        return;
    }

    for (size_t i = 0; i < payload_len; ++i) {
        if (frame[header_len + i] != payload[i]) {
            record_failure(test_name, "payload content mismatch");
            return;
        }
    }

    record_success(test_name);
}

/* Regression: the store deliberately stays quiet when nothing changed, so a client that
   comes up with no devices and no handshake never got a snapshot - and the framebuffer
   backend never painted, leaving a black screen on the device. */
static void test_ui_store_refresh_request(void) {
    const char *test_name = "ui_store_refresh_request";

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }

    struct mesh_ui_snapshot snapshot;

    /* An untouched store publishes nothing: this is the black-screen condition. */
    mesh_ui_store_set_discovery(&store, NULL, 0U);
    if (mesh_ui_store_consume_updates(&store, &snapshot)) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "empty discovery should not signal an update");
        return;
    }

    mesh_ui_store_request_refresh(&store);
    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "refresh request should yield a snapshot");
        return;
    }

    if (snapshot.device_count != 0U || snapshot.handshake_valid) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "refreshed snapshot should report an empty state");
        return;
    }

    /* And the refresh is one-shot, not a permanently dirty store. */
    if (mesh_ui_store_consume_updates(&store, &snapshot)) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "refresh should be consumed exactly once");
        return;
    }

    mesh_ui_store_set_transport_status(&store, "waiting-for-bluez");
    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "transport status change should signal an update");
        return;
    }

    if ((snapshot.update_flags & MESH_UI_UPDATE_TRANSPORT) == 0U ||
        strcmp(snapshot.transport_status, "waiting-for-bluez") != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "transport status not reflected in snapshot");
        return;
    }

    mesh_ui_store_set_transport_status(&store, "waiting-for-bluez");
    if (mesh_ui_store_consume_updates(&store, &snapshot)) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "duplicate transport status should not signal");
        return;
    }

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* The device has no console, so the quit mapping has to be correctable from launch.sh
   without a rebuild. */
static void test_ui_input_quit_keys(void) {
    const char *test_name = "ui_input_quit_keys";

    unsetenv("MESHCLIENT_QUIT_KEYS");
    mesh_ui_input_reload_quit_keys();

    if (!mesh_ui_input_is_quit_key(KEY_MENU) || !mesh_ui_input_is_quit_key(KEY_ESC)) {
        record_failure(test_name, "default quit keys should include MENU and ESC");
        return;
    }

    /* BTN_START stays free for the menu work still to come. */
    if (mesh_ui_input_is_quit_key(BTN_START)) {
        record_failure(test_name, "BTN_START should not quit by default");
        return;
    }

    if (mesh_ui_input_quit_hint() == NULL || mesh_ui_input_quit_hint()[0] == '\0') {
        record_failure(test_name, "quit hint should not be empty");
        return;
    }

    setenv("MESHCLIENT_QUIT_KEYS", "300, 301", 1);
    mesh_ui_input_reload_quit_keys();

    if (!mesh_ui_input_is_quit_key(300U) || !mesh_ui_input_is_quit_key(301U)) {
        unsetenv("MESHCLIENT_QUIT_KEYS");
        mesh_ui_input_reload_quit_keys();
        record_failure(test_name, "override should install the listed codes");
        return;
    }

    if (mesh_ui_input_is_quit_key(KEY_MENU)) {
        unsetenv("MESHCLIENT_QUIT_KEYS");
        mesh_ui_input_reload_quit_keys();
        record_failure(test_name, "override should replace the defaults, not extend them");
        return;
    }

    /* A garbage override must fall back rather than leave nothing able to quit. */
    setenv("MESHCLIENT_QUIT_KEYS", "not-a-code", 1);
    mesh_ui_input_reload_quit_keys();
    if (!mesh_ui_input_is_quit_key(KEY_MENU)) {
        unsetenv("MESHCLIENT_QUIT_KEYS");
        mesh_ui_input_reload_quit_keys();
        record_failure(test_name, "unparseable override should fall back to defaults");
        return;
    }

    unsetenv("MESHCLIENT_QUIT_KEYS");
    mesh_ui_input_reload_quit_keys();
    record_success(test_name);
}

/* Regression: the transport line used to be printed from inside print_devices(), which only
   runs for MESH_UI_UPDATE_DISCOVERY. A BLE state change that did not also change the device
   list (waiting-for-bluez -> waiting-for-adapter) therefore never reached the console. */
static void test_ui_cli_transport_update(void) {
    const char *test_name = "ui_cli_transport_update";

    const struct mesh_ui_backend *backend = mesh_ui_backend_cli();
    if (backend == NULL || backend->present == NULL) {
        record_failure(test_name, "cli backend unavailable");
        return;
    }

    struct mesh_ui_backend_cli_context context;
    memset(&context, 0, sizeof context);

    /* present() also writes to stderr; tty_stream is the part we can capture. */
    FILE *capture = tmpfile();
    if (capture == NULL) {
        record_failure(test_name, "tmpfile failed");
        return;
    }
    context.tty_stream = capture;

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.update_flags = MESH_UI_UPDATE_TRANSPORT;
    snprintf(snapshot.transport_status, sizeof snapshot.transport_status, "waiting-for-adapter");

    backend->present(&context, &snapshot, &context);

    fflush(capture);
    rewind(capture);
    char buffer[512];
    const size_t read_bytes = fread(buffer, 1, sizeof(buffer) - 1U, capture);
    buffer[read_bytes] = '\0';
    fclose(capture);

    if (strstr(buffer, "waiting-for-adapter") == NULL) {
        record_failure(test_name, "transport-only update should still print the transport line");
        return;
    }

    record_success(test_name);
}

struct test_case {
    const char *name;
    const char *category;
    void (*fn)(void);
};

/* Builds a decoded MeshPacket carrying `payload` on `portnum`, as the radio would hand it to
   us inside a FromRadio. */
static meshtastic_MeshPacket make_decoded_packet(uint32_t from, uint32_t to, uint8_t channel,
                                                 uint32_t id, meshtastic_PortNum portnum,
                                                 const void *payload, size_t payload_len) {
    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_default;
    packet.from = from;
    packet.to = to;
    packet.channel = channel;
    packet.id = id;
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum = portnum;
    if (payload_len > sizeof(packet.decoded.payload.bytes)) {
        payload_len = sizeof(packet.decoded.payload.bytes);
    }
    memcpy(packet.decoded.payload.bytes, payload, payload_len);
    packet.decoded.payload.size = (pb_size_t)payload_len;
    return packet;
}

/*
 * Golden frame for the wire format, derived by hand from the .proto field numbers rather than
 * from our own encoder - the point is to notice if the encoding ever drifts.
 *
 * ToRadio.packet          field 1, LEN      -> 0x0A, len 0x12
 *   MeshPacket.to         field 2, FIXED32  -> 0x15, FF FF FF FF (broadcast)
 *   MeshPacket.decoded    field 4, LEN      -> 0x22, len 0x06
 *     Data.portnum        field 1, varint   -> 0x08, 0x01 (TEXT_MESSAGE_APP)
 *     Data.payload        field 2, LEN      -> 0x12, len 0x02, "hi"
 *   MeshPacket.id         field 6, FIXED32  -> 0x35, 2A 00 00 00
 * channel 0 and want_ack false are singular defaults and are omitted by nanopb.
 */
static void test_message_encode_text_golden(void) {
    const char *test_name = "message_encode_text_golden";
    static const uint8_t k_expected[] = {0x0A, 0x12, 0x15, 0xFF, 0xFF, 0xFF, 0xFF,
                                         0x22, 0x06, 0x08, 0x01, 0x12, 0x02, 0x68,
                                         0x69, 0x35, 0x2A, 0x00, 0x00, 0x00};

    struct mesh_message_text_request request = {
        .dest = MESH_MESSAGE_BROADCAST_ADDR,
        .packet_id = 42U,
        .text = "hi",
        .channel = 0U,
        .hop_limit = 0U,
        .want_ack = false,
    };

    uint8_t buffer[64];
    size_t written = 0U;
    if (mesh_message_encode_text(&request, buffer, sizeof buffer, &written) != 0) {
        record_failure(test_name, "encode failed");
        return;
    }

    if (written != sizeof k_expected) {
        record_failure(test_name, "encoded length does not match the golden frame");
        return;
    }

    if (memcmp(buffer, k_expected, sizeof k_expected) != 0) {
        record_failure(test_name, "encoded bytes do not match the golden frame");
        return;
    }

    record_success(test_name);
}

static void test_message_encode_text_roundtrip(void) {
    const char *test_name = "message_encode_text_roundtrip";
    struct mesh_message_text_request request = {
        .dest = 0x433D1A2CU,
        .packet_id = 0x1234U,
        .text = "hello mesh",
        .channel = 2U,
        .hop_limit = 3U,
        .want_ack = true,
    };

    uint8_t buffer[256];
    size_t written = 0U;
    if (mesh_message_encode_text(&request, buffer, sizeof buffer, &written) != 0) {
        record_failure(test_name, "encode failed");
        return;
    }

    meshtastic_ToRadio decoded = meshtastic_ToRadio_init_default;
    pb_istream_t stream = pb_istream_from_buffer(buffer, written);
    if (!pb_decode(&stream, meshtastic_ToRadio_fields, &decoded)) {
        record_failure(test_name, "decode failed");
        return;
    }

    if (decoded.which_payload_variant != meshtastic_ToRadio_packet_tag) {
        record_failure(test_name, "expected a packet variant");
        return;
    }

    const meshtastic_MeshPacket *packet = &decoded.packet;
    if (packet->to != request.dest || packet->id != request.packet_id ||
        packet->channel != request.channel || packet->hop_limit != request.hop_limit ||
        !packet->want_ack) {
        record_failure(test_name, "packet header fields did not survive the roundtrip");
        return;
    }

    /* The firmware stamps `from` itself; a client must not claim a node number. */
    if (packet->from != 0U) {
        record_failure(test_name, "from should be left for the firmware to fill in");
        return;
    }

    if (packet->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        packet->decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) {
        record_failure(test_name, "expected a decoded TEXT_MESSAGE_APP payload");
        return;
    }

    if (packet->decoded.payload.size != strlen(request.text) ||
        memcmp(packet->decoded.payload.bytes, request.text, strlen(request.text)) != 0) {
        record_failure(test_name, "payload text mismatch");
        return;
    }

    record_success(test_name);
}

static void test_message_encode_text_limits(void) {
    const char *test_name = "message_encode_text_limits";
    uint8_t buffer[512];
    size_t written = 0U;

    struct mesh_message_text_request request = {
        .dest = MESH_MESSAGE_BROADCAST_ADDR,
        .packet_id = 1U,
        .text = "",
        .channel = 0U,
        .hop_limit = 0U,
        .want_ack = false,
    };

    if (mesh_message_encode_text(&request, buffer, sizeof buffer, &written) != -EINVAL) {
        record_failure(test_name, "empty text should be rejected");
        return;
    }

    char oversized[MESH_MESSAGE_TEXT_MAX + 8U];
    memset(oversized, 'a', sizeof oversized - 1U);
    oversized[sizeof oversized - 1U] = '\0';
    request.text = oversized;
    if (mesh_message_encode_text(&request, buffer, sizeof buffer, &written) != -EMSGSIZE) {
        record_failure(test_name, "oversized text should be rejected with -EMSGSIZE");
        return;
    }

    record_success(test_name);
}

static void test_message_log_ring(void) {
    const char *test_name = "message_log_ring";
    struct mesh_message_log log;
    mesh_message_log_reset(&log);

    /* Overfill by four so eviction, ordering and the dropped counter all get exercised. */
    const size_t total = MESH_MESSAGE_LOG_CAPACITY + 4U;
    for (size_t i = 0; i < total; ++i) {
        struct mesh_message message;
        memset(&message, 0, sizeof(message));
        message.packet_id = (uint32_t)(i + 1U);
        snprintf(message.text, sizeof(message.text), "message %zu", i);
        if (mesh_message_log_append(&log, &message) == NULL) {
            record_failure(test_name, "append failed");
            return;
        }
    }

    if (log.count != MESH_MESSAGE_LOG_CAPACITY) {
        record_failure(test_name, "log should saturate at capacity");
        return;
    }

    if (log.dropped != 4U) {
        record_failure(test_name, "dropped counter should report the evicted entries");
        return;
    }

    /* Index 0 is the oldest surviving entry, which is the fifth one we appended. */
    const struct mesh_message *oldest = mesh_message_log_at(&log, 0U);
    if (oldest == NULL || oldest->packet_id != 5U) {
        record_failure(test_name, "oldest surviving entry is wrong");
        return;
    }

    const struct mesh_message *newest = mesh_message_log_at(&log, log.count - 1U);
    if (newest == NULL || newest->packet_id != (uint32_t)total) {
        record_failure(test_name, "newest entry is wrong");
        return;
    }

    if (mesh_message_log_at(&log, log.count) != NULL) {
        record_failure(test_name, "out-of-range index should return NULL");
        return;
    }

    if (mesh_message_log_find(&log, 1U) != NULL) {
        record_failure(test_name, "an evicted packet id should not be found");
        return;
    }

    record_success(test_name);
}

static void test_message_ingest_text(void) {
    const char *test_name = "message_ingest_text";
    struct mesh_message_log log;
    mesh_message_log_reset(&log);

    /* Control bytes are what a hostile or buggy node can put on the wire; they must not reach
       a framebuffer or a log line intact. */
    const char payload[] = "hi\nthere\x01!";
    meshtastic_MeshPacket packet =
        make_decoded_packet(0x11111111U, 0x22222222U, 3U, 77U, meshtastic_PortNum_TEXT_MESSAGE_APP,
                            payload, sizeof(payload) - 1U);
    packet.has_rx_time = true;
    packet.rx_time = 1234U;
    packet.rx_snr = 5.5F;

    if (mesh_message_ingest(&log, &packet, 0x22222222U) != 1) {
        record_failure(test_name, "a text packet should be appended");
        return;
    }

    const struct mesh_message *message = mesh_message_log_at(&log, 0U);
    if (message == NULL) {
        record_failure(test_name, "appended message not readable");
        return;
    }

    if (message->direction != MESH_MESSAGE_INBOUND) {
        record_failure(test_name, "a packet from another node is inbound");
        return;
    }

    if (message->from != 0x11111111U || message->to != 0x22222222U || message->channel != 3U ||
        message->packet_id != 77U || message->rx_time != 1234U) {
        record_failure(test_name, "packet metadata did not survive ingest");
        return;
    }

    if (strcmp(message->text, "hi there?!") != 0) {
        record_failure(test_name, "control bytes should be folded to space and '?'");
        return;
    }

    record_success(test_name);
}

static void test_message_ingest_ignores_other_payloads(void) {
    const char *test_name = "message_ingest_ignores_other_payloads";
    struct mesh_message_log log;
    mesh_message_log_reset(&log);

    meshtastic_MeshPacket position =
        make_decoded_packet(1U, 2U, 0U, 5U, meshtastic_PortNum_POSITION_APP, "\x01\x02", 2U);
    if (mesh_message_ingest(&log, &position, 2U) != 0 || log.count != 0U) {
        record_failure(test_name, "a non-text portnum should add nothing");
        return;
    }

    /* We hold no channel keys, so an encrypted packet is opaque and must be skipped rather
       than parsed as though its bytes were a Data message. */
    meshtastic_MeshPacket encrypted = meshtastic_MeshPacket_init_default;
    encrypted.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
    encrypted.encrypted.size = 4U;
    memcpy(encrypted.encrypted.bytes, "\xDE\xAD\xBE\xEF", 4U);
    if (mesh_message_ingest(&log, &encrypted, 2U) != 0 || log.count != 0U) {
        record_failure(test_name, "an encrypted packet should add nothing");
        return;
    }

    if (mesh_message_ingest(NULL, &position, 2U) != -EINVAL ||
        mesh_message_ingest(&log, NULL, 2U) != -EINVAL) {
        record_failure(test_name, "NULL arguments should be rejected");
        return;
    }

    record_success(test_name);
}

static void test_message_ingest_echo_is_not_duplicated(void) {
    const char *test_name = "message_ingest_echo_is_not_duplicated";
    struct mesh_message_log log;
    mesh_message_log_reset(&log);

    const uint32_t my_node = 0xAABBCCDDU;

    /* What the transport records locally when it queues a send. */
    struct mesh_message sent;
    memset(&sent, 0, sizeof(sent));
    sent.packet_id = 99U;
    sent.from = my_node;
    sent.to = 0x1234U;
    sent.direction = MESH_MESSAGE_OUTBOUND;
    sent.ack = MESH_MESSAGE_ACK_PENDING;
    snprintf(sent.text, sizeof(sent.text), "ping");
    mesh_message_log_append(&log, &sent);

    /* The radio echoes the same packet back to us over FromRadio. */
    meshtastic_MeshPacket echo = make_decoded_packet(
        my_node, 0x1234U, 0U, 99U, meshtastic_PortNum_TEXT_MESSAGE_APP, "ping", 4U);
    echo.has_rx_time = true;
    echo.rx_time = 4242U;

    if (mesh_message_ingest(&log, &echo, my_node) != 0) {
        record_failure(test_name, "an echo should not be reported as a new message");
        return;
    }

    if (log.count != 1U) {
        record_failure(test_name, "the echo should not create a second entry");
        return;
    }

    const struct mesh_message *message = mesh_message_log_at(&log, 0U);
    if (message == NULL || message->rx_time != 4242U) {
        record_failure(test_name, "the echo should refresh the existing entry");
        return;
    }

    if (message->ack != MESH_MESSAGE_ACK_PENDING) {
        record_failure(test_name, "an echo is not a delivery confirmation");
        return;
    }

    record_success(test_name);
}

/* Wraps a Routing reply the way the firmware does: a ROUTING_APP Data whose request_id names
   the message being answered. */
static meshtastic_MeshPacket make_routing_reply(uint32_t request_id,
                                                meshtastic_Routing_Error error) {
    meshtastic_Routing routing = meshtastic_Routing_init_default;
    routing.which_variant = meshtastic_Routing_error_reason_tag;
    routing.error_reason = error;

    uint8_t payload[64];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    (void)pb_encode(&stream, meshtastic_Routing_fields, &routing);

    meshtastic_MeshPacket packet = make_decoded_packet(
        1U, 2U, 0U, 500U, meshtastic_PortNum_ROUTING_APP, payload, stream.bytes_written);
    packet.decoded.request_id = request_id;
    return packet;
}

static void test_message_routing_ack(void) {
    const char *test_name = "message_routing_ack";
    struct mesh_message_log log;
    mesh_message_log_reset(&log);

    struct mesh_message sent;
    memset(&sent, 0, sizeof(sent));
    sent.packet_id = 321U;
    sent.direction = MESH_MESSAGE_OUTBOUND;
    sent.ack = MESH_MESSAGE_ACK_PENDING;
    snprintf(sent.text, sizeof(sent.text), "are you there");
    mesh_message_log_append(&log, &sent);

    meshtastic_MeshPacket ack = make_routing_reply(321U, meshtastic_Routing_Error_NONE);
    if (mesh_message_ingest(&log, &ack, 2U) != 0) {
        record_failure(test_name, "a routing reply adds no message of its own");
        return;
    }

    const struct mesh_message *message = mesh_message_log_at(&log, 0U);
    if (message == NULL || message->ack != MESH_MESSAGE_ACK_DELIVERED) {
        record_failure(test_name, "a NONE error reason means delivered");
        return;
    }

    /* And a failure reason marks the same message failed, carrying the reason through. */
    struct mesh_message second;
    memset(&second, 0, sizeof(second));
    second.packet_id = 322U;
    second.direction = MESH_MESSAGE_OUTBOUND;
    second.ack = MESH_MESSAGE_ACK_PENDING;
    snprintf(second.text, sizeof(second.text), "still there");
    mesh_message_log_append(&log, &second);

    meshtastic_MeshPacket nak = make_routing_reply(322U, meshtastic_Routing_Error_NO_RESPONSE);
    if (mesh_message_ingest(&log, &nak, 2U) != 0) {
        record_failure(test_name, "a routing failure adds no message of its own");
        return;
    }

    const struct mesh_message *failed = mesh_message_log_at(&log, 1U);
    if (failed == NULL || failed->ack != MESH_MESSAGE_ACK_FAILED ||
        failed->ack_error != (uint8_t)meshtastic_Routing_Error_NO_RESPONSE) {
        record_failure(test_name, "an error reason should mark the message failed");
        return;
    }

    /* A reply naming a message we never sent must not disturb anything. */
    meshtastic_MeshPacket stray = make_routing_reply(9999U, meshtastic_Routing_Error_NONE);
    if (mesh_message_ingest(&log, &stray, 2U) != 0 || log.count != 2U) {
        record_failure(test_name, "an unmatched routing reply should be ignored");
        return;
    }

    record_success(test_name);
}

static void test_ui_store_messages(void) {
    const char *test_name = "ui_store_messages";
    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }

    struct mesh_ui_message_list list;
    memset(&list, 0, sizeof(list));
    list.count = 2U;
    list.dropped = 7U;
    list.entries[0].packet_id = 11U;
    list.entries[0].peer = 0x1234U;
    list.entries[0].direction = MESH_MESSAGE_INBOUND;
    snprintf(list.entries[0].peer_name, sizeof(list.entries[0].peer_name), "AB12");
    snprintf(list.entries[0].text, sizeof(list.entries[0].text), "hello there");
    list.entries[1].packet_id = 12U;
    list.entries[1].peer = MESH_MESSAGE_BROADCAST_ADDR;
    list.entries[1].direction = MESH_MESSAGE_OUTBOUND;
    list.entries[1].ack = MESH_MESSAGE_ACK_DELIVERED;
    list.entries[1].broadcast = true;
    snprintf(list.entries[1].peer_name, sizeof(list.entries[1].peer_name), "all");
    /* '=' and a backslash both need escaping in the on-disk format. */
    snprintf(list.entries[1].text, sizeof(list.entries[1].text), "a=b\\c");

    mesh_ui_store_set_messages(&store, &list);

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        record_failure(test_name, "setting messages should raise an update");
        return;
    }
    if ((snapshot.update_flags & MESH_UI_UPDATE_MESSAGES) == 0U) {
        record_failure(test_name, "the messages flag should be set");
        return;
    }
    if (snapshot.messages.count != 2U || snapshot.messages.dropped != 7U) {
        record_failure(test_name, "message list did not reach the snapshot");
        return;
    }

    /* Setting the same list again is not a change and must not wake the UI. */
    mesh_ui_store_set_messages(&store, &list);
    struct mesh_ui_snapshot repeat;
    memset(&repeat, 0, sizeof(repeat));
    if (mesh_ui_store_consume_updates(&store, &repeat)) {
        record_failure(test_name, "an unchanged message list should not raise an update");
        return;
    }

    char cache_path[] = "/tmp/mesh_ui_messagesXXXXXX";
    int fd = mkstemp(cache_path);
    if (fd < 0) {
        record_failure(test_name, "failed to create a temp cache file");
        mesh_ui_store_shutdown(&store);
        return;
    }
    close(fd);

    if (mesh_ui_store_save(&store, cache_path) != 0) {
        record_failure(test_name, "save failed");
        unlink(cache_path);
        mesh_ui_store_shutdown(&store);
        return;
    }

    struct mesh_ui_store loaded;
    if (mesh_ui_store_init(&loaded) != 0) {
        record_failure(test_name, "second store init failed");
        unlink(cache_path);
        mesh_ui_store_shutdown(&store);
        return;
    }

    if (mesh_ui_store_load(&loaded, cache_path) != 0) {
        record_failure(test_name, "load failed");
        unlink(cache_path);
        mesh_ui_store_shutdown(&loaded);
        mesh_ui_store_shutdown(&store);
        return;
    }

    bool ok = (loaded.messages.count == 2U) && (loaded.messages.dropped == 7U) &&
              (strcmp(loaded.messages.entries[0].text, "hello there") == 0) &&
              (strcmp(loaded.messages.entries[0].peer_name, "AB12") == 0) &&
              (loaded.messages.entries[1].packet_id == 12U) &&
              (loaded.messages.entries[1].ack == MESH_MESSAGE_ACK_DELIVERED) &&
              loaded.messages.entries[1].broadcast &&
              (strcmp(loaded.messages.entries[1].text, "a=b\\c") == 0);

    unlink(cache_path);
    mesh_ui_store_shutdown(&loaded);
    mesh_ui_store_shutdown(&store);

    if (!ok) {
        record_failure(test_name, "messages did not survive the save/load roundtrip");
        return;
    }

    record_success(test_name);
}

/*
 * End-to-end through the transport: a scripted FromRadio text packet must reach the message
 * log, an outbound send must leave as a single ToRadio write, and a Routing reply arriving the
 * same way must settle the outbound message's ack state.
 *
 * Uses a single cleanup path rather than repeating the teardown at every check: this test has
 * far more failure points than the others in this file.
 */
static void test_ble_transport_messaging_mock(void) {
    const char *test_name = "ble_transport_messaging_mock";
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:04", .name = "NodeFour", .rssi = -35},
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
    from_radio.packet =
        make_decoded_packet(peer_node, my_node, 0U, 4242U, meshtastic_PortNum_TEXT_MESSAGE_APP,
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
        make_decoded_packet(peer_node, my_node, 0U, 5555U, meshtastic_PortNum_ROUTING_APP,
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

/* Regression for the cache-erasure bug: the transport's log starts empty on every run, so a
   publish that ignored the restored history would blank the store and the next save would
   erase the conversation permanently. */
static void test_ui_message_list_merge(void) {
    const char *test_name = "ui_message_list_merge";

    struct mesh_ui_message_list cached;
    memset(&cached, 0, sizeof(cached));
    cached.count = 2U;
    cached.dropped = 1U;
    cached.entries[0].packet_id = 100U;
    snprintf(cached.entries[0].text, sizeof(cached.entries[0].text), "older");
    cached.entries[1].packet_id = 101U;
    snprintf(cached.entries[1].text, sizeof(cached.entries[1].text), "newer");

    struct mesh_ui_message_list live;
    memset(&live, 0, sizeof(live));

    /* An empty live list must leave the history intact - this is the actual bug. */
    struct mesh_ui_message_list merged;
    mesh_ui_message_list_merge(&cached, &live, &merged);
    if (merged.count != 2U || strcmp(merged.entries[0].text, "older") != 0 ||
        strcmp(merged.entries[1].text, "newer") != 0) {
        record_failure(test_name, "an empty live list should preserve cached history");
        return;
    }
    if (merged.dropped != 1U) {
        record_failure(test_name, "the cached dropped count should carry through");
        return;
    }

    /* Live messages append after the history, newest last. */
    live.count = 1U;
    live.entries[0].packet_id = 200U;
    snprintf(live.entries[0].text, sizeof(live.entries[0].text), "live");
    mesh_ui_message_list_merge(&cached, &live, &merged);
    if (merged.count != 3U || strcmp(merged.entries[0].text, "older") != 0 ||
        strcmp(merged.entries[2].text, "live") != 0) {
        record_failure(test_name, "live messages should append after cached history");
        return;
    }

    /* A message re-received after a restart must not appear twice. */
    live.entries[0].packet_id = 101U;
    snprintf(live.entries[0].text, sizeof(live.entries[0].text), "newer");
    mesh_ui_message_list_merge(&cached, &live, &merged);
    if (merged.count != 2U || strcmp(merged.entries[0].text, "older") != 0 ||
        merged.entries[1].packet_id != 101U) {
        record_failure(test_name, "a cached entry re-received live should not be duplicated");
        return;
    }

    /* Packet id 0 means "no id", so it must never be treated as a duplicate key. */
    struct mesh_ui_message_list unidentified;
    memset(&unidentified, 0, sizeof(unidentified));
    unidentified.count = 1U;
    snprintf(unidentified.entries[0].text, sizeof(unidentified.entries[0].text), "no id");
    struct mesh_ui_message_list zero_live;
    memset(&zero_live, 0, sizeof(zero_live));
    zero_live.count = 1U;
    snprintf(zero_live.entries[0].text, sizeof(zero_live.entries[0].text), "also no id");
    mesh_ui_message_list_merge(&unidentified, &zero_live, &merged);
    if (merged.count != 2U) {
        record_failure(test_name, "packet id 0 should not collapse distinct messages");
        return;
    }

    /* When live traffic fills every slot, the oldest history is dropped and counted. */
    struct mesh_ui_message_list full_live;
    memset(&full_live, 0, sizeof(full_live));
    full_live.count = MESH_UI_MAX_MESSAGES;
    for (uint32_t i = 0; i < MESH_UI_MAX_MESSAGES; ++i) {
        full_live.entries[i].packet_id = 1000U + i;
    }
    mesh_ui_message_list_merge(&cached, &full_live, &merged);
    if (merged.count != MESH_UI_MAX_MESSAGES || merged.entries[0].packet_id != 1000U) {
        record_failure(test_name, "live messages should win every slot when the list is full");
        return;
    }
    if (merged.dropped != 1U + 2U) {
        record_failure(test_name, "squeezed-out history should be added to the dropped count");
        return;
    }

    record_success(test_name);
}

/* Message text reaches --status --json, and JSON must be valid UTF-8, so malformed sequences
   from the radio have to be replaced rather than copied through. */
static void test_message_ingest_invalid_utf8(void) {
    const char *test_name = "message_ingest_invalid_utf8";

    struct {
        const char *label;
        const char *payload;
        size_t payload_len;
        const char *expected;
    } cases[] = {
        /* Well-formed multi-byte characters survive intact. */
        {"two-byte", "caf\xC3\xA9", 5U, "caf\xC3\xA9"},
        {"three-byte", "\xE2\x82\xAC", 3U, "\xE2\x82\xAC"},
        {"four-byte", "\xF0\x9F\x93\xA1", 4U, "\xF0\x9F\x93\xA1"},
        /* Bytes that can never lead a sequence. */
        /* Split so the hex escape does not swallow the trailing 'b' as another hex digit. */
        {"invalid-lead",
         "a\xFF"
         "b",
         3U, "a?b"},
        {"stray-continuation", "a\x80\x62", 3U, "a?b"},
        /* Truncated: a valid lead with the continuation bytes missing. */
        {"truncated", "a\xE2\x82", 3U, "a??"},
        /* Overlong encoding of '/' - the classic path-traversal smuggling trick. */
        {"overlong", "\xC0\xAF", 2U, "??"},
        /* UTF-16 surrogate half, not a valid character in UTF-8. */
        {"surrogate", "\xED\xA0\x80", 3U, "???"},
        /* Above U+10FFFF. */
        {"out-of-range", "\xF4\x90\x80\x80", 4U, "????"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct mesh_message_log log;
        mesh_message_log_reset(&log);

        meshtastic_MeshPacket packet =
            make_decoded_packet(1U, 2U, 0U, (uint32_t)(i + 1U), meshtastic_PortNum_TEXT_MESSAGE_APP,
                                cases[i].payload, cases[i].payload_len);

        if (mesh_message_ingest(&log, &packet, 2U) != 1) {
            record_failure(test_name, cases[i].label);
            return;
        }

        const struct mesh_message *message = mesh_message_log_at(&log, 0U);
        if (message == NULL || strcmp(message->text, cases[i].expected) != 0) {
            record_failure(test_name, cases[i].label);
            return;
        }
    }

    record_success(test_name);
}

static const struct test_case k_test_cases[] = {
    {"config_defaults", "unit", test_config_defaults},
    {"transport_registry_registration", "unit", test_transport_registry_registration},
    {"event_loop_init_shutdown", "unit", test_event_loop_init_shutdown},
    {"ble_transport_status_transitions", "unit", test_ble_transport_status_transitions},
    {"ble_transport_discovery_mock", "unit", test_ble_transport_discovery_mock},
    {"ble_transport_connect_mock", "unit", test_ble_transport_connect_mock},
    {"ui_store_basic", "unit", test_ui_store_basic},
    {"ui_store_persistence", "unit", test_ui_store_persistence},
    {"ui_store_refresh_request", "unit", test_ui_store_refresh_request},
    {"ui_input_quit_keys", "unit", test_ui_input_quit_keys},
    {"ui_cli_transport_update", "unit", test_ui_cli_transport_update},
    {"ui_controller_dispatch", "unit", test_ui_controller_dispatch},
    {"ui_preferences_roundtrip", "unit", test_ui_preferences_roundtrip},
    {"minui_format_menu", "unit", test_minui_format_menu},
    {"proto_varint_roundtrip", "unit", test_proto_varint_roundtrip},
    {"proto_frame_encode_decode", "unit", test_proto_frame_encode_decode},
    {"message_encode_text_golden", "unit", test_message_encode_text_golden},
    {"message_encode_text_roundtrip", "unit", test_message_encode_text_roundtrip},
    {"message_encode_text_limits", "unit", test_message_encode_text_limits},
    {"message_log_ring", "unit", test_message_log_ring},
    {"message_ingest_text", "unit", test_message_ingest_text},
    {"message_ingest_ignores_other_payloads", "unit", test_message_ingest_ignores_other_payloads},
    {"message_ingest_echo_is_not_duplicated", "unit", test_message_ingest_echo_is_not_duplicated},
    {"message_routing_ack", "unit", test_message_routing_ack},
    {"ui_store_messages", "unit", test_ui_store_messages},
    {"ble_transport_messaging_mock", "unit", test_ble_transport_messaging_mock},
    {"ui_message_list_merge", "unit", test_ui_message_list_merge},
    {"message_ingest_invalid_utf8", "unit", test_message_ingest_invalid_utf8},
};

struct test_options {
    const char *category;
    const char *name_filter;
    bool list_only;
};

static bool string_matches_filter(const char *value, const char *filter) {
    if (filter == NULL || filter[0] == '\0') {
        return true;
    }
    if (value == NULL) {
        return false;
    }
    return strstr(value, filter) != NULL;
}

static void print_available_tests(void) {
    const size_t count = sizeof(k_test_cases) / sizeof(k_test_cases[0]);
    printf("Available tests (%zu):\n", count);
    for (size_t i = 0; i < count; ++i) {
        printf("  - %-32s [%s]\n", k_test_cases[i].name, k_test_cases[i].category);
    }
}

static void select_tests(const struct test_options *options, size_t *selected, bool *ran_any) {
    const size_t count = sizeof(k_test_cases) / sizeof(k_test_cases[0]);
    size_t executed = 0U;
    size_t registered = 0U;

    for (size_t i = 0; i < count; ++i) {
        const struct test_case *test = &k_test_cases[i];
        if (options->category != NULL && options->category[0] != '\0' &&
            strcmp(options->category, test->category) != 0) {
            continue;
        }
        if (!string_matches_filter(test->name, options->name_filter)) {
            continue;
        }

        ++registered;
        if (options->list_only) {
            printf("%-32s [%s]\n", test->name, test->category);
            continue;
        }

        fprintf(stderr, "[RUN] %s (%s)\n", test->name, test->category);
        test->fn();
        ++executed;
    }

    if (selected != NULL) {
        *selected = registered;
    }
    if (ran_any != NULL) {
        *ran_any = (executed > 0U);
    }
}

static void print_summary(size_t selected, bool ran_any) {
    if (selected == 0U) {
        printf("No tests matched the provided filters.\n");
        return;
    }

    if (!ran_any) {
        return;
    }

    const size_t passed = g_successes;
    const size_t failed = (size_t)g_failures;
    const size_t total = passed + failed;

    printf("Summary: %zu executed (%zu passed, %zu failed)\n", total, passed, failed);
}

static void print_usage(void) {
    printf("Usage: meshclient_core_tests [options]\n");
    printf("Options:\n");
    printf("  --category NAME   Run only tests in category NAME (e.g. unit, integration).\n");
    printf("  --filter SUBSTR   Run tests whose name contains SUBSTR.\n");
    printf("  --list            List matching tests without executing them.\n");
    printf("  --help            Show this help message.\n");
}

int main(int argc, char **argv) {
    struct test_options options = {
        .category = NULL,
        .name_filter = NULL,
        .list_only = false,
    };

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--category") == 0) {
            if (i + 1 < argc) {
                options.category = argv[++i];
            } else {
                fprintf(stderr, "--category requires an argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--filter") == 0) {
            if (i + 1 < argc) {
                options.name_filter = argv[++i];
            } else {
                fprintf(stderr, "--filter requires an argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--list") == 0) {
            options.list_only = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage();
            print_available_tests();
            return 0;
        } else if (strcmp(argv[i], "--all") == 0) {
            options.category = NULL;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage();
            return 1;
        }
    }

    if (options.list_only) {
        select_tests(&options, NULL, NULL);
        return 0;
    }

    size_t selected = 0U;
    bool ran_any = false;
    select_tests(&options, &selected, &ran_any);
    print_summary(selected, ran_any);

    if (g_failures > 0) {
        fprintf(stderr, "Tests failed: %d\n", g_failures);
        return 1;
    }

    if (!ran_any) {
        printf("No tests were executed.\n");
    }

    return 0;
}
