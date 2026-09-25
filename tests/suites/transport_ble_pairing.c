#define _POSIX_C_SOURCE 200809L

/* Bonding a radio that asks for a PIN, and abandoning one that is refused. */

#include "framework/mesh_test.h"
#include "support/ble_fixture.h"

#include "mesh/app/app.h"
#include "mesh/transport/ble.h"

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
    uint32_t submitted_passkey = 0U;

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:0C", "NodePin", -55);
    rig.devices[0].paired = false;
    rig.mock.pair_requests_passkey = true;
    rig.mock.pair_passkey_capture = &submitted_passkey;

    struct mesh_transport *const ble = rig.ble;
    if (mesh_test_ble_rig_start(&rig) != 0) {
        mesh_test_ble_rig_close(&rig);
        record_failure(test_name, "ble start failed");
        return;
    }
    mesh_ble_transport_refresh_devices(ble);

#define PAIR_TEST_FAIL(reason)                                                                     \
    do {                                                                                           \
        mesh_test_ble_rig_close(&rig);                                                             \
        record_failure(test_name, (reason));                                                       \
        return;                                                                                    \
    } while (0)

    /*
     * Auto-connect bonds too - leaving it to StartNotify deadlocks, since that is a blocking
     * call and BlueZ answers it by asking our agent - but it is unattended: a node that wants
     * a PIN is refused rather than prompting over whatever the user was doing.
     */
    (void)mesh_ble_transport_connect(ble, rig.devices[0].address);
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
    if (mesh_ble_transport_connect(ble, rig.devices[0].address) != -EACCES) {
        PAIR_TEST_FAIL("auto-connect should stop bonding a node that wants a PIN");
    }

    /* A connect the user asked for bonds first rather than failing on StartNotify later. */
    if (mesh_ble_transport_connect_and_pair(ble, rig.devices[0].address) != 0) {
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
    if (request.kind != (uint8_t)INKWELL_BLE_AGENT_REQUEST_PASSKEY ||
        strcmp(request.address, rig.devices[0].address) != 0) {
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
    if (connected == NULL || strcmp(connected, rig.devices[0].address) != 0) {
        PAIR_TEST_FAIL("the connect should follow the pairing");
    }
    if (rig.write_len == 0U) {
        PAIR_TEST_FAIL("expected the want_config handshake write");
    }

    /* And a second connect to a node BlueZ now holds a bond for pairs nothing. */
    mesh_ble_transport_disconnect(ble);
    if (mesh_ble_transport_connect_and_pair(ble, rig.devices[0].address) != 0 ||
        mesh_ble_transport_is_pairing(ble)) {
        PAIR_TEST_FAIL("a bonded node should connect without pairing again");
    }

#undef PAIR_TEST_FAIL

    mesh_test_ble_rig_close(&rig);
    record_success(test_name);
}

/* A cancelled prompt abandons the bond instead of leaving the link half up. */
MESH_TEST_CASE(ble_transport_pair_cancel, unit) {
    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:0D", "NodeCancel", -55);
    rig.devices[0].paired = false;
    rig.mock.pair_requests_passkey = true;

    struct mesh_transport *const ble = rig.ble;
    if (mesh_test_ble_rig_start(&rig) != 0) {
        mesh_test_ble_rig_close(&rig);
        record_failure(test_name, "ble start failed");
        return;
    }
    mesh_ble_transport_refresh_devices(ble);
    (void)mesh_ble_transport_connect_and_pair(ble, rig.devices[0].address);

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

    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * "Wrong PIN" is an answer about digits somebody typed. BlueZ says AuthenticationCanceled for a
 * bond that never got as far as asking - a Brick whose controller had stopped carrying data
 * showed "wrong PIN" for a pairing no PIN was ever entered into - so without a PIN sent the
 * failure is just a pairing that failed.
 */
static bool pair_failure_says(const char *address, bool requests_passkey, const char *expected) {
    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, address, "NodeRefused", -55);
    rig.devices[0].paired = false;
    rig.mock.pair_result = -EACCES;
    rig.mock.pair_requests_passkey = requests_passkey;

    bool says = false;
    struct mesh_transport *const ble = rig.ble;
    if (mesh_test_ble_rig_start(&rig) == 0) {
        mesh_ble_transport_refresh_devices(ble);
        (void)mesh_ble_transport_connect_and_pair(ble, rig.devices[0].address);
        if (requests_passkey) {
            (void)mesh_ble_transport_submit_passkey(ble, 123456U);
        }
        for (int turn = 0; turn < 4 && mesh_ble_transport_is_pairing(ble); ++turn) {
            ble->ops->tick(ble);
        }
        char error[256] = {0};
        says = !mesh_ble_transport_is_pairing(ble) && ble->ops->take_error != NULL &&
               ble->ops->take_error(ble, error, sizeof error) && strstr(error, expected) != NULL;
    }
    mesh_test_ble_rig_close(&rig);
    return says;
}

MESH_TEST_CASE(ble_transport_wrong_pin_only_after_a_pin_was_sent, unit) {
    MESH_TEST_FAIL_IF(!pair_failure_says("AA:BB:CC:DD:EE:1F", false, "pairing failed"),
                      "a refused bond with no PIN typed should say the pairing failed");
    MESH_TEST_FAIL_IF(!pair_failure_says("AA:BB:CC:DD:EE:20", true, "wrong PIN"),
                      "a refused bond after a PIN was typed should say the PIN was wrong");
    record_success(test_name);
}
