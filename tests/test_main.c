#define _POSIX_C_SOURCE 200809L

#include "mesh/app.h"
#include "mesh/config.h"
#include "mesh/event_loop.h"
#include "mesh/mesh_message.h"
#include "mesh/proto/framing.h"
#include "mesh/proto/stream_framing.h"
#include "mesh/radio_settings.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/serial.h"
#include "mesh/transport/serial_usb.h"
#include "mesh/transport/transport.h"
#include "mesh/ui/backends/cli.h"
#include "mesh/ui/backends/minui.h"
#include "mesh/ui/backends/stub.h"
#include "mesh/ui/controller.h"
#include "mesh/ui/input.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/preferences.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/channel.pb.h"
#include "meshtastic/mesh.pb.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
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

static void test_sleep_ms(unsigned ms) {
    struct timespec ts = {.tv_sec = ms / 1000U, .tv_nsec = (long)(ms % 1000U) * 1000000L};
    nanosleep(&ts, NULL);
}

/* Mirrors what a real node does on a fresh BlueZ cache: Connect returns, but the GATT
   characteristics only appear a few polls later. The connect must stay pending, not fail. */
static void test_ble_transport_connect_deferred_services(void) {
    const char *test_name = "ble_transport_connect_deferred_services";
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:05", .name = "NodeFive", .rssi = -50},
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
static void test_ble_transport_connect_async_reply(void) {
    const char *test_name = "ble_transport_connect_async_reply";
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:08", .name = "NodeEight", .rssi = -50},
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

/* The foreground policy: a saved preferred node wins even when a stronger one is in range;
   with nothing saved, the strongest advertiser is used. */
static void test_app_autoconnect_policy(void) {
    const char *test_name = "app_autoconnect_policy";
    const char *failure = NULL;

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:06", .name = "NodeSix", .rssi = -30},
        {.address = "AA:BB:CC:DD:EE:07", .name = "NodeSeven", .rssi = -70},
    };

    uint8_t write_capture[64];
    size_t write_len = 0U;
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 2U,
        .write_capture_buffer = write_capture,
        .write_capture_capacity = sizeof(write_capture),
        .write_capture_length = &write_len,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    /* Keep the app's preference files out of the real $HOME. */
    char home_dir[] = "/tmp/mesh_app_autoconnectXXXXXX";
    if (mkdtemp(home_dir) == NULL) {
        mesh_bluez_client_mock_disable();
        record_failure(test_name, "mkdtemp failed");
        return;
    }
    setenv("HOME", home_dir, 1);
    setenv("MESHCLIENT_UI_BACKEND", "stub", 1);
    unsetenv("MESHCLIENT_AUTOCONNECT");

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    /* This test is about BLE ranking; a USB port on the build host now outranks every
       advertiser, so keep the serial link out of it. */
    config.enable_serial = false;
    snprintf(config.preferred_ble_device, sizeof config.preferred_ble_device, "%s", "NodeSeven");

    struct mesh_app app;
    memset(&app, 0, sizeof app);
    bool app_ready = false;
    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *ble = mesh_ble_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);

    mesh_app_autoconnect(&app);
    const char *connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[1].address) != 0) {
        failure = "preferred node (by name) should win over a stronger one";
        goto cleanup;
    }

    /* Drop the link and the preference: the strongest node should be chosen next. */
    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "disconnect failed";
        goto cleanup;
    }
    app.config.preferred_ble_device[0] = '\0';
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[0].address) != 0) {
        failure = "strongest node should be chosen without a preference";
        goto cleanup;
    }

    /* Already connected: another turn must be a no-op rather than a reconnect. */
    write_len = 0U;
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (write_len != 0U) {
        failure = "autoconnect should not act while connected";
        goto cleanup;
    }

    /* Not in foreground mode it must never connect. */
    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "second disconnect failed";
        goto cleanup;
    }
    app.config.run_mode = MESH_APP_RUN_SINGLE_POLL;
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "single-poll mode must not auto-connect";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    {
        char path[256];
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs.handshake", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient/ui_prefs", home_dir);
        unlink(path);
        snprintf(path, sizeof path, "%s/.meshclient", home_dir);
        rmdir(path);
        rmdir(home_dir);
    }
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
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
        strcmp(loaded.preferred_channel, prefs.preferred_channel) != 0 ||
        loaded.preferred_device_kind != prefs.preferred_device_kind) {
        unlink(prefab_path);
        record_failure(test_name, "roundtrip mismatch");
        return;
    }

    /* A USB port roundtrips as serial, so the reconnect goes to the right link. */
    snprintf(prefs.preferred_device, sizeof prefs.preferred_device, "%s", "/dev/ttyUSB0");
    prefs.preferred_device_kind = (uint8_t)MESH_UI_DEVICE_SERIAL;
    if (mesh_ui_preferences_save(&prefs, prefab_path) != 0 ||
        mesh_ui_preferences_load(&loaded, prefab_path) != 0 ||
        loaded.preferred_device_kind != (uint8_t)MESH_UI_DEVICE_SERIAL ||
        strcmp(loaded.preferred_device, "/dev/ttyUSB0") != 0) {
        unlink(prefab_path);
        record_failure(test_name, "serial preference did not roundtrip");
        return;
    }

    /* A file from before the kind was recorded: a tty path must not be handed to BLE. */
    temp = fopen(prefab_path, "w");
    if (temp == NULL) {
        unlink(prefab_path);
        record_failure(test_name, "failed to rewrite temp file");
        return;
    }
    fprintf(temp, "preferred_device=/dev/ttyUSB0\npreferred_channel=LongFast\n");
    fclose(temp);
    memset(&loaded, 0, sizeof loaded);
    if (mesh_ui_preferences_load(&loaded, prefab_path) != 0 ||
        loaded.preferred_device_kind != (uint8_t)MESH_UI_DEVICE_SERIAL) {
        unlink(prefab_path);
        record_failure(test_name, "a legacy tty preference should migrate to serial");
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

/* Builds a store with three devices (one connected), a synced handshake with three nodes (the
   first is us) and two messages: a broadcast from ALFA and a direct message from BRVO. */
static void test_nav_populate(struct mesh_ui_store *store) {
    struct mesh_ui_device devices[3] = {
        {.identifier = "AA:BB:CC:DD:EE:01", .name = "NodeOne", .rssi = -45, .connected = true},
        {.identifier = "AA:BB:CC:DD:EE:02", .name = "NodeTwo", .rssi = -60, .connected = false},
        {.identifier = "AA:BB:CC:DD:EE:03", .name = "NodeThree", .rssi = -70, .connected = false},
    };
    mesh_ui_store_set_discovery(store, devices, 3U);

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.config_complete = true;
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 3U;
    handshake.nodes[0].node_id = 0x1000U;
    snprintf(handshake.nodes[0].short_name, sizeof handshake.nodes[0].short_name, "%s", "ME");
    handshake.nodes[1].node_id = 0x2000U;
    snprintf(handshake.nodes[1].short_name, sizeof handshake.nodes[1].short_name, "%s", "ALFA");
    snprintf(handshake.nodes[1].long_name, sizeof handshake.nodes[1].long_name, "%s", "Alfa Node");
    handshake.nodes[2].node_id = 0x3000U;
    snprintf(handshake.nodes[2].short_name, sizeof handshake.nodes[2].short_name, "%s", "BRVO");
    mesh_ui_store_set_handshake(store, &handshake);

    struct mesh_ui_message_list messages;
    memset(&messages, 0, sizeof messages);
    messages.count = 2U;
    messages.entries[0].packet_id = 11U;
    messages.entries[0].peer = 0x2000U;
    messages.entries[0].broadcast = true;
    messages.entries[0].direction = MESH_MESSAGE_INBOUND;
    snprintf(messages.entries[0].peer_name, sizeof messages.entries[0].peer_name, "%s", "ALFA");
    snprintf(messages.entries[0].text, sizeof messages.entries[0].text, "%s", "hello all");
    messages.entries[1].packet_id = 12U;
    messages.entries[1].peer = 0x3000U;
    messages.entries[1].broadcast = false;
    messages.entries[1].direction = MESH_MESSAGE_INBOUND;
    snprintf(messages.entries[1].peer_name, sizeof messages.entries[1].peer_name, "%s", "BRVO");
    snprintf(messages.entries[1].text, sizeof messages.entries[1].text, "%s", "just you");
    mesh_ui_store_set_messages(store, &messages);
}

static void test_ui_nav_navigation(void) {
    const char *test_name = "ui_nav_navigation";
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    test_nav_populate(&store);

    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action action;

    /* First frame: Messages tab showing the inbox, cursor parked on the newest message. */
    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        failure = "expected initial snapshot";
        goto cleanup;
    }
    if (snapshot.nav.screen != MESH_UI_SCREEN_MESSAGES || !snapshot.nav.inbox ||
        snapshot.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U ||
        snapshot.nav.target_node != MESH_MESSAGE_BROADCAST_ADDR ||
        strcmp(snapshot.nav.target_name, "#Primary") != 0) {
        failure = "initial nav state wrong";
        goto cleanup;
    }

    /* A on the direct message replies to its sender and lands on the first canned reply. */
    if (!mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action) ||
        action.type != MESH_UI_ACTION_NONE) {
        failure = "reply should change screen without an action";
        goto cleanup;
    }
    if (store.nav.screen != MESH_UI_SCREEN_COMPOSE || store.nav.target_node != 0x3000U ||
        strcmp(store.nav.target_name, "BRVO") != 0 || store.nav.inbox ||
        store.nav.cursor[MESH_UI_SCREEN_COMPOSE] != MESH_UI_COMPOSE_FIRST_CANNED) {
        failure = "reply target not taken from the message";
        goto cleanup;
    }
    if (!mesh_ui_store_consume_updates(&store, &snapshot) ||
        (snapshot.update_flags & MESH_UI_UPDATE_NAV) == 0U) {
        failure = "nav change must signal the store";
        goto cleanup;
    }

    /* A on a canned row asks the app to send it to the target and shows that conversation. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_SEND_TEXT || action.dest != 0x3000U || action.channel != 0U ||
        strcmp(action.text, mesh_ui_canned_text(0)) != 0 ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "send action not produced";
        goto cleanup;
    }
    /* Only the one direct message with BRVO is in view now. */
    if (mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_MESSAGES) != 1U) {
        failure = "conversation view should hold only BRVO's messages";
        goto cleanup;
    }

    /* Y reopens Compose; A on the To: row opens the picker with the cursor on the current
       target (BRVO, after #Primary and ALFA), Up/Down move, A picks, B cancels. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (store.nav.screen != MESH_UI_SCREEN_COMPOSE) {
        failure = "Y should open Compose";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    if (store.nav.cursor[MESH_UI_SCREEN_COMPOSE] != MESH_UI_COMPOSE_ROW_TARGET) {
        failure = "UP twice should reach the To: row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.picker_open || mesh_ui_nav_picker_count(&store) != 3U ||
        store.nav.picker_cursor != 2U) {
        failure = "picker should open on the current target";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);  /* jump to the top */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* LEFT/RIGHT switch tabs? no */
    if (store.nav.screen != MESH_UI_SCREEN_COMPOSE || !store.nav.picker_open ||
        store.nav.picker_cursor != 2U) {
        failure = "LEFT/RIGHT in the picker must page, not switch tabs";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action); /* clamps at the top */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.picker_open || store.nav.target_node != MESH_MESSAGE_BROADCAST_ADDR ||
        store.nav.target_channel != 0U || strcmp(store.nav.target_name, "#Primary") != 0) {
        failure = "picking the first row should target the primary channel";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.target_node != 0x2000U || strcmp(store.nav.target_name, "ALFA") != 0) {
        failure = "second picker row should be the first node other than us";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action); /* cancel keeps ALFA */
    if (store.nav.picker_open || store.nav.target_node != 0x2000U) {
        failure = "B should cancel the picker without changing the target";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* +10 clamps to the last */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.target_node != 0x3000U) {
        failure = "RIGHT should jump to the last row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action); /* no-op at the top */
    if (store.nav.cursor[MESH_UI_SCREEN_COMPOSE] != 0U) {
        failure = "UP at row 0 must not underflow";
        goto cleanup;
    }

    /* B leaves Compose for the conversation; X walks conversations: BRVO -> Inbox ->
       #Primary -> BRVO (the only direct peer) -> Inbox. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES || store.nav.inbox) {
        failure = "B should return to the BRVO conversation";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (!store.nav.inbox) {
        failure = "X after the last direct peer should show the inbox";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (store.nav.inbox || store.nav.target_node != MESH_MESSAGE_BROADCAST_ADDR ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_MESSAGES) != 1U) {
        failure = "X from the inbox should show the channel with its one broadcast";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (store.nav.inbox || store.nav.target_node != 0x3000U) {
        failure = "X after the channels should reach the first direct peer";
        goto cleanup;
    }

    /* Tabs wrap in both directions; L1/R1 mirror Left/Right. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_SETTINGS) {
        failure = "LEFT from the first tab should wrap to the last";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_R1, &action);
    if (store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "R1 from the last tab should wrap to the first";
        goto cleanup;
    }

    /* Nodes tab: A on a node targets it; A on ourselves does nothing. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_NODES) {
        failure = "RIGHT should reach Nodes";
        goto cleanup;
    }
    if (mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action)) {
        failure = "A on our own node should be a no-op";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* clamps at the last row */
    if (store.nav.cursor[MESH_UI_SCREEN_NODES] != 2U) {
        failure = "DOWN must clamp at the last node";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.screen != MESH_UI_SCREEN_COMPOSE || store.nav.target_node != 0x3000U ||
        strcmp(store.nav.target_name, "BRVO") != 0) {
        failure = "A on a node should target it in Compose";
        goto cleanup;
    }

    /* Devices tab: A connects to an unconnected device and does nothing on the connected one. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_DEVICES) {
        failure = "RIGHT from Compose should reach Devices";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "A on the connected device should not reconnect";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_CONNECT ||
        strcmp(action.identifier, "AA:BB:CC:DD:EE:02") != 0) {
        failure = "A on another device should request a connect";
        goto cleanup;
    }

    /* Status has no rows; the cursor must stay at zero and A must be inert. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    if (store.nav.screen != MESH_UI_SCREEN_STATUS ||
        store.nav.cursor[MESH_UI_SCREEN_STATUS] != 0U ||
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action) ||
        action.type != MESH_UI_ACTION_NONE) {
        failure = "Status tab must be inert";
        goto cleanup;
    }

    /* Back in the inbox, a cursor on the newest line follows new traffic; one that was moved
       up stays where it was. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* Settings */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* wraps to Messages */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);     /* BRVO -> inbox */
    if (!store.nav.inbox) {
        failure = "expected the inbox";
        goto cleanup;
    }
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (snapshot.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "switching conversation should park the cursor on the newest line";
        goto cleanup;
    }
    struct mesh_ui_message_list more = store.messages;
    more.entries[more.count] = more.entries[1];
    more.entries[more.count].packet_id = 13U;
    more.count++;
    mesh_ui_store_set_messages(&store, &more);
    if (!mesh_ui_store_consume_updates(&store, &snapshot) ||
        snapshot.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 2U) {
        failure = "cursor at the tail should follow a new message";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    more.entries[more.count] = more.entries[1];
    more.entries[more.count].packet_id = 14U;
    more.count++;
    mesh_ui_store_set_messages(&store, &more);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (snapshot.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 1U) {
        failure = "cursor moved off the tail should hold its place";
        goto cleanup;
    }

    /* Lists shrinking pull the cursor back inside. */
    struct mesh_ui_message_list fewer;
    memset(&fewer, 0, sizeof fewer);
    fewer.count = 1U;
    fewer.entries[0] = more.entries[0];
    mesh_ui_store_set_messages(&store, &fewer);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (snapshot.nav.cursor[MESH_UI_SCREEN_MESSAGES] != 0U ||
        (snapshot.update_flags & MESH_UI_UPDATE_NAV) == 0U) {
        failure = "cursor must be clamped when the list shrinks";
        goto cleanup;
    }

    /* Toasts expire on tick and are dismissed by any key. */
    mesh_ui_store_set_toast(&store, 1000U, "Sent to BRVO");
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (strcmp(snapshot.nav.toast, "Sent to BRVO") != 0) {
        failure = "toast not carried in the snapshot";
        goto cleanup;
    }
    mesh_ui_store_tick(&store, 2000U);
    if (store.nav.toast[0] == '\0') {
        failure = "toast expired too early";
        goto cleanup;
    }
    mesh_ui_store_tick(&store, 6000U);
    if (store.nav.toast[0] != '\0' || !mesh_ui_store_consume_updates(&store, &snapshot)) {
        failure = "toast should expire after a few seconds and repaint";
        goto cleanup;
    }
    mesh_ui_store_set_toast(&store, 7000U, "Connecting");
    if (!mesh_ui_store_handle_key(&store, MESH_UI_KEY_SELECT, &action) ||
        store.nav.toast[0] != '\0') {
        failure = "any key should dismiss a toast";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/* Channel table drives the To: cycle and the conversation filter; the keyboard builds a draft. */
static void test_ui_nav_channels_and_keyboard(void) {
    const char *test_name = "ui_nav_channels_and_keyboard";
    const char *failure = NULL;
    mesh_ui_canned_reset();

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    test_nav_populate(&store);

    /* Add a channel table (primary "LongFast", secondary "Team", slot 2 disabled) and a
       broadcast on the secondary channel. */
    struct mesh_ui_handshake_state handshake = store.handshake;
    handshake.channel_count = 3U;
    handshake.channels[0].index = 0U;
    handshake.channels[0].role = 1U;
    snprintf(handshake.channels[0].name, sizeof handshake.channels[0].name, "%s", "LongFast");
    handshake.channels[1].index = 1U;
    handshake.channels[1].role = 2U;
    snprintf(handshake.channels[1].name, sizeof handshake.channels[1].name, "%s", "Team");
    handshake.channels[2].index = 2U;
    handshake.channels[2].role = 0U;
    mesh_ui_store_set_handshake(&store, &handshake);

    struct mesh_ui_message_list messages = store.messages;
    messages.entries[messages.count] = messages.entries[0];
    messages.entries[messages.count].packet_id = 21U;
    messages.entries[messages.count].channel = 1U;
    snprintf(messages.entries[messages.count].text, sizeof messages.entries[0].text, "%s",
             "team only");
    messages.count++;
    mesh_ui_store_set_messages(&store, &messages);

    struct mesh_ui_snapshot snapshot;
    struct mesh_ui_action action;
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    if (strcmp(snapshot.nav.target_name, "#LongFast") != 0) {
        failure = "target name should pick up the primary channel name";
        goto cleanup;
    }

    /* X: Inbox -> #LongFast (1 broadcast) -> #Team (1 broadcast) -> BRVO -> Inbox. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (store.nav.inbox || store.nav.target_channel != 0U ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_MESSAGES) != 1U) {
        failure = "first X should show the primary channel";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (store.nav.target_channel != 1U || strcmp(store.nav.target_name, "#Team") != 0 ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_MESSAGES) != 1U) {
        failure = "second X should show the secondary channel with its broadcast";
        goto cleanup;
    }
    uint32_t indices[MESH_UI_MAX_MESSAGES];
    if (mesh_ui_nav_filter_messages(&store.nav, &store.messages, indices, MESH_UI_MAX_MESSAGES) !=
            1U ||
        store.messages.entries[indices[0]].packet_id != 21U) {
        failure = "channel filter picked the wrong message";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (!store.nav.inbox) {
        failure = "X should return to the inbox after the direct peers";
        goto cleanup;
    }

    /* The picker lists #LongFast, #Team, ALFA, BRVO (never us, never the disabled slot); a
       canned reply sent while on #Team carries channel 1. */
    if (mesh_ui_nav_picker_count(&store) != 4U) {
        failure = "picker should list two channels and two nodes";
        goto cleanup;
    }
    char row_name[96];
    uint32_t row_node = 0U;
    uint8_t row_channel = 0U;
    if (!mesh_ui_nav_picker_row(&store, 1U, &row_node, &row_channel, row_name, sizeof row_name) ||
        row_node != MESH_MESSAGE_BROADCAST_ADDR || row_channel != 1U ||
        strcmp(row_name, "#Team") != 0) {
        failure = "picker row 1 should be the secondary channel";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);    /* open picker (on BRVO) */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action); /* top */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* #Team */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.picker_open || store.nav.target_node != MESH_MESSAGE_BROADCAST_ADDR ||
        store.nav.target_channel != 1U) {
        failure = "picker should select the secondary channel";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_SEND_TEXT || action.dest != MESH_MESSAGE_BROADCAST_ADDR ||
        action.channel != 1U) {
        failure = "canned send should target the selected channel";
        goto cleanup;
    }

    /* Keyboard: type "Hi", a space, delete it, a space again, START sends "Hi ". */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action); /* draft row */
    if (store.nav.cursor[MESH_UI_SCREEN_COMPOSE] != MESH_UI_COMPOSE_ROW_DRAFT) {
        failure = "expected the draft row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open || store.nav.kb_row != 0U || store.nav.kb_col != 0U) {
        failure = "A on the draft row should open the keyboard at the top-left";
        goto cleanup;
    }
    /* LEFT/RIGHT move within the grid while the keyboard is open, never switch tabs. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.kb_col != MESH_UI_KB_COLS - 1U || store.nav.screen != MESH_UI_SCREEN_COMPOSE) {
        failure = "LEFT should wrap to the last column";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* back to col 0 */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* row 2: asdfghjkl' */
    for (int i = 0; i < 5; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action); /* shift */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* H */
    if (strcmp(store.nav.draft, "H") != 0 || store.nav.kb_layer != MESH_UI_KB_LOWER) {
        failure = "shift should apply to one character";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action); /* row 1: qwertyuiop */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* col 7: i */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action); /* space */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action); /* delete it */
    if (strcmp(store.nav.draft, "Hi") != 0) {
        failure = "typing/deleting produced the wrong draft";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_SEND_TEXT || strcmp(action.text, "Hi ") != 0 ||
        action.channel != 1U || store.nav.keyboard_open || store.nav.draft[0] != '\0' ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES) {
        failure = "START should send the draft and return to the conversation";
        goto cleanup;
    }

    /* The action row: moving down from column 9 lands on the last (cancel) key; the mapping
       comes back to a sensible column. Cancel drops the draft and closes the keyboard. B on
       an empty draft also closes it. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);    /* keyboard open, row 0 col 0 */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);    /* '1' */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action); /* col 9 */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);   /* wraps to the action row */
    if (store.nav.kb_row != MESH_UI_KB_CHAR_ROWS || store.nav.kb_col != MESH_UI_KB_ACTIONS - 1U) {
        failure = "column should map onto the action row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* cancel */
    if (store.nav.keyboard_open || store.nav.draft[0] != '\0' ||
        store.nav.screen != MESH_UI_SCREEN_COMPOSE) {
        failure = "cancel should discard the draft and close the keyboard";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* reopen (cursor still on draft) */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.keyboard_open) {
        failure = "B with an empty draft should close the keyboard";
        goto cleanup;
    }
    if (mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action) == false &&
        action.type != MESH_UI_ACTION_NONE) {
        failure = "unexpected action";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/* The radio's Channel messages land in the handshake status, indexed by slot. */
static void test_ble_transport_channel_decode(void) {
    const char *test_name = "ble_transport_channel_decode";
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:09", .name = "NodeNine", .rssi = -50},
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
    if (!pb_encode(&stream, meshtastic_FromRadio_fields, &from_radio)) {
        record_failure(test_name, "encode channel 1 failed");
        return;
    }
    read_payload_lengths[0] = stream.bytes_written;

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_channel_tag;
    from_radio.channel.index = 0;
    from_radio.channel.role = meshtastic_Channel_Role_PRIMARY;
    from_radio.channel.has_settings = true; /* unnamed: the default primary */
    stream = pb_ostream_from_buffer(read_buffers[1], sizeof read_buffers[1]);
    if (!pb_encode(&stream, meshtastic_FromRadio_fields, &from_radio)) {
        record_failure(test_name, "encode channel 0 failed");
        return;
    }
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

static void test_ui_canned_load(void) {
    const char *test_name = "ui_canned_load";
    const char *failure = NULL;

    char path[] = "/tmp/meshclient-canned-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        record_failure(test_name, "mkstemp failed");
        return;
    }
    const char *content = "# quick replies\n\nAck\n  \nBe there in 5\nbad\x01line\n";
    if (write(fd, content, strlen(content)) < 0) {
        close(fd);
        unlink(path);
        record_failure(test_name, "write failed");
        return;
    }
    close(fd);

    mesh_ui_canned_reset();
    const size_t defaults = mesh_ui_canned_count();
    if (defaults == 0U || strcmp(mesh_ui_canned_text(0), "OK") != 0) {
        failure = "built-in replies missing";
        goto cleanup;
    }

    /* Comments, blank lines and lines with control bytes are skipped; "  " is not blank but
       has no visible text and is kept as-is (the user asked for it). */
    const int loaded = mesh_ui_canned_load(path);
    if (loaded != 3 || mesh_ui_canned_count() != 3U || strcmp(mesh_ui_canned_text(0), "Ack") != 0 ||
        strcmp(mesh_ui_canned_text(2), "Be there in 5") != 0) {
        failure = "canned file not parsed as expected";
        goto cleanup;
    }
    if (mesh_ui_canned_text(3)[0] != '\0') {
        failure = "out-of-range index must yield an empty string";
        goto cleanup;
    }
    if (mesh_ui_canned_load("/nonexistent/canned.txt") != -ENOENT || mesh_ui_canned_count() != 3U) {
        failure = "a missing file must leave the loaded set alone";
        goto cleanup;
    }

cleanup:
    unlink(path);
    mesh_ui_canned_reset();
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

struct test_key_capture {
    enum mesh_ui_key keys[16];
    size_t count;
};

static void test_capture_key(void *userdata, enum mesh_ui_key key) {
    struct test_key_capture *capture = (struct test_key_capture *)userdata;
    if (capture->count < sizeof(capture->keys) / sizeof(capture->keys[0])) {
        capture->keys[capture->count++] = key;
    }
}

static void test_ui_input_key_mapping(void) {
    const char *test_name = "ui_input_key_mapping";
    const char *failure = NULL;
    unsetenv("MESHCLIENT_QUIT_KEYS");
    mesh_ui_input_reload_quit_keys();

    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        return;
    }

    struct test_key_capture capture;
    memset(&capture, 0, sizeof capture);
    struct mesh_ui_input input;
    memset(&input, 0, sizeof input);
    input.loop = &loop; /* not opening /dev/input: only the translation is under test */
    mesh_ui_input_set_handler(&input, test_capture_key, &capture);

    /* The Brick's gamepad: face buttons as BTN_ codes, d-pad as hat axes. */
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_SOUTH, 1);
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_SOUTH, 0); /* release: nothing */
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_EAST, 1);
    mesh_ui_input_handle_event(&input, EV_ABS, ABS_HAT0Y, -1); /* up */
    mesh_ui_input_handle_event(&input, EV_ABS, ABS_HAT0Y, 0);  /* centre: nothing */
    mesh_ui_input_handle_event(&input, EV_ABS, ABS_HAT0X, 1);  /* right */
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_TL, 1);
    mesh_ui_input_handle_event(&input, EV_KEY, KEY_DOWN, 2); /* keyboard autorepeat counts */
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_SELECT, 1);
    mesh_ui_input_handle_event(&input, EV_SYN, 0, 0);
    mesh_ui_input_handle_event(&input, EV_KEY, KEY_F1, 1); /* unmapped: nothing */

    /* BTN_SOUTH is the Brick's B and BTN_EAST its A (Nintendo layout). */
    const enum mesh_ui_key expected[] = {
        MESH_UI_KEY_B,  MESH_UI_KEY_A,    MESH_UI_KEY_UP,     MESH_UI_KEY_RIGHT,
        MESH_UI_KEY_L1, MESH_UI_KEY_DOWN, MESH_UI_KEY_SELECT,
    };
    const size_t expected_count = sizeof(expected) / sizeof(expected[0]);
    if (capture.count != expected_count) {
        failure = "unexpected number of logical keys";
        goto cleanup;
    }
    for (size_t i = 0; i < expected_count; ++i) {
        if (capture.keys[i] != expected[i]) {
            failure = "logical key order mismatch";
            goto cleanup;
        }
    }
    if (loop.stop_requested) {
        failure = "navigation keys must not stop the loop";
        goto cleanup;
    }

    /* MENU (as either device reports it) still quits, and never reaches the handler. */
    mesh_ui_input_handle_event(&input, EV_KEY, BTN_MODE, 1);
    if (!loop.stop_requested || capture.count != expected_count) {
        failure = "MENU should stop the loop without emitting a key";
        goto cleanup;
    }
    if (mesh_ui_input_is_quit_key(BTN_SELECT) || mesh_ui_input_is_quit_key(BTN_START)) {
        failure = "SELECT/START are navigation keys, not quit keys";
        goto cleanup;
    }

cleanup:
    mesh_event_loop_shutdown(&loop);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

struct test_action_capture {
    struct mesh_ui_action last;
    size_t count;
};

static void test_capture_action(void *userdata, const struct mesh_ui_action *action) {
    struct test_action_capture *capture = (struct test_action_capture *)userdata;
    capture->last = *action;
    capture->count++;
}

static void test_ui_controller_key_dispatch(void) {
    const char *test_name = "ui_controller_key_dispatch";
    const char *failure = NULL;
    mesh_ui_canned_reset();

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

    struct mesh_ui_backend_stub_context backend;
    memset(&backend, 0, sizeof backend);
    struct mesh_ui_controller controller;
    if (mesh_ui_controller_init(&controller, &store, mesh_ui_backend_stub(), &backend, &loop) !=
        0) {
        mesh_ui_store_shutdown(&store);
        mesh_event_loop_shutdown(&loop);
        record_failure(test_name, "controller init failed");
        return;
    }
    struct test_action_capture actions;
    memset(&actions, 0, sizeof actions);
    mesh_ui_controller_set_action_handler(&controller, test_capture_action, &actions);

    test_nav_populate(&store);
    mesh_event_loop_run(&loop, 0);
    const size_t presents_before = backend.present_calls;

    /* Right x2 lands on Compose; the repaint arrives through the eventfd on the next turn. */
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_RIGHT);
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_RIGHT);
    mesh_event_loop_run(&loop, 0);
    if (backend.present_calls <= presents_before ||
        backend.last_snapshot.nav.screen != MESH_UI_SCREEN_COMPOSE ||
        (backend.last_snapshot.update_flags & MESH_UI_UPDATE_NAV) == 0U) {
        failure = "key presses should repaint with the new tab";
        goto cleanup;
    }

    /* Down past the draft row to the first canned reply, A sends it: the action reaches the
       handler once. */
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_DOWN);
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_DOWN);
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_A);
    if (actions.count != 1U || actions.last.type != MESH_UI_ACTION_SEND_TEXT ||
        actions.last.dest != MESH_MESSAGE_BROADCAST_ADDR ||
        strcmp(actions.last.text, mesh_ui_canned_text(0)) != 0) {
        failure = "send action did not reach the handler";
        goto cleanup;
    }

    /* Navigation-only keys never call the handler. */
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_UP);
    mesh_ui_controller_handle_key(&controller, MESH_UI_KEY_NONE);
    if (actions.count != 1U) {
        failure = "navigation keys must not produce actions";
        goto cleanup;
    }

cleanup:
    mesh_ui_controller_shutdown(&controller);
    mesh_ui_store_shutdown(&store);
    mesh_event_loop_shutdown(&loop);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/* BlueZ says the device is gone: the link resets, the UI sees "running", and auto-connect can
   try again. Checked via the explicit probe tick() runs every couple of seconds. */
static void test_ble_transport_link_drop(void) {
    const char *test_name = "ble_transport_link_drop";
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:0A", .name = "NodeTen", .rssi = -50},
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
static void test_ble_transport_write_failure(void) {
    const char *test_name = "ble_transport_write_failure";
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:0B", .name = "NodeEleven", .rssi = -50},
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

/* A packet from a node refreshes its last_heard/SNR; one from a node the sync never delivered
   adds it, so the UI can name and target whoever is actually talking to us. */
static void test_ble_transport_packet_touches_node(void) {
    const char *test_name = "ble_transport_packet_touches_node";
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:0C", .name = "NodeTwelve", .rssi = -50},
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
    if (!pb_encode(&stream, meshtastic_FromRadio_fields, &from_radio)) {
        record_failure(test_name, "encode packet failed");
        return;
    }
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
    if (!pb_encode(&stream, meshtastic_FromRadio_fields, &from_radio)) {
        record_failure(test_name, "encode position failed");
        return;
    }
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
    if (status.nodes[1].node_id != 0x0badf00dU || status.nodes[1].last_heard != 6000U ||
        status.nodes[1].short_name[0] != '\0') {
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

/* ---- radio settings / admin ---------------------------------------------------------------- */

static bool test_encode_from_radio(const meshtastic_FromRadio *message, uint8_t *out, size_t cap,
                                   size_t *out_len) {
    pb_ostream_t stream = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&stream, meshtastic_FromRadio_fields, message)) {
        return false;
    }
    *out_len = stream.bytes_written;
    return true;
}

/* Builds the ADMIN_APP reply a radio would send for `admin`, quoting `request_id`. */
static bool test_make_admin_reply(uint32_t my_node, uint32_t request_id,
                                  const meshtastic_AdminMessage *admin,
                                  meshtastic_MeshPacket *out) {
    uint8_t payload[meshtastic_AdminMessage_size];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof payload);
    if (!pb_encode(&stream, meshtastic_AdminMessage_fields, admin)) {
        return false;
    }
    *out = make_decoded_packet(my_node, my_node, 0U, 0x5150U, meshtastic_PortNum_ADMIN_APP, payload,
                               stream.bytes_written);
    out->decoded.request_id = request_id;
    return true;
}

static void test_radio_settings_admin_roundtrip(void) {
    const char *test_name = "radio_settings_admin_roundtrip";
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
    if (!test_make_admin_reply(my_node, 77U, &admin, &reply)) {
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
    meshtastic_MeshPacket text = make_decoded_packet(0x1234U, my_node, 0U, 99U,
                                                     meshtastic_PortNum_TEXT_MESSAGE_APP, "hi", 2U);
    if (mesh_radio_settings_ingest(&settings, &text) != 0) {
        record_failure(test_name, "text packet should be left alone");
        return;
    }

    record_success(test_name);
}

static void test_radio_settings_fetch_queue(void) {
    const char *test_name = "radio_settings_fetch_queue";
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
    if (!test_make_admin_reply(0x10U, 2U, &admin, &reply) ||
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

static void test_ui_settings_items(void) {
    const char *test_name = "ui_settings_items";

    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.use_preset = true;
    settings.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    settings.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE;
    settings.hop_limit = 3U;
    settings.tx_enabled = true;
    settings.has_device = true;
    settings.role = meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
    settings.has_security = true;
    settings.public_key_len = 32U;
    settings.public_key[0] = 0xDEU;
    settings.public_key[1] = 0xADU;
    settings.public_key[2] = 0xBEU;
    settings.public_key[3] = 0xEFU;

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.channel_count = 2U;
    handshake.channels[0].index = 0U;
    handshake.channels[0].role = 1U;
    handshake.channels[0].psk_len = 1U;
    handshake.channels[1].index = 1U;
    handshake.channels[1].role = 2U;
    handshake.channels[1].psk_len = 16U;
    handshake.channels[1].uplink_enabled = true;
    snprintf(handshake.channels[1].name, sizeof handshake.channels[1].name, "%s", "Team");

    if (!mesh_ui_settings_section_loaded(&settings, &handshake, MESH_UI_SETTINGS_LORA) ||
        mesh_ui_settings_section_loaded(&settings, &handshake, MESH_UI_SETTINGS_DISPLAY) ||
        mesh_ui_settings_item_count(&settings, &handshake, MESH_UI_SETTINGS_DISPLAY,
                                    MESH_UI_SETTINGS_NO_CHANNEL) != 0U) {
        record_failure(test_name, "section loaded flags are wrong");
        return;
    }

    struct mesh_ui_settings_item item;
    if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_LORA,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        strcmp(item.label, "Region") != 0 || strcmp(item.value, "US") != 0 ||
        item.kind != MESH_UI_SETTING_ENUM) {
        record_failure(test_name, "LoRa region row is wrong");
        return;
    }
    if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_LORA,
                               MESH_UI_SETTINGS_NO_CHANNEL, 2U, &item) ||
        strcmp(item.label, "Preset") != 0 || strcmp(item.value, "Long Range - Moderate") != 0) {
        record_failure(test_name, "LoRa preset row is wrong");
        return;
    }
    if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_DEVICE,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        strcmp(item.value, "Router Late") != 0) {
        record_failure(test_name, "device role row is wrong");
        return;
    }
    if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_SECURITY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        item.kind != MESH_UI_SETTING_KEY || strncmp(item.value, "deadbeef...", 11U) != 0 ||
        strstr(item.value, "32 bytes") == NULL) {
        record_failure(test_name, "public key fingerprint is wrong");
        return;
    }
    if (mesh_ui_settings_item_count(&settings, &handshake, MESH_UI_SETTINGS_CHANNELS,
                                    MESH_UI_SETTINGS_NO_CHANNEL) != 2U ||
        !mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                               MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
        strcmp(item.label, "1 Team") != 0 || strstr(item.value, "AES-128") == NULL ||
        strstr(item.value, "up on") == NULL || strstr(item.value, "down off") == NULL) {
        record_failure(test_name, "channel row is wrong");
        return;
    }
    if (!mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        strcmp(item.label, "0 Primary") != 0 || strstr(item.value, "default key") == NULL) {
        record_failure(test_name, "primary channel row is wrong");
        return;
    }
    if (mesh_ui_settings_item(&settings, &handshake, NULL, 0U, MESH_UI_SETTINGS_LORA,
                              MESH_UI_SETTINGS_NO_CHANNEL, 99U, &item)) {
        record_failure(test_name, "out-of-range row should fail");
        return;
    }
    record_success(test_name);
}

static void test_ui_nav_settings(void) {
    const char *test_name = "ui_nav_settings";
    const char *failure = NULL;

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.use_preset = true;
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    /* Messages → Nodes → Compose → Devices → Status → Settings. */
    for (int i = 0; i < 5; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    if (store.nav.screen != MESH_UI_SCREEN_SETTINGS ||
        store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) !=
            MESH_UI_SETTINGS_SECTION_COUNT) {
        failure = "Settings tab should open on the section list";
        goto cleanup;
    }
    for (int i = 0; i < MESH_UI_SETTINGS_LORA; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    if (store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != MESH_UI_SETTINGS_LORA) {
        failure = "cursor should sit on LoRa";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    const uint32_t lora_rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS);
    if (store.nav.settings_section != MESH_UI_SETTINGS_LORA ||
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 0U || lora_rows == 0U ||
        action.type != MESH_UI_ACTION_NONE) {
        failure = "A should open the LoRa section";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    if (store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 1U) {
        failure = "Down should move within the section";
        goto cleanup;
    }
    /* A on the "Use preset" toggle edits it in place; nothing is sent until Y. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_NONE || store.nav.settings_section != MESH_UI_SETTINGS_LORA ||
        store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_LORA_USE_PRESET) {
        failure = "A on a toggle should record an edit";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (action.type != MESH_UI_ACTION_REFRESH_SETTINGS) {
        failure = "X should ask for a refresh";
        goto cleanup;
    }
    /* B with an edit asks first; B again discards and leaves. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (!store.nav.settings_discard_armed || store.nav.settings_section != MESH_UI_SETTINGS_LORA) {
        failure = "B with an edit should ask before leaving";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION ||
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != MESH_UI_SETTINGS_LORA ||
        store.nav.screen != MESH_UI_SCREEN_SETTINGS) {
        failure = "B should return to the section list at the same row";
        goto cleanup;
    }
    /* An unloaded section opens empty rather than refusing; the backend explains. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_DISPLAY ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != 0U) {
        failure = "unloaded section should open with no rows";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/* The whole path on the mock bus: handshake fragments land in the settings, the completed
   handshake triggers the admin probe, the reply is consumed (not logged as a message) and
   the passkey is held for the next request. */
static void test_ble_transport_admin_probe(void) {
    const char *test_name = "ble_transport_admin_probe";
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:0A", .name = "NodeAdmin", .rssi = -40},
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
    test_encode_from_radio(&from_radio, read_buffers[0], sizeof read_buffers[0],
                           &read_payload_lengths[0]);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_config_tag;
    from_radio.config.which_payload_variant = meshtastic_Config_lora_tag;
    from_radio.config.payload_variant.lora.region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
    from_radio.config.payload_variant.lora.hop_limit = 5U;
    test_encode_from_radio(&from_radio, read_buffers[1], sizeof read_buffers[1],
                           &read_payload_lengths[1]);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_moduleConfig_tag;
    from_radio.moduleConfig.which_payload_variant = meshtastic_ModuleConfig_store_forward_tag;
    from_radio.moduleConfig.payload_variant.store_forward.enabled = true;
    test_encode_from_radio(&from_radio, read_buffers[2], sizeof read_buffers[2],
                           &read_payload_lengths[2]);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    from_radio.node_info.num = my_node;
    from_radio.node_info.has_user = true;
    snprintf(from_radio.node_info.user.short_name, sizeof from_radio.node_info.user.short_name,
             "%s", "0ad8");
    snprintf(from_radio.node_info.user.long_name, sizeof from_radio.node_info.user.long_name, "%s",
             "Meshtastic 0ad8");
    test_encode_from_radio(&from_radio, read_buffers[3], sizeof read_buffers[3],
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
    test_encode_from_radio(&from_radio, read_buffers[5], sizeof read_buffers[5],
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
    if (!test_make_admin_reply(my_node, sent.packet.id, &admin, &from_radio.packet)) {
        failure = "encode metadata reply failed";
        goto cleanup;
    }
    read_index = 7U;
    test_encode_from_radio(&from_radio, read_buffers[7], sizeof read_buffers[7],
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
    if (queued != 21) {
        failure = "refresh should queue all thirteen sections and eight channel slots";
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

/* The write path in isolation: a save queues passkey refresh, set_*, read-back; the set_*
   carries the full section and the passkey; a Routing reply quoting the id settles it. */
static void test_radio_settings_write_queue(void) {
    const char *test_name = "radio_settings_write_queue";
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
    if (!test_make_admin_reply(0x1234U, 41U, &reply, &packet) ||
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
    meshtastic_MeshPacket stray = make_routing_reply(7U, meshtastic_Routing_Error_NONE);
    if (mesh_radio_settings_ingest(&settings, &stray) != 0 ||
        !mesh_radio_settings_busy(&settings)) {
        record_failure(test_name, "an unrelated routing reply must be left alone");
        return;
    }
    meshtastic_MeshPacket ack = make_routing_reply(42U, meshtastic_Routing_Error_NONE);
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
    test_make_admin_reply(0x1234U, 50U, &reply, &packet);
    mesh_radio_settings_ingest(&settings, &packet);
    mesh_radio_settings_next_request(&settings, 2U, &next);
    mesh_radio_settings_mark_sent(&settings, 51U, 2U);
    meshtastic_MeshPacket nak =
        make_routing_reply(51U, meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY);
    if (mesh_radio_settings_ingest(&settings, &nak) != 1 || settings.writes_failed != 1U ||
        settings.last_write_error != (int32_t)meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY) {
        record_failure(test_name, "a rejection should carry its reason");
        return;
    }
    mesh_radio_settings_queue_write(&settings, &owner_write);
    mesh_radio_settings_next_request(&settings, 3U, &next); /* get_owner */
    mesh_radio_settings_mark_sent(&settings, 52U, 3U);
    test_make_admin_reply(0x1234U, 52U, &reply, &packet);
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

/* Editable rows: the field table, pending edits rendered in place, and the steppers. */
static void test_ui_settings_edits(void) {
    const char *test_name = "ui_settings_edits";
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_owner = true;
    snprintf(settings.long_name, sizeof settings.long_name, "%s", "Meshtastic 0ad8");
    snprintf(settings.short_name, sizeof settings.short_name, "%s", "0ad8");
    settings.has_display = true;
    settings.screen_on_secs = 60U;
    settings.compass_orientation = 7U;
    settings.units = 1U;
    settings.has_telemetry = true;
    settings.device_update_interval = 1234U; /* not a preset */

    struct mesh_ui_settings_item item;
    if (!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_USER,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        item.field != MESH_UI_FIELD_USER_LONG_NAME || item.kind != MESH_UI_SETTING_TEXT ||
        item.dirty || strcmp(item.text, "Meshtastic 0ad8") != 0 ||
        strcmp(item.value, "Meshtastic 0ad8") != 0) {
        record_failure(test_name, "long name row is wrong");
        return;
    }
    if (mesh_ui_settings_text_max(MESH_UI_FIELD_USER_LONG_NAME) != 24U ||
        mesh_ui_settings_text_max(MESH_UI_FIELD_USER_SHORT_NAME) != 4U ||
        mesh_ui_settings_text_max(MESH_UI_FIELD_DISPLAY_12H) != 0U) {
        record_failure(test_name, "text caps are wrong");
        return;
    }
    if (!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_DISPLAY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        item.field != MESH_UI_FIELD_DISPLAY_SCREEN_ON || item.kind != MESH_UI_SETTING_NUMBER ||
        item.number != 60U || strcmp(item.value, "1m") != 0) {
        record_failure(test_name, "screen-on row is wrong");
        return;
    }
    if (mesh_ui_settings_number_step(MESH_UI_FIELD_DISPLAY_SCREEN_ON, 60U, +1) != 120U ||
        mesh_ui_settings_number_step(MESH_UI_FIELD_DISPLAY_SCREEN_ON, 60U, -1) != 30U ||
        mesh_ui_settings_number_step(MESH_UI_FIELD_DISPLAY_SCREEN_ON, 3600U, +1) != 3600U ||
        mesh_ui_settings_number_step(MESH_UI_FIELD_DISPLAY_SCREEN_ON, 0U, -1) != 0U ||
        mesh_ui_settings_number_step(MESH_UI_FIELD_TELEMETRY_INTERVAL, 1234U, +1) != 1800U ||
        mesh_ui_settings_number_step(MESH_UI_FIELD_TELEMETRY_INTERVAL, 1234U, -1) != 900U) {
        record_failure(test_name, "number presets step wrong");
        return;
    }
    if (mesh_ui_settings_enum_count(MESH_UI_FIELD_DISPLAY_COMPASS) != 8U ||
        mesh_ui_settings_enum_count(MESH_UI_FIELD_DISPLAY_UNITS) != 2U ||
        mesh_ui_settings_enum_count(MESH_UI_FIELD_DISPLAY_FLIP) != 0U ||
        strcmp(mesh_ui_settings_enum_name(MESH_UI_FIELD_DISPLAY_UNITS, 1U), "Imperial") != 0 ||
        strcmp(mesh_ui_settings_enum_name(MESH_UI_FIELD_DISPLAY_COMPASS, 7U), "270 flip") != 0) {
        record_failure(test_name, "enum tables are wrong");
        return;
    }
    if (mesh_ui_settings_field_section(MESH_UI_FIELD_SF_SERVER) != MESH_UI_SETTINGS_STORE_FORWARD ||
        mesh_ui_settings_field_kind(MESH_UI_FIELD_TELEMETRY_INTERVAL) != MESH_UI_SETTING_NUMBER ||
        strcmp(mesh_ui_settings_field_label(MESH_UI_FIELD_USER_SHORT_NAME), "Short name") != 0) {
        record_failure(test_name, "field descriptions are wrong");
        return;
    }
    /* A read-only row has no field. */
    if (!mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_TELEMETRY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
        item.field != MESH_UI_FIELD_TELEMETRY_INTERVAL || strcmp(item.value, "1234s") != 0) {
        record_failure(test_name, "telemetry interval row is wrong");
        return;
    }

    /* Pending edits show in place, marked. */
    struct mesh_ui_setting_edit edits[2];
    memset(edits, 0, sizeof edits);
    edits[0].field = MESH_UI_FIELD_DISPLAY_UNITS;
    edits[0].number = 0U;
    edits[1].field = MESH_UI_FIELD_USER_SHORT_NAME;
    snprintf(edits[1].text, sizeof edits[1].text, "%s", "BRCK");
    if (!mesh_ui_settings_item(&settings, NULL, edits, 2U, MESH_UI_SETTINGS_DISPLAY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 4U, &item) ||
        item.field != MESH_UI_FIELD_DISPLAY_UNITS || !item.dirty || item.number != 0U ||
        strcmp(item.value, "Metric") != 0) {
        record_failure(test_name, "an enum edit should render in place");
        return;
    }
    if (!mesh_ui_settings_item(&settings, NULL, edits, 2U, MESH_UI_SETTINGS_USER,
                               MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
        !item.dirty || strcmp(item.text, "BRCK") != 0 || strcmp(item.value, "BRCK") != 0) {
        record_failure(test_name, "a text edit should render in place");
        return;
    }
    if (!mesh_ui_settings_item(&settings, NULL, edits, 2U, MESH_UI_SETTINGS_DISPLAY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        item.dirty || item.number != 60U) {
        record_failure(test_name, "rows without an edit stay clean");
        return;
    }
    if (mesh_ui_settings_find_edit(edits, 2U, MESH_UI_FIELD_USER_SHORT_NAME) != &edits[1] ||
        mesh_ui_settings_find_edit(edits, 2U, MESH_UI_FIELD_DISPLAY_FLIP) != NULL) {
        record_failure(test_name, "find_edit is wrong");
        return;
    }
    record_success(test_name);
}

/* Editing through the nav: Left/Right and A change rows, the keyboard edits text and gives
   the Compose draft back, Y emits the save, B asks before discarding. */
static void test_ui_nav_settings_edit(void) {
    const char *test_name = "ui_nav_settings_edit";
    const char *failure = NULL;

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_owner = true;
    snprintf(settings.long_name, sizeof settings.long_name, "%s", "Old Name");
    snprintf(settings.short_name, sizeof settings.short_name, "%s", "OLDN");
    settings.has_display = true;
    settings.screen_on_secs = 60U;
    settings.use_12h_clock = false;
    settings.units = 0U;
    settings.has_lora = true;
    mesh_ui_store_set_settings(&store, &settings);
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "half typed");

    struct mesh_ui_action action;
    for (int i = 0; i < 5; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    for (int i = 0; i < MESH_UI_SETTINGS_DISPLAY; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_DISPLAY) {
        failure = "Display should open";
        goto cleanup;
    }

    /* Right on Screen on steps to the next preset; Left twice goes back past it. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    struct mesh_ui_settings_item item;
    if (store.nav.screen != MESH_UI_SCREEN_SETTINGS || store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_DISPLAY_SCREEN_ON ||
        store.nav.settings_edits[0].number != 120U ||
        !mesh_ui_settings_item(&store.settings, NULL, store.nav.settings_edits,
                               store.nav.settings_edit_count, MESH_UI_SETTINGS_DISPLAY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        !item.dirty || strcmp(item.value, "2m") != 0) {
        failure = "Right should step the number and stay on the tab";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.settings_edit_count != 0U) {
        failure = "stepping back to the radio's value should drop the edit";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.settings_edit_count != 1U || store.nav.settings_edits[0].number != 30U) {
        failure = "Left should step down";
        goto cleanup;
    }
    /* Down to 12-hour clock: A flips a toggle. Down to Units: Left wraps the enum. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.settings_edit_count != 3U ||
        mesh_ui_settings_find_edit(store.nav.settings_edits, 3U, MESH_UI_FIELD_DISPLAY_12H) ==
            NULL ||
        mesh_ui_settings_find_edit(store.nav.settings_edits, 3U, MESH_UI_FIELD_DISPLAY_12H)
                ->number != 1U ||
        mesh_ui_settings_find_edit(store.nav.settings_edits, 3U, MESH_UI_FIELD_DISPLAY_UNITS) ==
            NULL ||
        mesh_ui_settings_find_edit(store.nav.settings_edits, 3U, MESH_UI_FIELD_DISPLAY_UNITS)
                ->number != 1U) {
        failure = "toggle and enum edits are wrong";
        goto cleanup;
    }
    if (mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != 6U) {
        failure = "edits must not change the row count";
        goto cleanup;
    }

    /* B asks first; a different key stands the question down; B twice discards. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (!store.nav.settings_discard_armed ||
        store.nav.settings_section != MESH_UI_SETTINGS_DISPLAY) {
        failure = "B with edits should ask, not leave";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    if (store.nav.settings_discard_armed) {
        failure = "another key should cancel the discard question";
        goto cleanup;
    }
    /* Y saves: the action carries the section and every edit; the nav keeps them until the
       app says so. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (action.type != MESH_UI_ACTION_SAVE_SETTINGS || action.section != MESH_UI_SETTINGS_DISPLAY ||
        action.edit_count != 3U || action.edits[0].field != MESH_UI_FIELD_DISPLAY_SCREEN_ON ||
        action.edits[0].number != 30U || store.nav.settings_edit_count != 3U) {
        failure = "Y should emit a save with the edits";
        goto cleanup;
    }
    mesh_ui_store_settings_edits_clear(&store);
    if (store.nav.settings_edit_count != 0U || (store.pending_flags & MESH_UI_UPDATE_NAV) == 0U) {
        failure = "clearing the edits should repaint";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "Y with nothing to save does nothing";
        goto cleanup;
    }

    /* Text: A on Short name opens the keyboard on that field with the value preloaded, the
       Compose draft parked; typing is capped at four bytes; done records the edit. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    for (int i = 0; i < MESH_UI_SETTINGS_DISPLAY - MESH_UI_SETTINGS_USER; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open || store.nav.keyboard_field != MESH_UI_FIELD_USER_SHORT_NAME ||
        strcmp(store.nav.draft, "OLDN") != 0 || strcmp(store.nav.draft_saved, "half typed") != 0 ||
        strcmp(mesh_ui_kb_action_label(&store.nav, MESH_UI_KB_ACTION_SEND), "done") != 0) {
        failure = "A on a text row should open the keyboard for it";
        goto cleanup;
    }
    /* Row 0 col 0 of the lower layer is '1': appending at the cap is refused. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (strcmp(store.nav.draft, "OLDN") != 0) {
        failure = "the draft must respect the field's byte cap";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action); /* delete -> OLD */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* '1' -> OLD1 */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (store.nav.keyboard_open || store.nav.screen != MESH_UI_SCREEN_SETTINGS ||
        store.nav.keyboard_field != MESH_UI_FIELD_NONE ||
        strcmp(store.nav.draft, "half typed") != 0 || action.type != MESH_UI_ACTION_NONE ||
        store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_USER_SHORT_NAME ||
        strcmp(store.nav.settings_edits[0].text, "OLD1") != 0) {
        failure = "done should record the text edit and restore the Compose draft";
        goto cleanup;
    }
    /* Reopen and cancel: nothing changes. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open || strcmp(store.nav.draft, "OLD1") != 0) {
        failure = "the keyboard should preload the pending edit";
        goto cleanup;
    }
    store.nav.kb_row = MESH_UI_KB_CHAR_ROWS;
    store.nav.kb_col = MESH_UI_KB_ACTION_CANCEL;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.keyboard_open || store.nav.settings_edit_count != 1U ||
        strcmp(store.nav.draft, "half typed") != 0) {
        failure = "cancel should keep the edit as it was";
        goto cleanup;
    }
    /* B twice leaves the section with the edits gone. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION ||
        store.nav.settings_edit_count != 0U) {
        failure = "B twice should discard and go back";
        goto cleanup;
    }
    /* Left on the section list still switches tabs. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_STATUS) {
        failure = "Left on the section list should switch tabs";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/* The app turns a save into a full-section write from the radio's own copy. */
static void test_app_settings_write_build(void) {
    const char *test_name = "app_settings_write_build";
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    radio.has_owner = true;
    snprintf(radio.owner.long_name, sizeof radio.owner.long_name, "%s", "Old Name");
    snprintf(radio.owner.short_name, sizeof radio.owner.short_name, "%s", "OLDN");
    radio.owner.public_key.size = 32U;
    radio.owner.public_key.bytes[0] = 0x42U;
    radio.has_telemetry = true;
    radio.telemetry.device_update_interval = 900U;
    radio.telemetry.environment_measurement_enabled = true;
    radio.telemetry.power_update_interval = 777U;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_USER;
    action.edit_count = 3U;
    action.edits[0].field = MESH_UI_FIELD_USER_LONG_NAME;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "Brick");
    action.edits[1].field = MESH_UI_FIELD_USER_UNMESSAGEABLE;
    action.edits[1].number = 1U;
    action.edits[2].field = MESH_UI_FIELD_DISPLAY_FLIP; /* wrong section: ignored */
    action.edits[2].number = 1U;

    struct mesh_admin_request write;
    if (mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
        write.kind != MESH_ADMIN_SET_OWNER || strcmp(write.payload.owner.long_name, "Brick") != 0 ||
        strcmp(write.payload.owner.short_name, "OLDN") != 0 ||
        !write.payload.owner.has_is_unmessagable || !write.payload.owner.is_unmessagable ||
        write.payload.owner.public_key.size != 32U ||
        write.payload.owner.public_key.bytes[0] != 0x42U) {
        record_failure(test_name, "set_owner should be the radio's user plus the edits");
        return;
    }

    action.section = MESH_UI_SETTINGS_TELEMETRY;
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_TELEMETRY_INTERVAL;
    action.edits[0].number = 3600U;
    action.edits[1].field = MESH_UI_FIELD_TELEMETRY_DEVICE;
    action.edits[1].number = 1U;
    if (mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
        write.kind != MESH_ADMIN_SET_MODULE_CONFIG ||
        write.type != meshtastic_AdminMessage_ModuleConfigType_TELEMETRY_CONFIG ||
        write.payload.module_config.which_payload_variant !=
            meshtastic_ModuleConfig_telemetry_tag ||
        write.payload.module_config.payload_variant.telemetry.device_update_interval != 3600U ||
        !write.payload.module_config.payload_variant.telemetry.device_telemetry_enabled ||
        !write.payload.module_config.payload_variant.telemetry.environment_measurement_enabled ||
        write.payload.module_config.payload_variant.telemetry.power_update_interval != 777U) {
        record_failure(test_name, "set_module_config should keep the fields we do not show");
        return;
    }

    action.section = MESH_UI_SETTINGS_DISPLAY;
    if (mesh_app_build_settings_write(&radio, &action, &write) != -ENOENT) {
        record_failure(test_name, "a section the radio has not sent cannot be written");
        return;
    }
    action.section = MESH_UI_SETTINGS_RADIO;
    if (mesh_app_build_settings_write(&radio, &action, &write) != -ENOTSUP) {
        record_failure(test_name, "the Radio section is read-only");
        return;
    }
    record_success(test_name);
}

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
        test_make_admin_reply(my_node, sent.packet.id, &reply, &from_radio.packet);
        break;
    case meshtastic_AdminMessage_get_device_metadata_request_tag:
        reply.which_payload_variant = meshtastic_AdminMessage_get_device_metadata_response_tag;
        test_make_admin_reply(my_node, sent.packet.id, &reply, &from_radio.packet);
        break;
    case meshtastic_AdminMessage_get_module_config_request_tag:
        reply.which_payload_variant = meshtastic_AdminMessage_get_module_config_response_tag;
        reply.get_module_config_response.which_payload_variant =
            meshtastic_ModuleConfig_store_forward_tag;
        reply.get_module_config_response.payload_variant.store_forward.enabled = true;
        reply.get_module_config_response.payload_variant.store_forward.heartbeat = true;
        test_make_admin_reply(my_node, sent.packet.id, &reply, &from_radio.packet);
        break;
    case meshtastic_AdminMessage_set_module_config_tag:
    case meshtastic_AdminMessage_set_config_tag:
    case meshtastic_AdminMessage_set_owner_tag:
        from_radio.packet = make_routing_reply(sent.packet.id, set_result);
        from_radio.packet.from = my_node;
        from_radio.packet.to = my_node;
        break;
    default:
        return -3;
    }
    if (!test_encode_from_radio(&from_radio, out, out_cap, out_len)) {
        return -4;
    }
    return (int)admin.which_payload_variant;
}

/* End to end on the mock bus: a Store & Forward save goes out as passkey refresh, set with
   the passkey and the full section, read-back; the Routing ack is consumed rather than logged,
   the counters move, and the read-back updates the view. */
static void test_ble_transport_settings_write(void) {
    const char *test_name = "ble_transport_settings_write";
    const char *failure = NULL;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:0B", .name = "NodeWrite", .rssi = -40},
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
    test_encode_from_radio(&from_radio, read_buffers[0], sizeof read_buffers[0],
                           &read_payload_lengths[0]);
    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_moduleConfig_tag;
    from_radio.moduleConfig.which_payload_variant = meshtastic_ModuleConfig_store_forward_tag;
    from_radio.moduleConfig.payload_variant.store_forward.enabled = false;
    from_radio.moduleConfig.payload_variant.store_forward.records = 500U;
    test_encode_from_radio(&from_radio, read_buffers[1], sizeof read_buffers[1],
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

/* Channels in the core: the table is kept whole, get_channel is one-based on the wire, and a
   set_channel write reads its slot back. */
static void test_radio_settings_channel_write(void) {
    const char *test_name = "radio_settings_channel_write";
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
    test_make_admin_reply(0x1234U, 10U, &reply, &packet);
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
    meshtastic_MeshPacket ack = make_routing_reply(11U, meshtastic_Routing_Error_NONE);
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

/* Channel editing through the nav: opening a slot, walking the key choices, typing a key,
   and the confirm overlay that stands between Y and the write. */
static void test_ui_nav_channel_edit(void) {
    const char *test_name = "ui_nav_channel_edit";
    const char *failure = NULL;

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    test_nav_populate(&store);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_channels = true;
    settings.channels[0].present = true;
    settings.channels[0].role = 1U;
    settings.channels[0].psk_len = 1U;
    settings.channels[0].psk[0] = 1U;
    settings.channels[1].present = true;
    settings.channels[1].index = 1U;
    settings.channels[1].role = 2U;
    snprintf(settings.channels[1].name, sizeof settings.channels[1].name, "%s", "Team");
    settings.channels[1].psk_len = 16U;
    for (unsigned i = 0; i < 16U; ++i) {
        settings.channels[1].psk[i] = (uint8_t)(0xA0U + i);
    }
    settings.channels[1].position_precision = 13U;
    settings.channels[2].present = true; /* disabled: not listed */
    settings.channels[2].index = 2U;
    settings.has_bluetooth = true;
    settings.pairing_mode = 0U;
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    struct mesh_ui_settings_item item;
    for (int i = 0; i < 5; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    for (int i = 0; i < MESH_UI_SETTINGS_CHANNELS; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_CHANNELS ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != 3U ||
        mesh_ui_settings_channel_at_row(&store.settings, NULL, 1U) != 1 ||
        mesh_ui_settings_channel_at_row(&store.settings, NULL, 2U) != 2 ||
        mesh_ui_settings_channel_at_row(&store.settings, NULL, 3U) != -1 ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                               MESH_UI_SETTINGS_NO_CHANNEL, 2U, &item) ||
        strcmp(item.label, "2 (empty)") != 0 || strstr(item.value, "disabled") == NULL) {
        failure = "the channel list should show every slot, the empty one openable";
        goto cleanup;
    }
    /* An empty slot opens with the same rows, role Disabled: that is how a channel is added. */
    if (mesh_ui_settings_item_count(&store.settings, NULL, MESH_UI_SETTINGS_CHANNELS, 2U) != 6U ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 2U, 1U,
                               &item) ||
        item.field != MESH_UI_FIELD_CHANNEL_ROLE || item.number != 0U) {
        failure = "an empty slot should open with an editable Disabled role";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.settings_channel != 1U || store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 0U ||
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_SETTINGS) != 6U ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 1U, 2U,
                               &item) ||
        item.field != MESH_UI_FIELD_CHANNEL_KEY || item.kind != MESH_UI_SETTING_KEY ||
        strcmp(item.text, "oKGio6SlpqeoqaqrrK2urw==") != 0 ||
        strstr(item.value, "oKGio6Sl...") == NULL || strstr(item.value, "AES-128") == NULL ||
        !mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 1U, 5U,
                               &item) ||
        strcmp(item.value, "~3 km") != 0) {
        failure = "A should open channel 1 with its six rows";
        goto cleanup;
    }
    /* The primary slot's role is not offered. */
    if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_CHANNELS, 0U, 1U,
                               &item) ||
        item.field != MESH_UI_FIELD_NONE || strcmp(item.value, "Primary") != 0) {
        failure = "the primary channel's role should be read-only";
        goto cleanup;
    }

    /* Key row: Right walks default / random 128 / random 256 / none / back to keep. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    if (store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].field != MESH_UI_FIELD_CHANNEL_KEY ||
        store.nav.settings_edits[0].number != MESH_UI_PSK_DEFAULT ||
        !mesh_ui_settings_item(&store.settings, NULL, store.nav.settings_edits, 1U,
                               MESH_UI_SETTINGS_CHANNELS, 1U, 2U, &item) ||
        !item.dirty || strcmp(item.value, "default key") != 0 || strlen(item.text) != 24U) {
        failure = "Right on the key should pick the default key and keep the text for the keyboard";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    if (store.nav.settings_edits[0].number != MESH_UI_PSK_RANDOM_256) {
        failure = "Right twice more should reach random AES-256";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.settings_edit_count != 0U) {
        failure = "Left back to keep should drop the edit";
        goto cleanup;
    }
    /* A opens the keyboard on the key as base64; a bad key keeps it open; a good one is
       recorded. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open || store.nav.keyboard_field != MESH_UI_FIELD_CHANNEL_KEY ||
        strcmp(store.nav.draft, "oKGio6SlpqeoqaqrrK2urw==") != 0) {
        failure = "A on the key should open the keyboard on the current key as base64";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action); /* 23 chars: not base64 */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (!store.nav.keyboard_open || store.nav.settings_edit_count != 0U) {
        failure = "a truncated key should be refused and the keyboard stay open";
        goto cleanup;
    }
    /* Delete "w=" too, then type "a==": still 16 bytes, last byte different. 'a' is row 2
       col 0 of the lower layer; '=' is row 1 col 2 of the symbol layer. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (strcmp(store.nav.draft, "oKGio6SlpqeoqaqrrK2ura==") != 0) {
        failure = "typing on the key keyboard went wrong";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (store.nav.keyboard_open || store.nav.settings_edit_count != 1U ||
        store.nav.settings_edits[0].number != MESH_UI_PSK_TYPED ||
        strcmp(store.nav.settings_edits[0].text, "oKGio6SlpqeoqaqrrK2ura==") != 0) {
        failure = "a valid typed key should be recorded";
        goto cleanup;
    }
    /* Y asks first; B cancels; Y, Up, A saves with the channel slot in the action. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (!store.nav.confirm_open || store.nav.confirm_cursor != 1U ||
        action.type != MESH_UI_ACTION_NONE) {
        failure = "Y on a channel should open the confirm overlay on Cancel";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_NONE ||
        store.nav.settings_edit_count != 1U) {
        failure = "A on Cancel should close the overlay and keep the edits";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.confirm_open || action.type != MESH_UI_ACTION_SAVE_SETTINGS ||
        action.section != MESH_UI_SETTINGS_CHANNELS || action.channel != 1U ||
        action.edit_count != 1U || action.edits[0].field != MESH_UI_FIELD_CHANNEL_KEY) {
        failure = "confirming should emit the save for channel 1";
        goto cleanup;
    }
    mesh_ui_store_settings_edits_clear(&store);
    /* B leaves the channel for the list, then the list for the sections. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.settings_channel != MESH_UI_SETTINGS_NO_CHANNEL ||
        store.nav.settings_section != MESH_UI_SETTINGS_CHANNELS ||
        store.nav.cursor[MESH_UI_SCREEN_SETTINGS] != 1U) {
        failure = "B should return to the channel list at the same row";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.settings_section != MESH_UI_SETTINGS_NO_SECTION) {
        failure = "B again should return to the section list";
        goto cleanup;
    }
    /* Bluetooth asks too; Display does not. */
    if (!mesh_ui_settings_section_needs_confirm(MESH_UI_SETTINGS_BLUETOOTH) ||
        mesh_ui_settings_section_needs_confirm(MESH_UI_SETTINGS_DISPLAY)) {
        failure = "confirm applies to Bluetooth and Channels only";
        goto cleanup;
    }
    if (!mesh_ui_settings_item(&store.settings, NULL, NULL, 0U, MESH_UI_SETTINGS_BLUETOOTH,
                               MESH_UI_SETTINGS_NO_CHANNEL, 2U, &item) ||
        item.field != MESH_UI_FIELD_BT_PIN || strcmp(item.text, "000000") != 0 ||
        mesh_ui_settings_text_max(MESH_UI_FIELD_BT_PIN) != 6U) {
        failure = "the Bluetooth PIN row is wrong";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/* Key choices become bytes, roles map back, and a bad PIN never reaches the radio. */
static void test_app_channel_write_build(void) {
    const char *test_name = "app_channel_write_build";
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    meshtastic_Channel channel = meshtastic_Channel_init_default;
    channel.index = 1;
    channel.role = meshtastic_Channel_Role_SECONDARY;
    channel.has_settings = true;
    channel.settings.psk.size = 1U;
    channel.settings.psk.bytes[0] = 1U;
    channel.settings.id = 77U;
    mesh_radio_settings_apply_channel(&radio, &channel);
    radio.has_bluetooth = true;
    radio.bluetooth.enabled = true;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_CHANNELS;
    action.channel = 1U;
    action.edit_count = 4U;
    action.edits[0].field = MESH_UI_FIELD_CHANNEL_KEY;
    action.edits[0].number = MESH_UI_PSK_RANDOM_256;
    action.edits[1].field = MESH_UI_FIELD_CHANNEL_NAME;
    snprintf(action.edits[1].text, sizeof action.edits[1].text, "%s", "Hikers");
    action.edits[2].field = MESH_UI_FIELD_CHANNEL_ROLE;
    action.edits[2].number = 0U; /* disabled */
    action.edits[3].field = MESH_UI_FIELD_CHANNEL_POSITION;
    action.edits[3].number = 16U;

    struct mesh_admin_request write;
    if (mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
        write.kind != MESH_ADMIN_SET_CHANNEL || write.type != 1U ||
        write.payload.channel.index != 1 ||
        write.payload.channel.role != meshtastic_Channel_Role_DISABLED ||
        strcmp(write.payload.channel.settings.name, "Hikers") != 0 ||
        write.payload.channel.settings.psk.size != 32U ||
        write.payload.channel.settings.id != 77U ||
        !write.payload.channel.settings.has_module_settings ||
        write.payload.channel.settings.module_settings.position_precision != 16U) {
        record_failure(test_name, "the channel write should carry the edits over the radio's copy");
        return;
    }
    bool all_zero = true;
    for (unsigned i = 0; i < 32U; ++i) {
        if (write.payload.channel.settings.psk.bytes[i] != 0U) {
            all_zero = false;
        }
    }
    if (all_zero) {
        record_failure(test_name, "a random key should not be all zeroes");
        return;
    }
    action.edit_count = 1U;
    action.edits[0].number = MESH_UI_PSK_TYPED;
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s",
             "d4f1bb3a20290759f0bcffabcf4e6901");
    if (mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
        write.payload.channel.settings.psk.size != 16U ||
        write.payload.channel.settings.psk.bytes[0] != 0xD4U ||
        write.payload.channel.settings.psk.bytes[15] != 0x01U) {
        record_failure(test_name, "a typed key should be parsed as hex");
        return;
    }
    action.edits[0].number = MESH_UI_PSK_NONE;
    if (mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
        write.payload.channel.settings.psk.size != 0U) {
        record_failure(test_name, "no encryption is an empty key");
        return;
    }
    action.channel = 3U;
    if (mesh_app_build_settings_write(&radio, &action, &write) != -ENOENT) {
        record_failure(test_name, "a slot the radio never sent cannot be written");
        return;
    }

    action.section = MESH_UI_SETTINGS_BLUETOOTH;
    action.channel = MESH_UI_SETTINGS_NO_CHANNEL;
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_BT_MODE;
    action.edits[0].number = 1U;
    action.edits[1].field = MESH_UI_FIELD_BT_PIN;
    snprintf(action.edits[1].text, sizeof action.edits[1].text, "%s", "123456");
    if (mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
        write.kind != MESH_ADMIN_SET_CONFIG ||
        write.type != meshtastic_AdminMessage_ConfigType_BLUETOOTH_CONFIG ||
        write.payload.config.which_payload_variant != meshtastic_Config_bluetooth_tag ||
        write.payload.config.payload_variant.bluetooth.mode !=
            meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN ||
        write.payload.config.payload_variant.bluetooth.fixed_pin != 123456U ||
        !write.payload.config.payload_variant.bluetooth.enabled) {
        record_failure(test_name, "the Bluetooth write is wrong");
        return;
    }
    snprintf(action.edits[1].text, sizeof action.edits[1].text, "%s", "12ab56");
    if (mesh_app_build_settings_write(&radio, &action, &write) != -EINVAL) {
        record_failure(test_name, "a PIN that is not six digits must be refused");
        return;
    }
    record_success(test_name);
}

/* Keys as text: base64 out, base64 or hex in, per-field lengths and choices. */
static void test_ui_settings_key_text(void) {
    const char *test_name = "ui_settings_key_text";
    static const uint8_t k_default[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                          0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};
    char text[64];
    mesh_ui_settings_key_text(k_default, sizeof k_default, text, sizeof text);
    if (strcmp(text, "1PG7OiApB1nwvP+rz05pAQ==") != 0) {
        record_failure(test_name, "base64 of the default key is wrong");
        return;
    }
    uint8_t parsed[32];
    size_t len = 0U;
    if (!mesh_ui_settings_key_parse("1PG7OiApB1nwvP+rz05pAQ==", parsed, sizeof parsed, &len) ||
        len != 16U || memcmp(parsed, k_default, 16U) != 0) {
        record_failure(test_name, "base64 should parse back");
        return;
    }
    if (!mesh_ui_settings_key_parse("d4f1bb3a20290759f0bcffabcf4e6901", parsed, sizeof parsed,
                                    &len) ||
        len != 16U || memcmp(parsed, k_default, 16U) != 0) {
        record_failure(test_name, "hex should parse too");
        return;
    }
    if (!mesh_ui_settings_key_parse("AQ==", parsed, sizeof parsed, &len) || len != 1U ||
        parsed[0] != 1U || !mesh_ui_settings_key_parse("", parsed, sizeof parsed, &len) ||
        len != 0U) {
        record_failure(test_name, "one-byte and empty keys should parse");
        return;
    }
    if (mesh_ui_settings_key_parse("1PG7OiApB1nwvP+rz05pAQ=", parsed, sizeof parsed, &len) ||
        mesh_ui_settings_key_parse("1PG7Oi=pB1nwvP+rz05pAQ==", parsed, sizeof parsed, &len) ||
        mesh_ui_settings_key_parse("not a key!", parsed, sizeof parsed, &len) ||
        mesh_ui_settings_key_parse("abc", parsed, sizeof parsed, &len)) {
        record_failure(test_name, "bad text must be refused");
        return;
    }
    uint8_t all[32];
    for (unsigned i = 0; i < 32U; ++i) {
        all[i] = (uint8_t)(i * 7U);
    }
    mesh_ui_settings_key_text(all, 32U, text, sizeof text);
    if (strlen(text) != 44U || !mesh_ui_settings_key_parse(text, parsed, sizeof parsed, &len) ||
        len != 32U || memcmp(parsed, all, 32U) != 0) {
        record_failure(test_name, "a 32-byte key should round-trip");
        return;
    }
    if (!mesh_ui_settings_key_len_ok(MESH_UI_FIELD_CHANNEL_KEY, 1U) ||
        mesh_ui_settings_key_len_ok(MESH_UI_FIELD_CHANNEL_KEY, 8U) ||
        !mesh_ui_settings_key_len_ok(MESH_UI_FIELD_SECURITY_PRIVATE_KEY, 32U) ||
        mesh_ui_settings_key_len_ok(MESH_UI_FIELD_SECURITY_PRIVATE_KEY, 16U) ||
        !mesh_ui_settings_key_len_ok(MESH_UI_FIELD_SECURITY_ADMIN_KEY_1, 0U) ||
        mesh_ui_settings_key_len_ok(MESH_UI_FIELD_DISPLAY_FLIP, 0U)) {
        record_failure(test_name, "key length rules are wrong");
        return;
    }
    if (mesh_ui_settings_key_choices(MESH_UI_FIELD_SECURITY_PRIVATE_KEY) !=
            (MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_KEEP) |
             MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_RANDOM_256)) ||
        (mesh_ui_settings_key_choices(MESH_UI_FIELD_CHANNEL_KEY) &
         MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_DEFAULT)) == 0U ||
        mesh_ui_settings_key_choices(MESH_UI_FIELD_LORA_HOPS) != 0U) {
        record_failure(test_name, "key choices are wrong");
        return;
    }
    record_success(test_name);
}

/* LoRa and Security rows, and the writes built from them. */
static void test_app_lora_security_write_build(void) {
    const char *test_name = "app_lora_security_write_build";
    struct mesh_radio_settings radio;
    mesh_radio_settings_reset(&radio);
    radio.has_lora = true;
    radio.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    radio.lora.use_preset = true;
    radio.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    radio.lora.hop_limit = 3U;
    radio.lora.tx_enabled = true;
    radio.lora.frequency_offset = 1.5f;
    radio.has_security = true;
    radio.security.private_key.size = 32U;
    memset(radio.security.private_key.bytes, 0x11, 32U);
    radio.security.public_key.size = 32U;
    memset(radio.security.public_key.bytes, 0x22, 32U);
    radio.security.admin_key_count = 2U;
    radio.security.admin_key[0].size = 32U;
    memset(radio.security.admin_key[0].bytes, 0x33, 32U);
    radio.security.admin_key[1].size = 32U;
    memset(radio.security.admin_key[1].bytes, 0x44, 32U);

    /* The flattened view carries the keys for the rows. */
    struct mesh_ui_settings settings;
    struct mesh_ui_action probe;
    memset(&probe, 0, sizeof probe);
    struct mesh_ui_snapshot *unused = NULL;
    (void)unused;
    (void)probe;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_lora = true;
    settings.region = 1U;
    settings.use_preset = true;
    settings.tx_power = 0;
    settings.has_security = true;
    settings.private_key_len = 32U;
    memset(settings.private_key, 0x11, 32U);
    settings.admin_key_count = 2U;
    settings.admin_key_lens[0] = 32U;
    memset(settings.admin_keys[0], 0x33, 32U);
    settings.admin_key_lens[1] = 32U;
    struct mesh_ui_settings_item item;
    if (mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_LORA,
                                    MESH_UI_SETTINGS_NO_CHANNEL) != 11U ||
        !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                               MESH_UI_SETTINGS_NO_CHANNEL, 0U, &item) ||
        item.field != MESH_UI_FIELD_LORA_REGION || strcmp(item.value, "US") != 0 ||
        mesh_ui_settings_enum_count(MESH_UI_FIELD_LORA_REGION) != 38U ||
        strcmp(mesh_ui_settings_enum_name(MESH_UI_FIELD_LORA_REGION, 37U), "ITU2 1.25m") != 0 ||
        !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                               MESH_UI_SETTINGS_NO_CHANNEL, 8U, &item) ||
        item.field != MESH_UI_FIELD_LORA_TX_POWER || strcmp(item.value, "max") != 0 ||
        mesh_ui_settings_number_step(MESH_UI_FIELD_LORA_TX_POWER, 0U, +1) != 2U ||
        !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_LORA,
                               MESH_UI_SETTINGS_NO_CHANNEL, 5U, &item) ||
        strcmp(item.value, "4/0") != 0) {
        record_failure(test_name, "LoRa rows are wrong");
        return;
    }
    if (mesh_ui_settings_item_count(&settings, NULL, MESH_UI_SETTINGS_SECURITY,
                                    MESH_UI_SETTINGS_NO_CHANNEL) != 10U ||
        !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_SECURITY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 1U, &item) ||
        item.field != MESH_UI_FIELD_SECURITY_PRIVATE_KEY || item.kind != MESH_UI_SETTING_KEY ||
        strlen(item.text) != 44U || strstr(item.value, "256-bit") == NULL ||
        !mesh_ui_settings_item(&settings, NULL, NULL, 0U, MESH_UI_SETTINGS_SECURITY,
                               MESH_UI_SETTINGS_NO_CHANNEL, 4U, &item) ||
        item.field != MESH_UI_FIELD_SECURITY_ADMIN_KEY_2 || strcmp(item.value, "none") != 0 ||
        !mesh_ui_settings_section_needs_confirm(MESH_UI_SETTINGS_LORA) ||
        !mesh_ui_settings_section_needs_confirm(MESH_UI_SETTINGS_SECURITY)) {
        record_failure(test_name, "Security rows are wrong");
        return;
    }

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_SAVE_SETTINGS;
    action.section = MESH_UI_SETTINGS_LORA;
    action.channel = MESH_UI_SETTINGS_NO_CHANNEL;
    action.edit_count = 3U;
    action.edits[0].field = MESH_UI_FIELD_LORA_REGION;
    action.edits[0].number = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
    action.edits[1].field = MESH_UI_FIELD_LORA_HOPS;
    action.edits[1].number = 5U;
    action.edits[2].field = MESH_UI_FIELD_LORA_TX_POWER;
    action.edits[2].number = 20U;
    struct mesh_admin_request write;
    if (mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
        write.kind != MESH_ADMIN_SET_CONFIG ||
        write.type != meshtastic_AdminMessage_ConfigType_LORA_CONFIG ||
        write.payload.config.which_payload_variant != meshtastic_Config_lora_tag ||
        write.payload.config.payload_variant.lora.region !=
            meshtastic_Config_LoRaConfig_RegionCode_EU_868 ||
        write.payload.config.payload_variant.lora.hop_limit != 5U ||
        write.payload.config.payload_variant.lora.tx_power != 20 ||
        !write.payload.config.payload_variant.lora.use_preset ||
        write.payload.config.payload_variant.lora.frequency_offset != 1.5f) {
        record_failure(test_name, "the LoRa write should carry the edits over the radio's copy");
        return;
    }

    /* Security: a new private key is clamped and the public key cleared for the firmware to
       derive; clearing admin key 1 compacts the list. */
    action.section = MESH_UI_SETTINGS_SECURITY;
    action.edit_count = 3U;
    action.edits[0].field = MESH_UI_FIELD_SECURITY_PRIVATE_KEY;
    action.edits[0].number = MESH_UI_PSK_RANDOM_256;
    action.edits[1].field = MESH_UI_FIELD_SECURITY_ADMIN_KEY_0;
    action.edits[1].number = MESH_UI_PSK_NONE;
    action.edits[2].field = MESH_UI_FIELD_SECURITY_MANAGED;
    action.edits[2].number = 1U;
    if (mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
        write.type != meshtastic_AdminMessage_ConfigType_SECURITY_CONFIG ||
        write.payload.config.which_payload_variant != meshtastic_Config_security_tag) {
        record_failure(test_name, "the Security write should build");
        return;
    }
    const meshtastic_Config_SecurityConfig *sec = &write.payload.config.payload_variant.security;
    if (sec->private_key.size != 32U || (sec->private_key.bytes[0] & 7U) != 0U ||
        (sec->private_key.bytes[31] & 0x80U) != 0U || (sec->private_key.bytes[31] & 0x40U) == 0U ||
        memcmp(sec->private_key.bytes, radio.security.private_key.bytes, 32U) == 0 ||
        sec->public_key.size != 0U || !sec->is_managed) {
        record_failure(test_name, "a new private key should be clamped and the public key cleared");
        return;
    }
    if (sec->admin_key_count != 1U || sec->admin_key[0].size != 32U ||
        sec->admin_key[0].bytes[0] != 0x44U || sec->admin_key[1].size != 0U) {
        record_failure(test_name, "clearing an admin key should compact the list");
        return;
    }
    /* Restoring a backed-up private key and adding an admin key by text. */
    action.edit_count = 2U;
    action.edits[0].field = MESH_UI_FIELD_SECURITY_PRIVATE_KEY;
    action.edits[0].number = MESH_UI_PSK_TYPED;
    uint8_t restore[32];
    memset(restore, 0x5A, 32U);
    mesh_ui_settings_key_text(restore, 32U, action.edits[0].text, sizeof action.edits[0].text);
    action.edits[1].field = MESH_UI_FIELD_SECURITY_ADMIN_KEY_2;
    action.edits[1].number = MESH_UI_PSK_TYPED;
    memset(restore, 0x66, 32U);
    mesh_ui_settings_key_text(restore, 32U, action.edits[1].text, sizeof action.edits[1].text);
    if (mesh_app_build_settings_write(&radio, &action, &write) != 0 ||
        sec->private_key.bytes[0] != 0x5AU || sec->private_key.size != 32U ||
        sec->public_key.size != 0U || sec->admin_key_count != 3U ||
        sec->admin_key[2].bytes[0] != 0x66U) {
        record_failure(test_name, "typed keys should be restored as given");
        return;
    }
    snprintf(action.edits[0].text, sizeof action.edits[0].text, "%s", "AQ=="); /* 1 byte */
    if (mesh_app_build_settings_write(&radio, &action, &write) != -EINVAL) {
        record_failure(test_name, "a private key must be 32 bytes");
        return;
    }
    record_success(test_name);
}

/* ---- serial transport ------------------------------------------------------------------------ */

static void test_stream_frame_encode(void) {
    const char *test_name = "stream_frame_encode";
    const uint8_t payload[] = {0x08U, 0x96U, 0x01U};
    uint8_t frame[16];
    size_t written = 0U;

    if (mesh_stream_frame_encode(payload, sizeof payload, frame, sizeof frame, &written) != 0) {
        record_failure(test_name, "encode failed");
        return;
    }
    if (written != MESH_STREAM_FRAME_HEADER_LEN + sizeof payload) {
        record_failure(test_name, "unexpected frame length");
        return;
    }
    if (frame[0] != MESH_STREAM_FRAME_START1 || frame[1] != MESH_STREAM_FRAME_START2 ||
        frame[2] != 0x00U || frame[3] != (uint8_t)sizeof payload) {
        record_failure(test_name, "header is not 0x94 0xC3 with a big-endian length");
        return;
    }
    if (memcmp(frame + MESH_STREAM_FRAME_HEADER_LEN, payload, sizeof payload) != 0) {
        record_failure(test_name, "payload was not copied verbatim");
        return;
    }

    uint8_t big[MESH_STREAM_FRAME_MAX_PAYLOAD + 1U];
    memset(big, 0, sizeof big);
    uint8_t sink[MESH_STREAM_FRAME_HEADER_LEN + sizeof big];
    if (mesh_stream_frame_encode(big, sizeof big, sink, sizeof sink, &written) != -EMSGSIZE) {
        record_failure(test_name, "oversized payload should be rejected");
        return;
    }
    if (mesh_stream_frame_encode(payload, sizeof payload, frame, 4U, &written) != -ENOSPC) {
        record_failure(test_name, "a short output buffer should return -ENOSPC");
        return;
    }

    record_success(test_name);
}

struct stream_capture {
    size_t frame_count;
    size_t frame_len[8];
    uint8_t frames[8][32];
    size_t text_bytes;
};

static void stream_capture_frame(const uint8_t *payload, size_t len, void *ctx) {
    struct stream_capture *capture = (struct stream_capture *)ctx;
    if (capture->frame_count >= 8U || len > 32U) {
        return;
    }
    memcpy(capture->frames[capture->frame_count], payload, len);
    capture->frame_len[capture->frame_count] = len;
    capture->frame_count += 1U;
}

static void stream_capture_text(const uint8_t *text, size_t len, void *ctx) {
    (void)text;
    ((struct stream_capture *)ctx)->text_bytes += len;
}

/*
 * The radio interleaves its own log with the frames on the same port, and a reader gets
 * arbitrary chunk boundaries. The parser has to skip the log, resync on a header that turns out
 * to be log text, and hold a frame that arrives in pieces.
 */
static void test_stream_parser_resync(void) {
    const char *test_name = "stream_parser_resync";

    struct mesh_stream_parser parser;
    mesh_stream_parser_reset(&parser);
    struct stream_capture capture;
    memset(&capture, 0, sizeof capture);
    const struct mesh_stream_parser_callbacks callbacks = {
        .on_frame = stream_capture_frame,
        .on_text = stream_capture_text,
        .ctx = &capture,
    };

    const uint8_t log_line[] = {'I', 'N', 'F', 'O', ' ', 'b', 'o', 'o', 't', '\n'};
    mesh_stream_parser_push(&parser, log_line, sizeof log_line, &callbacks);
    if (capture.frame_count != 0U || capture.text_bytes != sizeof log_line) {
        record_failure(test_name, "log text should be reported as text, not frames");
        return;
    }

    /* A 0x94 0xC3 inside a log line, with a length no frame could have. */
    const uint8_t false_start[] = {MESH_STREAM_FRAME_START1, MESH_STREAM_FRAME_START2, 0xFFU, 0xFFU,
                                   'x'};
    mesh_stream_parser_push(&parser, false_start, sizeof false_start, &callbacks);
    if (capture.frame_count != 0U) {
        record_failure(test_name, "an impossible length should not produce a frame");
        return;
    }

    const uint8_t payload_a[] = {0x01U, 0x02U, 0x03U};
    const uint8_t payload_b[] = {0x0AU, 0x0BU};
    uint8_t stream[64];
    size_t total = 0U;
    size_t written = 0U;
    mesh_stream_frame_encode(payload_a, sizeof payload_a, stream, sizeof stream, &written);
    total += written;
    /* Back-to-back frames with a scrap of log between them. */
    stream[total++] = '.';
    mesh_stream_frame_encode(payload_b, sizeof payload_b, stream + total, sizeof stream - total,
                             &written);
    total += written;

    /* One byte at a time: every split lands mid-header and mid-payload at some point. */
    for (size_t i = 0; i < total; ++i) {
        mesh_stream_parser_push(&parser, &stream[i], 1U, &callbacks);
    }

    if (capture.frame_count != 2U) {
        record_failure(test_name, "expected both frames to survive a byte-at-a-time feed");
        return;
    }
    if (capture.frame_len[0] != sizeof payload_a ||
        memcmp(capture.frames[0], payload_a, sizeof payload_a) != 0 ||
        capture.frame_len[1] != sizeof payload_b ||
        memcmp(capture.frames[1], payload_b, sizeof payload_b) != 0) {
        record_failure(test_name, "frame payloads did not round-trip");
        return;
    }
    if (parser.frames != 2U || parser.dropped_bytes == 0U) {
        record_failure(test_name, "parser counters did not track frames and junk");
        return;
    }

    record_success(test_name);
}

/* One unbound native-USB node, as the Brick sees a Heltec nRF52840 before we bind it. */
static struct mesh_serial_device_info serial_test_device(void) {
    struct mesh_serial_device_info device;
    memset(&device, 0, sizeof device);
    snprintf(device.id, sizeof device.id, "%s", "1-1:1.1");
    snprintf(device.name, sizeof device.name, "%s", "Heltec Mesh Node");
    device.vendor_id = 0x239AU;
    device.product_id = 0x4405U;
    device.busnum = 1U;
    device.devnum = 3U;
    device.control_interface = 0;
    device.bound = false;
    device.needs_line_state = true;
    return device;
}

/* Reads whatever the radio end of the socketpair has, with a short grace period. */
static ssize_t serial_test_read(int fd, uint8_t *out, size_t cap) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        const ssize_t got = read(fd, out, cap);
        if (got >= 0) {
            return got;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        struct timespec ts = {0, 10 * 1000 * 1000};
        (void)nanosleep(&ts, NULL);
    }
    return 0;
}

static void serial_test_sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000 * 1000};
    (void)nanosleep(&ts, NULL);
}

/*
 * The whole serial connect, against a socketpair standing in for the tty: the Brick's bind and
 * usbfs DTR each happen once, the radio gets the resync burst, the handshake goes out framed,
 * and a framed FromRadio comes back into the session.
 */
static void test_serial_transport_connect_mock(void) {
    const char *test_name = "serial_transport_connect_mock";

    int pair[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        record_failure(test_name, "socketpair failed");
        return;
    }
    (void)fcntl(pair[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(pair[1], F_SETFL, O_NONBLOCK);

    const struct mesh_serial_device_info devices[] = {serial_test_device()};
    struct mesh_serial_usb_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.devices = devices;
    mock.device_count = 1U;
    mock.bound_path = "/dev/ttyUSB0";
    mock.open_fd = pair[0];
    mesh_serial_usb_mock_enable(&mock);

    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        goto cleanup;
    }

    struct mesh_transport *transport = mesh_serial_transport();
    struct mesh_app_config config = mesh_app_config_default();
    if (transport->ops->start(transport, &config, &loop) != 0) {
        record_failure(test_name, "serial start failed");
        goto cleanup_loop;
    }
    if (strcmp(transport->ops->status(transport), "running") != 0) {
        record_failure(test_name, "status should be running once a port is found");
        goto cleanup_transport;
    }

    if (mesh_serial_transport_connect(transport, "1-1:1.1") != 0) {
        record_failure(test_name, "connect failed");
        goto cleanup_transport;
    }
    if (mesh_serial_usb_mock_bind_calls() != 1U || mesh_serial_usb_mock_line_state_calls() != 1U) {
        record_failure(test_name, "connect should bind the port and assert DTR exactly once");
        goto cleanup_transport;
    }
    if (!mesh_serial_transport_is_connecting(transport) ||
        mesh_serial_transport_connected_port(transport) != NULL) {
        record_failure(test_name, "the link should be waking, not yet connected");
        goto cleanup_transport;
    }

    uint8_t wake[64];
    ssize_t got = serial_test_read(pair[1], wake, sizeof wake);
    if (got != 32) {
        record_failure(test_name, "expected a 32-byte resync burst before the handshake");
        goto cleanup_transport;
    }
    for (ssize_t i = 0; i < got; ++i) {
        if (wake[i] != MESH_STREAM_FRAME_START2) {
            record_failure(test_name, "the resync burst should be bare START2 bytes");
            goto cleanup_transport;
        }
    }

    /* The handshake waits out the settle window, then goes out from tick(). */
    serial_test_sleep_ms(150);
    transport->ops->tick(transport);
    if (mesh_serial_transport_connected_port(transport) == NULL) {
        record_failure(test_name, "the link should be connected once the radio has woken");
        goto cleanup_transport;
    }

    uint8_t request[128];
    got = serial_test_read(pair[1], request, sizeof request);
    if (got < (ssize_t)MESH_STREAM_FRAME_HEADER_LEN || request[0] != MESH_STREAM_FRAME_START1 ||
        request[1] != MESH_STREAM_FRAME_START2) {
        record_failure(test_name, "want_config_id should go out framed");
        goto cleanup_transport;
    }
    const size_t request_len = ((size_t)request[2] << 8U) | (size_t)request[3];
    if (request_len == 0U || request_len + MESH_STREAM_FRAME_HEADER_LEN != (size_t)got) {
        record_failure(test_name, "framed length does not match what was written");
        goto cleanup_transport;
    }

    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_zero;
    pb_istream_t decode =
        pb_istream_from_buffer(request + MESH_STREAM_FRAME_HEADER_LEN, request_len);
    if (!pb_decode(&decode, meshtastic_ToRadio_fields, &to_radio) ||
        to_radio.which_payload_variant != meshtastic_ToRadio_want_config_id_tag ||
        to_radio.want_config_id == 0U) {
        record_failure(test_name, "the framed packet is not a want_config_id ToRadio");
        goto cleanup_transport;
    }

    /* The radio answers with MyNodeInfo, framed. */
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_zero;
    from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    from_radio.my_info.my_node_num = 0x433D1A2CU;
    uint8_t encoded[256];
    size_t encoded_len = 0U;
    if (!test_encode_from_radio(&from_radio, encoded, sizeof encoded, &encoded_len)) {
        record_failure(test_name, "failed to encode the reply");
        goto cleanup_transport;
    }
    uint8_t reply[300];
    size_t reply_len = 0U;
    mesh_stream_frame_encode(encoded, encoded_len, reply, sizeof reply, &reply_len);
    /* A log line ahead of the frame, and the frame itself split across two writes: both are
       what a read() off the real port hands over. */
    const uint8_t noise[] = {'D', 'E', 'B', 'U', 'G', '\n'};
    if (write(pair[1], noise, sizeof noise) != (ssize_t)sizeof noise ||
        write(pair[1], reply, 3U) != 3 ||
        write(pair[1], reply + 3U, reply_len - 3U) != (ssize_t)(reply_len - 3U)) {
        record_failure(test_name, "failed to write the reply into the port");
        goto cleanup_transport;
    }

    if (mesh_serial_transport_pump(transport) <= 0) {
        record_failure(test_name, "pump should have read the reply");
        goto cleanup_transport;
    }

    const struct mesh_handshake_status *status =
        mesh_session_handshake(mesh_serial_transport_session(transport));
    if (status == NULL || !status->has_my_info || status->my_info.my_node_num != 0x433D1A2CU) {
        record_failure(test_name, "MyNodeInfo did not reach the session");
        goto cleanup_transport;
    }

    struct mesh_serial_transport_stats stats = mesh_serial_transport_stats(transport);
    if (stats.frames_received != 1U || stats.junk_bytes == 0U) {
        record_failure(test_name, "stats should count one frame and the log bytes around it");
        goto cleanup_transport;
    }

    if (mesh_serial_transport_disconnect(transport) != 0 ||
        mesh_serial_transport_connected_port(transport) != NULL) {
        record_failure(test_name, "disconnect should drop the link");
        goto cleanup_transport;
    }

    record_success(test_name);

cleanup_transport:
    transport->ops->stop(transport);
cleanup_loop:
    mesh_event_loop_shutdown(&loop);
cleanup:
    mesh_serial_usb_mock_disable();
    if (pair[0] >= 0) {
        close(pair[0]);
    }
    if (pair[1] >= 0) {
        close(pair[1]);
    }
}

/* Unplugging the node shows up as EOF on the tty; the link has to reset rather than spin. */
static void test_serial_transport_link_drop(void) {
    const char *test_name = "serial_transport_link_drop";

    int pair[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        record_failure(test_name, "socketpair failed");
        return;
    }
    (void)fcntl(pair[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(pair[1], F_SETFL, O_NONBLOCK);

    struct mesh_serial_device_info device = serial_test_device();
    device.bound = true;
    device.needs_line_state = false;
    snprintf(device.path, sizeof device.path, "%s", "/dev/ttyUSB0");
    device.control_interface = -1;
    const struct mesh_serial_device_info devices[] = {device};

    struct mesh_serial_usb_mock_config mock;
    memset(&mock, 0, sizeof mock);
    mock.devices = devices;
    mock.device_count = 1U;
    mock.open_fd = pair[0];
    mesh_serial_usb_mock_enable(&mock);

    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "event loop init failed");
        goto cleanup;
    }

    struct mesh_transport *transport = mesh_serial_transport();
    struct mesh_app_config config = mesh_app_config_default();
    if (transport->ops->start(transport, &config, &loop) != 0) {
        record_failure(test_name, "serial start failed");
        goto cleanup_loop;
    }

    if (mesh_serial_transport_connect(transport, "/dev/ttyUSB0") != 0) {
        record_failure(test_name, "connect failed");
        goto cleanup_transport;
    }
    /* An already-bound bridge needs neither the generic driver nor the usbfs DTR path. */
    if (mesh_serial_usb_mock_bind_calls() != 0U || mesh_serial_usb_mock_line_state_calls() != 0U) {
        record_failure(test_name, "a bound port should not be rebound or poked over usbfs");
        goto cleanup_transport;
    }

    serial_test_sleep_ms(150);
    transport->ops->tick(transport);
    if (mesh_serial_transport_connected_port(transport) == NULL) {
        record_failure(test_name, "expected a connected link");
        goto cleanup_transport;
    }

    /* Drain what the transport wrote: a socketpair closed with unread data sends RST, and we
       want the clean EOF that an unplugged tty gives. */
    uint8_t drain[256];
    while (read(pair[1], drain, sizeof drain) > 0) {
    }
    close(pair[1]);
    pair[1] = -1;
    if (mesh_serial_transport_pump(transport) != -ENOTCONN) {
        record_failure(test_name, "pump should report the port gone");
        goto cleanup_transport;
    }
    if (mesh_serial_transport_connected_port(transport) != NULL ||
        strcmp(transport->ops->status(transport), "running") != 0) {
        record_failure(test_name, "the link should be reset and the transport back to scanning");
        goto cleanup_transport;
    }
    if (mesh_session_attached(mesh_serial_transport_session(transport))) {
        record_failure(test_name, "the session should have been detached");
        goto cleanup_transport;
    }

    record_success(test_name);

cleanup_transport:
    transport->ops->stop(transport);
cleanup_loop:
    mesh_event_loop_shutdown(&loop);
cleanup:
    mesh_serial_usb_mock_disable();
    if (pair[0] >= 0) {
        close(pair[0]);
    }
    if (pair[1] >= 0) {
        close(pair[1]);
    }
}

/*
 * With both links available the app must prefer the plugged-in node, route the connect to the
 * serial transport, list USB ports above BLE advertisers, and - the point of the shared session -
 * fold what the USB radio says into the app's own session rather than one buried in the link.
 */
static void test_app_link_routing(void) {
    const char *test_name = "app_link_routing";
    const char *failure = NULL;
    int pair[2] = {-1, -1};
    bool app_ready = false;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        record_failure(test_name, "socketpair failed");
        return;
    }
    (void)fcntl(pair[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(pair[1], F_SETFL, O_NONBLOCK);

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:06", .name = "NodeSix", .rssi = -30},
    };
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 1U,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    const struct mesh_serial_device_info ports[] = {serial_test_device()};
    struct mesh_serial_usb_mock_config serial_mock;
    memset(&serial_mock, 0, sizeof serial_mock);
    serial_mock.devices = ports;
    serial_mock.device_count = 1U;
    serial_mock.bound_path = "/dev/ttyUSB0";
    serial_mock.open_fd = pair[0];
    mesh_serial_usb_mock_enable(&serial_mock);

    char home_dir[] = "/tmp/mesh_app_link_routingXXXXXX";
    if (mkdtemp(home_dir) == NULL) {
        failure = "mkdtemp failed";
        goto cleanup;
    }
    setenv("HOME", home_dir, 1);
    setenv("MESHCLIENT_UI_BACKEND", "stub", 1);
    unsetenv("MESHCLIENT_AUTOCONNECT");

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;

    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_transport *serial = mesh_serial_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);
    mesh_serial_transport_refresh_devices(serial);

    /* USB wins even though a BLE advertiser is in range at a healthy RSSI. */
    mesh_app_autoconnect(&app);
    if (!mesh_serial_transport_is_connecting(serial)) {
        failure = "auto-connect should have opened the USB port first";
        goto cleanup;
    }
    if (mesh_ble_transport_connected_address(ble) != NULL ||
        mesh_ble_transport_is_connecting(ble)) {
        failure = "the BLE link should have been left alone";
        goto cleanup;
    }

    serial_test_sleep_ms(150);
    mesh_transport_registry_tick(&app.transport_registry);
    const char *connected = mesh_app_connected_identifier();
    if (connected == NULL || strcmp(connected, "/dev/ttyUSB0") != 0) {
        failure = "the app should report the tty as the connected radio";
        goto cleanup;
    }
    if (mesh_app_active_transport() != serial) {
        failure = "the serial transport should be the active link";
        goto cleanup;
    }

    /* The radio's reply has to land in the app's session, not one hidden in the transport. */
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_zero;
    from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    from_radio.my_info.my_node_num = 0x0BADCAFEU;
    uint8_t encoded[256];
    size_t encoded_len = 0U;
    if (!test_encode_from_radio(&from_radio, encoded, sizeof encoded, &encoded_len)) {
        failure = "failed to encode the reply";
        goto cleanup;
    }
    uint8_t frame[300];
    size_t frame_len = 0U;
    mesh_stream_frame_encode(encoded, encoded_len, frame, sizeof frame, &frame_len);
    if (write(pair[1], frame, frame_len) != (ssize_t)frame_len) {
        failure = "failed to write the reply into the port";
        goto cleanup;
    }
    if (mesh_serial_transport_pump(serial) <= 0) {
        failure = "pump should have read the reply";
        goto cleanup;
    }
    if (!app.session.handshake.has_my_info ||
        app.session.handshake.my_info.my_node_num != 0x0BADCAFEU) {
        failure = "the USB link should feed the app's own session";
        goto cleanup;
    }

    /* The Devices tab shows one list: USB ports first, then BLE advertisers. */
    mesh_app_publish_ui_state(&app);
    if (app.ui_store.device_count != 2U) {
        failure = "both the USB port and the BLE advertiser should be listed";
        goto cleanup;
    }
    if (app.ui_store.devices[0].kind != (uint8_t)MESH_UI_DEVICE_SERIAL ||
        strcmp(app.ui_store.devices[0].identifier, "/dev/ttyUSB0") != 0 ||
        !app.ui_store.devices[0].connected) {
        failure = "the connected USB port should be the first row";
        goto cleanup;
    }
    if (app.ui_store.devices[1].kind != (uint8_t)MESH_UI_DEVICE_BLE ||
        strcmp(app.ui_store.devices[1].identifier, "AA:BB:CC:DD:EE:06") != 0 ||
        app.ui_store.devices[1].connected) {
        failure = "the BLE advertiser should follow it, unconnected";
        goto cleanup;
    }

    /* Nothing should be reconnected while a link is up. */
    app.autoconnect_retry_at_ms = 0U;
    mesh_app_autoconnect(&app);
    if (mesh_ble_transport_is_connecting(ble) ||
        mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "auto-connect should stay put while the USB link is up";
        goto cleanup;
    }

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    mesh_serial_usb_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    if (pair[0] >= 0) {
        close(pair[0]);
    }
    if (pair[1] >= 0) {
        close(pair[1]);
    }
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/* The clock mesh_app_publish_ui_state stamps its toasts with. */
static uint64_t test_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/*
 * A BLE connect can return 0 and still fail seconds later, when BlueZ finishes service discovery
 * and StartNotify is rejected because the node was never paired. That used to leave the UI stuck
 * on "connecting" with the reason only in the log.
 */
static void test_app_connect_failure_toast(void) {
    const char *test_name = "app_connect_failure_toast";
    const char *failure = NULL;
    bool app_ready = false;
    struct mesh_app app;
    memset(&app, 0, sizeof app);

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:07", .name = "NodeSeven", .rssi = -40},
    };
    struct mesh_bluez_mock_config mock_config = {
        .adapter_path = "/org/bluez/hci0",
        .devices = mock_devices,
        .device_count = 1U,
        /* The node answers Connect and resolves services, then refuses the subscription. */
        .connect_pending_polls = 1U,
        .subscribe_result = -EACCES,
    };
    mesh_bluez_client_mock_enable(&mock_config);

    char home_dir[] = "/tmp/mesh_app_connect_failXXXXXX";
    if (mkdtemp(home_dir) == NULL) {
        failure = "mkdtemp failed";
        goto cleanup;
    }
    setenv("HOME", home_dir, 1);
    setenv("MESHCLIENT_UI_BACKEND", "stub", 1);
    unsetenv("MESHCLIENT_AUTOCONNECT");

    struct mesh_app_config config = mesh_app_config_default();
    config.run_mode = MESH_APP_RUN_FOREGROUND;
    config.enable_serial = false;

    if (mesh_app_init(&app, &config) != 0) {
        failure = "app init failed";
        goto cleanup;
    }
    app_ready = true;

    struct mesh_transport *ble = mesh_ble_transport();
    if (mesh_transport_registry_start_all(&app.transport_registry, &app.config, &app.loop) < 0) {
        failure = "transport start failed";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);

    if (app.ui_controller.on_action == NULL) {
        failure = "the app should have installed a UI action handler";
        goto cleanup;
    }

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    action.type = MESH_UI_ACTION_CONNECT;
    action.kind = (uint8_t)MESH_UI_DEVICE_BLE;
    snprintf(action.identifier, sizeof action.identifier, "%s", "AA:BB:CC:DD:EE:07");
    app.ui_controller.on_action(app.ui_controller.action_userdata, &action);

    /* Connect has only been sent; nothing has failed yet. */
    if (!mesh_ble_transport_is_connecting(ble)) {
        failure = "the connect should be in flight";
        goto cleanup;
    }
    if (strstr(app.ui_store.nav.toast, "Connecting") == NULL) {
        failure = "the user should first be told the connect is in flight";
        goto cleanup;
    }
    if (mesh_app_report_link_errors(&app)) {
        failure = "no failure should be reported while the connect is still pending";
        goto cleanup;
    }

    /* Now the reply lands, services resolve, and StartNotify is refused. */
    mesh_transport_registry_tick(&app.transport_registry);
    if (mesh_ble_transport_is_connecting(ble) ||
        mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "the link should have been dropped";
        goto cleanup;
    }

    if (!mesh_app_report_link_errors(&app)) {
        failure = "the pairing failure should have been drained";
        goto cleanup;
    }
    if (strstr(app.ui_store.nav.toast, "pairing") == NULL ||
        strstr(app.ui_store.nav.toast, "EE:07") == NULL) {
        failure = "the pairing failure should have reached the screen";
        goto cleanup;
    }

    /* One report per attempt: the same failure must not keep re-toasting every turn. */
    mesh_ui_store_set_toast(&app.ui_store, test_now_ms(), "quiet");
    if (mesh_app_report_link_errors(&app)) {
        failure = "the failure should be reported once, not on every turn";
        goto cleanup;
    }

    /*
     * A connect that returned 0 and failed later is still a failure. Nothing else tells
     * auto-connect that, so without it the backoff never grows and a node that refuses every
     * time is retried every couple of seconds forever.
     */
    if (app.autoconnect_failures == 0U) {
        failure = "the late failure should have counted against auto-connect";
        goto cleanup;
    }
    const unsigned failures_before = app.autoconnect_failures;
    const uint64_t retry_before = app.autoconnect_retry_at_ms;

    /* Auto-connect retries the same doomed node on every backoff, so its failures stay in the
       log; only a connect the user asked for is worth interrupting them for. */
    app.autoconnect_retry_at_ms = 0U;
    snprintf(app.config.preferred_ble_device, sizeof app.config.preferred_ble_device, "%s",
             "AA:BB:CC:DD:EE:07");
    mesh_app_autoconnect(&app);
    mesh_transport_registry_tick(&app.transport_registry);
    if (!mesh_app_report_link_errors(&app)) {
        failure = "the auto-connect failure should still have been drained";
        goto cleanup;
    }
    if (strcmp(app.ui_store.nav.toast, "quiet") != 0) {
        failure = "an auto-connect failure should not raise a toast";
        goto cleanup;
    }
    if (app.autoconnect_failures <= failures_before) {
        failure = "each failed attempt should push the backoff out further";
        goto cleanup;
    }
    (void)retry_before;

cleanup:
    if (app_ready) {
        mesh_app_shutdown(&app);
    }
    mesh_bluez_client_mock_disable();
    unsetenv("MESHCLIENT_UI_BACKEND");
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

static const struct test_case k_test_cases[] = {
    {"config_defaults", "unit", test_config_defaults},
    {"transport_registry_registration", "unit", test_transport_registry_registration},
    {"event_loop_init_shutdown", "unit", test_event_loop_init_shutdown},
    {"ble_transport_status_transitions", "unit", test_ble_transport_status_transitions},
    {"ble_transport_discovery_mock", "unit", test_ble_transport_discovery_mock},
    {"ble_transport_connect_mock", "unit", test_ble_transport_connect_mock},
    {"ble_transport_connect_deferred_services", "unit",
     test_ble_transport_connect_deferred_services},
    {"ble_transport_connect_async_reply", "unit", test_ble_transport_connect_async_reply},
    {"app_autoconnect_policy", "unit", test_app_autoconnect_policy},
    {"app_connect_failure_toast", "unit", test_app_connect_failure_toast},
    {"ui_store_basic", "unit", test_ui_store_basic},
    {"ui_store_persistence", "unit", test_ui_store_persistence},
    {"ui_store_refresh_request", "unit", test_ui_store_refresh_request},
    {"ui_input_quit_keys", "unit", test_ui_input_quit_keys},
    {"ui_cli_transport_update", "unit", test_ui_cli_transport_update},
    {"ui_controller_dispatch", "unit", test_ui_controller_dispatch},
    {"ui_controller_key_dispatch", "unit", test_ui_controller_key_dispatch},
    {"ui_nav_navigation", "unit", test_ui_nav_navigation},
    {"ui_nav_channels_and_keyboard", "unit", test_ui_nav_channels_and_keyboard},
    {"ble_transport_channel_decode", "unit", test_ble_transport_channel_decode},
    {"ble_transport_link_drop", "unit", test_ble_transport_link_drop},
    {"ble_transport_write_failure", "unit", test_ble_transport_write_failure},
    {"ble_transport_packet_touches_node", "unit", test_ble_transport_packet_touches_node},
    {"ui_canned_load", "unit", test_ui_canned_load},
    {"ui_input_key_mapping", "unit", test_ui_input_key_mapping},
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
    {"radio_settings_admin_roundtrip", "unit", test_radio_settings_admin_roundtrip},
    {"radio_settings_fetch_queue", "unit", test_radio_settings_fetch_queue},
    {"ui_settings_items", "unit", test_ui_settings_items},
    {"ui_nav_settings", "unit", test_ui_nav_settings},
    {"ble_transport_admin_probe", "unit", test_ble_transport_admin_probe},
    {"radio_settings_write_queue", "unit", test_radio_settings_write_queue},
    {"ui_settings_edits", "unit", test_ui_settings_edits},
    {"ui_nav_settings_edit", "unit", test_ui_nav_settings_edit},
    {"app_settings_write_build", "unit", test_app_settings_write_build},
    {"ble_transport_settings_write", "unit", test_ble_transport_settings_write},
    {"radio_settings_channel_write", "unit", test_radio_settings_channel_write},
    {"ui_nav_channel_edit", "unit", test_ui_nav_channel_edit},
    {"app_channel_write_build", "unit", test_app_channel_write_build},
    {"ui_settings_key_text", "unit", test_ui_settings_key_text},
    {"app_lora_security_write_build", "unit", test_app_lora_security_write_build},
    {"stream_frame_encode", "unit", test_stream_frame_encode},
    {"stream_parser_resync", "unit", test_stream_parser_resync},
    {"serial_transport_connect_mock", "unit", test_serial_transport_connect_mock},
    {"serial_transport_link_drop", "unit", test_serial_transport_link_drop},
    {"app_link_routing", "unit", test_app_link_routing},
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
