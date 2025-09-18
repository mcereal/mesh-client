#include "mesh/config.h"
#include "mesh/event_loop.h"
#include "mesh/proto/framing.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/transport.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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
    test_proto_varint_roundtrip();
    test_proto_frame_encode_decode();

    if (g_failures > 0) {
        fprintf(stderr, "Tests failed: %d\n", g_failures);
        return 1;
    }

    printf("All tests passed\n");
    return 0;
}
