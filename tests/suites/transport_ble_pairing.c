#define _POSIX_C_SOURCE 200809L

/* Bonding a radio that asks for a PIN, and abandoning one that is refused. */

#include "framework/mesh_test.h"

#include "mesh/core/app.h"
#include "mesh/core/config.h"
#include "mesh/core/event_loop.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/ble_bluez.h"
#include "mesh/transport/transport.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * The whole PIN-mode path with no bus: an unpaired node connects by pairing first, the agent's
 * question reaches the caller, the digits go back to BlueZ, and the connect follows on its own.
 */
MESH_TEST_CASE(ble_transport_pair_then_connect, unit) {
    struct mesh_transport *ble = mesh_ble_transport();

    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:0C", .name = "NodePin", .rssi = -55, .paired = false},
    };

    uint32_t submitted_passkey = 0U;
    uint8_t write_capture[64];
    memset(write_capture, 0, sizeof(write_capture));
    size_t write_len = 0U;
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
        .pair_result = 0,
        .pair_requests_passkey = true,
        .pair_passkey_capture = &submitted_passkey,
        .subscribe_result = 0,
        .write_result = 0,
        .toradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0C/service000a/char000b",
        .fromradio_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0C/service000a/char000d",
        .fromnum_char_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_0C/service000a/char000f",
        .read_index = &read_index,
        .devices = mock_devices,
        .device_count = sizeof(mock_devices) / sizeof(mock_devices[0]),
        .list_result = 0,
        .write_capture_buffer = write_capture,
        .write_capture_capacity = sizeof(write_capture),
        .write_capture_length = &write_len,
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

#define PAIR_TEST_FAIL(reason)                                                                     \
    do {                                                                                           \
        ble->ops->stop(ble);                                                                       \
        mesh_event_loop_shutdown(&loop);                                                           \
        mesh_bluez_client_mock_disable();                                                          \
        record_failure(test_name, (reason));                                                       \
        return;                                                                                    \
    } while (0)

    /*
     * Auto-connect bonds too - leaving it to StartNotify deadlocks, since that is a blocking
     * call and BlueZ answers it by asking our agent - but it is unattended: a node that wants
     * a PIN is refused rather than prompting over whatever the user was doing.
     */
    (void)mesh_ble_transport_connect(ble, mock_devices[0].address);
    ble->ops->tick(ble);
    if (mesh_ble_transport_pairing_request(ble, NULL)) {
        PAIR_TEST_FAIL("an automatic connect must not raise a PIN prompt");
    }
    if (mesh_ble_transport_is_pairing(ble) || mesh_ble_transport_connected_address(ble) != NULL) {
        PAIR_TEST_FAIL("a refused bond should leave nothing up");
    }
    if (submitted_passkey != 0U) {
        PAIR_TEST_FAIL("no PIN should have been sent");
    }
    /* And it does not try again on a timer: every attempt is a failed pairing at the node. */
    if (mesh_ble_transport_connect(ble, mock_devices[0].address) != -EACCES) {
        PAIR_TEST_FAIL("auto-connect should stop bonding a node that wants a PIN");
    }

    /* A connect the user asked for bonds first rather than failing on StartNotify later. */
    if (mesh_ble_transport_connect_and_pair(ble, mock_devices[0].address) != 0) {
        PAIR_TEST_FAIL("connect should start the pairing");
    }
    if (!mesh_ble_transport_is_pairing(ble)) {
        PAIR_TEST_FAIL("the link should be pairing");
    }
    if (strcmp(ble->ops->status(ble), "pairing") != 0) {
        PAIR_TEST_FAIL("status should report pairing");
    }
    if (mesh_ble_transport_connected_address(ble) != NULL) {
        PAIR_TEST_FAIL("nothing is connected while pairing");
    }

    if (!mesh_app_link_connecting()) {
        PAIR_TEST_FAIL("a bond in flight has to count as a link coming up");
    }

    struct mesh_ble_pairing_request request;
    if (!mesh_ble_transport_pairing_request(ble, &request)) {
        PAIR_TEST_FAIL("the agent should be waiting for a PIN");
    }
    if (request.kind != (uint8_t)MESH_BLUEZ_AGENT_REQUEST_PASSKEY ||
        strcmp(request.address, mock_devices[0].address) != 0) {
        PAIR_TEST_FAIL("the request should name the node it is bonding");
    }

    /* Ticking with the prompt up must not time the pairing out or complete it behind the user. */
    ble->ops->tick(ble);
    if (!mesh_ble_transport_is_pairing(ble)) {
        PAIR_TEST_FAIL("pairing should wait for the PIN");
    }

    if (mesh_ble_transport_submit_passkey(ble, 632090U) != 0) {
        PAIR_TEST_FAIL("submitting the PIN should be accepted");
    }
    if (submitted_passkey != 632090U) {
        PAIR_TEST_FAIL("the PIN did not reach BlueZ");
    }
    if (mesh_ble_transport_pairing_request(ble, &request)) {
        PAIR_TEST_FAIL("the request should be gone once answered");
    }

    /* The pair completing carries straight on into the connect the user actually asked for. */
    const char *connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, mock_devices[0].address) != 0) {
        PAIR_TEST_FAIL("the connect should follow the pairing");
    }
    if (write_len == 0U) {
        PAIR_TEST_FAIL("expected the want_config handshake write");
    }

    /* And a second connect to a node BlueZ now holds a bond for pairs nothing. */
    mesh_ble_transport_disconnect(ble);
    if (mesh_ble_transport_connect_and_pair(ble, mock_devices[0].address) != 0 ||
        mesh_ble_transport_is_pairing(ble)) {
        PAIR_TEST_FAIL("a bonded node should connect without pairing again");
    }

#undef PAIR_TEST_FAIL

    ble->ops->stop(ble);
    mesh_event_loop_shutdown(&loop);
    mesh_bluez_client_mock_disable();
    record_success(test_name);
}

/* A cancelled prompt abandons the bond instead of leaving the link half up. */
MESH_TEST_CASE(ble_transport_pair_cancel, unit) {
    struct mesh_transport *ble = mesh_ble_transport();
    struct mesh_bluez_device_info mock_devices[] = {
        {.address = "AA:BB:CC:DD:EE:0D", .name = "NodeCancel", .rssi = -55, .paired = false},
    };
    struct mesh_bluez_mock_config mock_config = {
        .init_result = 0,
        .check_ready_result = 0,
        .find_adapter_result = 0,
        .adapter_path = "/org/bluez/hci0",
        .connect_result = 0,
        .pair_result = 0,
        .pair_requests_passkey = true,
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
    (void)mesh_ble_transport_connect_and_pair(ble, mock_devices[0].address);

    const char *failure = NULL;
    if (!mesh_ble_transport_is_pairing(ble)) {
        failure = "the link should be pairing";
    } else if (mesh_ble_transport_cancel_pairing(ble) != 0) {
        failure = "cancel should be accepted";
    } else if (mesh_ble_transport_is_pairing(ble)) {
        failure = "cancel should end the pairing";
    } else if (mesh_ble_transport_pairing_request(ble, NULL)) {
        failure = "cancel should drop the agent request";
    } else if (mesh_ble_transport_connected_address(ble) != NULL) {
        failure = "a cancelled pairing must not leave a link up";
    }

    ble->ops->stop(ble);
    mesh_event_loop_shutdown(&loop);
    mesh_bluez_client_mock_disable();
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}
