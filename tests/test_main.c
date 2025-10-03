#define _POSIX_C_SOURCE 200809L

#include "mesh/config.h"
#include "mesh/event_loop.h"
#include "mesh/proto/framing.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/transport.h"
#include "mesh/ui/backends/minui.h"
#include "mesh/ui/backends/stub.h"
#include "mesh/ui/controller.h"
#include "mesh/ui/store.h"
#include "mesh/ui/preferences.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/mesh.pb.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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

static bool string_matches_any(const char *value, const char *const options[], size_t option_count) {
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
    const char *expected_states[] = {"running", "waiting-for-bluez", "waiting-for-adapter", "inactive"};
    if (!string_matches_any(running_status, expected_states, sizeof(expected_states) / sizeof(expected_states[0]))) {
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
    size_t count = mesh_ble_transport_get_devices(ble, discovered, sizeof(discovered) / sizeof(discovered[0]));
    if (count != mock_config.device_count) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "unexpected discovered device count");
        return;
    }

    if (strcmp(discovered[0].name, "NodeOne") != 0 || strcmp(discovered[1].address, "AA:BB:CC:DD:EE:02") != 0) {
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
        .rx_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_03/service0017/char0025",
        .tx_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_03/service0017/char0026",
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

    if (strcmp(write_path, mock_config.rx_char_path) != 0) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "want_config write path mismatch");
        return;
    }

    size_t header_len = 0;
    size_t payload_len = 0;
    if (mesh_proto_frame_decode(write_capture, write_len, &header_len, &payload_len) != 0) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "failed to decode want_config frame");
        return;
    }

    const uint8_t *to_radio_payload = write_capture + header_len;
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t to_radio_stream = pb_istream_from_buffer(to_radio_payload, payload_len);
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

    uint8_t from_payload[256];
    uint8_t from_frame[320];

    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    from_radio.my_info.my_node_num = 0x01020304U;
    from_radio.my_info.nodedb_count = 2U;

    pb_ostream_t encode_stream = pb_ostream_from_buffer(from_payload, sizeof(from_payload));
    if (!pb_encode(&encode_stream, meshtastic_FromRadio_fields, &from_radio)) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "failed to encode my_info");
        return;
    }

    size_t from_payload_len = encode_stream.bytes_written;
    size_t from_frame_len = 0U;
    if (mesh_proto_frame_encode(from_payload, from_payload_len, from_frame, sizeof(from_frame), &from_frame_len) != 0) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "failed to frame my_info");
        return;
    }

    mesh_bluez_client_mock_emit_notification(mock_config.tx_char_path, from_frame, 2U);
    mesh_bluez_client_mock_emit_notification(mock_config.tx_char_path, from_frame + 2U, from_frame_len - 2U);

    handshake = mesh_ble_transport_handshake_status(ble);
    if (!handshake.has_my_info || handshake.my_info.my_node_num != from_radio.my_info.my_node_num) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "my_info not cached");
        return;
    }

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    from_radio.node_info.num = 0x01020305U;
    from_radio.node_info.has_user = true;
    snprintf(from_radio.node_info.user.long_name, sizeof(from_radio.node_info.user.long_name), "%s", "Alice Example");
    snprintf(from_radio.node_info.user.short_name, sizeof(from_radio.node_info.user.short_name), "%s", "AE");
    from_radio.node_info.last_heard = 1234U;
    from_radio.node_info.snr = 12.5f;
    from_radio.node_info.via_mqtt = true;
    from_radio.node_info.has_hops_away = true;
    from_radio.node_info.hops_away = 2U;

    encode_stream = pb_ostream_from_buffer(from_payload, sizeof(from_payload));
    if (!pb_encode(&encode_stream, meshtastic_FromRadio_fields, &from_radio)) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "failed to encode node_info");
        return;
    }

    from_payload_len = encode_stream.bytes_written;
    if (mesh_proto_frame_encode(from_payload, from_payload_len, from_frame, sizeof(from_frame), &from_frame_len) != 0) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "failed to frame node_info");
        return;
    }

    mesh_bluez_client_mock_emit_notification(mock_config.tx_char_path, from_frame, from_frame_len);

    handshake = mesh_ble_transport_handshake_status(ble);
    if (handshake.node_count != 1U || handshake.nodes[0].node_id != from_radio.node_info.num) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "node info cache incorrect");
        return;
    }

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
    from_radio.config_complete_id = to_radio.want_config_id;

    encode_stream = pb_ostream_from_buffer(from_payload, sizeof(from_payload));
    if (!pb_encode(&encode_stream, meshtastic_FromRadio_fields, &from_radio)) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "failed to encode config_complete");
        return;
    }

    from_payload_len = encode_stream.bytes_written;
    if (mesh_proto_frame_encode(from_payload, from_payload_len, from_frame, sizeof(from_frame), &from_frame_len) != 0) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "failed to frame config_complete");
        return;
    }

    mesh_bluez_client_mock_emit_notification(mock_config.tx_char_path, from_frame, from_frame_len);

    handshake = mesh_ble_transport_handshake_status(ble);
    if (handshake.request_in_flight || !handshake.config_complete ||
        handshake.config_complete_id != to_radio.want_config_id) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "config handshake did not complete");
        return;
    }

    struct mesh_ble_transport_stats stats = mesh_ble_transport_stats(ble);
    if (stats.frames_received != 3U) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "unexpected frame count after handshake");
        return;
    }

    size_t handshake_write_calls = write_call_count;
    uint8_t outbound_frame[64];
    for (size_t i = 0; i < sizeof(outbound_frame); ++i) {
        outbound_frame[i] = (uint8_t)i;
    }

    if (mesh_ble_transport_send_frame(ble, outbound_frame, sizeof(outbound_frame)) != 0) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "failed to queue outbound frame");
        return;
    }

    const size_t expected_segments = (sizeof(outbound_frame) + 19U) / 20U;
    size_t produced_segments = write_call_count - handshake_write_calls;
    if (produced_segments != expected_segments) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "unexpected segment count");
        return;
    }

    size_t segment_sum = 0U;
    for (size_t i = 0; i < expected_segments; ++i) {
        size_t length = write_lengths[handshake_write_calls + i];
        if (length == 0U || length > 20U) {
            ble->ops->stop(ble);
            mesh_event_loop_shutdown(&loop);
            mesh_bluez_client_mock_disable();
            record_failure(test_name, "segment size invalid");
            return;
        }
        segment_sum += length;
    }

    if (segment_sum != sizeof(outbound_frame)) {
        ble->ops->stop(ble);
        mesh_event_loop_shutdown(&loop);
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "segment sizes do not sum to payload length");
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
    if (mesh_ui_controller_init(&controller, &store, mesh_ui_backend_stub(), &context, &loop) != 0) {
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
    snprintf(snapshot.devices[0].identifier, sizeof snapshot.devices[0].identifier, "%s", "AA:BB:CC:DD:EE:01");
    snprintf(snapshot.devices[0].name, sizeof snapshot.devices[0].name, "%s", "NodeOne");
    snapshot.devices[0].rssi = -42;
    snapshot.devices[0].connected = true;

    snprintf(snapshot.devices[1].identifier, sizeof snapshot.devices[1].identifier, "%s", "AA:BB:CC:DD:EE:02");
    snprintf(snapshot.devices[1].name, sizeof snapshot.devices[1].name, "%s", "NodeTwo");
    snapshot.devices[1].rssi = -60;
    snapshot.devices[1].connected = false;

    snapshot.handshake_valid = true;
    snapshot.handshake.request_in_flight = false;
    snapshot.handshake.request_id = 5U;
    snapshot.handshake.config_complete = true;
    snapshot.handshake.config_complete_id = 5U;
    snapshot.handshake.node_count = 2U;
    snprintf(snapshot.handshake.my_short_name, sizeof snapshot.handshake.my_short_name, "%s", "ABCD");
    snapshot.handshake.nodes[0].node_id = 101U;
    snprintf(snapshot.handshake.nodes[0].long_name, sizeof snapshot.handshake.nodes[0].long_name, "%s",
             "BaseStation");
    snprintf(snapshot.handshake.nodes[0].short_name, sizeof snapshot.handshake.nodes[0].short_name, "%s",
             "BASE");
    snapshot.handshake.nodes[0].snr = 8.5f;
    snapshot.handshake.nodes[1].node_id = 202U;
    snprintf(snapshot.handshake.nodes[1].long_name, sizeof snapshot.handshake.nodes[1].long_name, "%s",
             "FieldUnit");
    snprintf(snapshot.handshake.nodes[1].short_name, sizeof snapshot.handshake.nodes[1].short_name, "%s",
             "FILD");
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
        {0U, 1U},
        {1U, 1U},
        {127U, 1U},
        {128U, 2U},
        {16384U, 3U},
        {268435455U, 4U},
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

struct test_case {
    const char *name;
    const char *category;
    void (*fn)(void);
};

static const struct test_case k_test_cases[] = {
    {"config_defaults", "unit", test_config_defaults},
    {"transport_registry_registration", "unit", test_transport_registry_registration},
    {"event_loop_init_shutdown", "unit", test_event_loop_init_shutdown},
    {"ble_transport_status_transitions", "unit", test_ble_transport_status_transitions},
    {"ble_transport_discovery_mock", "unit", test_ble_transport_discovery_mock},
    {"ble_transport_connect_mock", "unit", test_ble_transport_connect_mock},
    {"ui_store_basic", "unit", test_ui_store_basic},
    {"ui_store_persistence", "unit", test_ui_store_persistence},
    {"ui_controller_dispatch", "unit", test_ui_controller_dispatch},
    {"ui_preferences_roundtrip", "unit", test_ui_preferences_roundtrip},
    {"minui_format_menu", "unit", test_minui_format_menu},
    {"proto_varint_roundtrip", "unit", test_proto_varint_roundtrip},
    {"proto_frame_encode_decode", "unit", test_proto_frame_encode_decode},
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
