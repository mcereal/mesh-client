#include "mesh/config.h"
#include "mesh/event_loop.h"
#include "mesh/proto/framing.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/transport.h"
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

static int g_failures = 0;

static void record_failure(const char *test_name, const char *message) {
    fprintf(stderr, "[FAIL] %s: %s\n", test_name, message);
    ++g_failures;
}

static void record_success(const char *test_name) {
    (void)test_name;
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

    struct mesh_ui_handshake_state handshake = {
        .request_in_flight = true,
        .config_complete = false,
        .node_count = 5U,
    };
    snprintf(handshake.primary_channel, sizeof(handshake.primary_channel), "%s", "LongRange");
    snprintf(handshake.my_short_name, sizeof(handshake.my_short_name), "%s", "ME");
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

    struct mesh_ui_handshake_state handshake = {
        .config_complete = true,
        .request_in_flight = false,
        .node_count = 2U,
    };
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

int main(void) {
    test_config_defaults();
    test_transport_registry_registration();
    test_event_loop_init_shutdown();
    test_ble_transport_status_transitions();
    test_ble_transport_discovery_mock();
    test_ble_transport_connect_mock();
    test_ui_store_basic();
    test_ui_controller_dispatch();
    test_ui_preferences_roundtrip();
    test_proto_varint_roundtrip();
    test_proto_frame_encode_decode();

    if (g_failures > 0) {
        fprintf(stderr, "Tests failed: %d\n", g_failures);
        return 1;
    }

    printf("All tests passed\n");
    return 0;
}
