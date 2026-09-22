#define _POSIX_C_SOURCE 200809L

/* Bringing a BLE link up and watching it fall over: discovery, connect, drops. */

#include "inkwell/base/log.h"

#include "framework/mesh_test.h"
#include "support/ble_fixture.h"

#include "inkwell/ble/central.h"
#include "inkwell/runtime/loop.h"
#include "mesh/core/config.h"
#include "mesh/core/message.h"
#include "mesh/core/session.h"
#include "mesh/transport/ble.h"
#include "mesh/transport/transport.h"

#include <pb_decode.h>

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

    struct inkwell_loop loop;
    inkwell_loop_init(&loop);

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
    inkwell_loop_shutdown(&loop);
    record_success(test_name);
}

MESH_TEST_CASE(ble_transport_discovery_mock, unit) {
    const char *failure = NULL;

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:01", "NodeOne", -45);
    mesh_test_ble_rig_add_device(&rig, "AA:BB:CC:DD:EE:02", "NodeTwo", -60);
    struct mesh_transport *const ble = rig.ble;

    const int result = mesh_test_ble_rig_start(&rig);
    if (result != 0) {
        failure = "ble start should succeed with mock";
        goto cleanup;
    }

    struct inkwell_ble_device discovered[4];
    size_t count =
        mesh_ble_transport_get_devices(ble, discovered, sizeof(discovered) / sizeof(discovered[0]));
    if (count != rig.device_count) {
        failure = "unexpected discovered device count";
        goto cleanup;
    }

    if (strcmp(discovered[0].name, "NodeOne") != 0 ||
        strcmp(discovered[1].address, "AA:BB:CC:DD:EE:02") != 0) {
        failure = "device details mismatch";
        goto cleanup;
    }

    /* One of the two drops out of earshot between listings. */
    rig.devices[0].rssi = -35;
    rig.mock.device_count = 1U;
    mesh_test_ble_rig_reload(&rig);
    const size_t refreshed = mesh_ble_transport_refresh_devices(ble);
    if (refreshed != 1U) {
        failure = "refresh should update device list";
        goto cleanup;
    }

cleanup:
    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(ble_transport_connect_mock, unit) {
    const char *failure = NULL;

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:03", "NodeThree", -40);
    struct mesh_transport *const ble = rig.ble;

    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }

    if (mesh_test_ble_rig_connect(&rig) != 0) {
        failure = "connect should succeed";
        goto cleanup;
    }

    if (rig.write_len == 0U) {
        failure = "expected want_config write";
        goto cleanup;
    }

    if (strcmp(rig.write_path, rig.toradio_path) != 0) {
        failure = "want_config write path mismatch";
        goto cleanup;
    }

    /* BLE ToRadio writes carry the bare protobuf: no varint length prefix. */
    meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
    pb_istream_t to_radio_stream = pb_istream_from_buffer(rig.write_capture, rig.write_len);
    if (!pb_decode(&to_radio_stream, meshtastic_ToRadio_fields, &to_radio)) {
        failure = "failed to decode want_config payload";
        goto cleanup;
    }

    if (to_radio.which_payload_variant != meshtastic_ToRadio_want_config_id_tag) {
        failure = "unexpected ToRadio payload";
        goto cleanup;
    }

    struct mesh_handshake_status handshake = mesh_ble_transport_handshake_status(ble);
    if (!handshake.request_in_flight || handshake.request_id != to_radio.want_config_id) {
        failure = "handshake state not initialised";
        goto cleanup;
    }

    /* Script the node's FromRadio FIFO: my_info, node_info, config_complete, then empty. */
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    from_radio.my_info.my_node_num = 0x01020304U;
    from_radio.my_info.nodedb_count = 2U;

    if (!mesh_test_ble_rig_script(&rig, 0U, &from_radio)) {
        failure = "failed to encode my_info";
        goto cleanup;
    }
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

    if (!mesh_test_ble_rig_script(&rig, 1U, &from_radio)) {
        failure = "failed to encode node_info";
        goto cleanup;
    }
    const uint32_t expected_peer_num = from_radio.node_info.num;

    /* Two more peers so the FIFO is longer than one turn's read budget. */
    for (size_t extra = 0; extra < 2U; ++extra) {
        from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
        from_radio.which_payload_variant = meshtastic_FromRadio_node_info_tag;
        from_radio.node_info.num = 0x01020306U + (uint32_t)extra;
        from_radio.node_info.has_user = true;
        snprintf(from_radio.node_info.user.short_name, sizeof(from_radio.node_info.user.short_name),
                 "P%zu", extra);
        if (!mesh_test_ble_rig_script(&rig, 2U + extra, &from_radio)) {
            failure = "failed to encode extra node_info";
            goto cleanup;
        }
    }

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
    from_radio.config_complete_id = to_radio.want_config_id;

    if (!mesh_test_ble_rig_script(&rig, 4U, &from_radio)) {
        failure = "failed to encode config_complete";
        goto cleanup;
    }

    /* Rewind the scripted FIFO and poke FromNum. */
    rig.read_index = 0U;
    const uint8_t from_num[4] = {5U, 0U, 0U, 0U};
    inkwell_ble_mock_emit_notification(rig.fromnum_path, from_num, sizeof(from_num));

    /* The first turn reads its budget and must stop short of the end; the loop wake finishes it. */
    if (rig.read_index >= 6U) {
        failure = "drain did not yield to the event loop between read batches";
        goto cleanup;
    }
    for (int spin = 0; spin < 20 && rig.read_index < 6U; ++spin) {
        inkwell_loop_run(&rig.loop, 10);
        ble->ops->tick(ble);
    }

    handshake = mesh_ble_transport_handshake_status(ble);
    if (!handshake.has_my_info || handshake.my_info.my_node_num != expected_node_num) {
        failure = "my_info not cached";
        goto cleanup;
    }

    if (handshake.node_count != 3U || handshake.nodes[0].node_id != expected_peer_num) {
        failure = "node info cache incorrect";
        goto cleanup;
    }

    if (handshake.request_in_flight || !handshake.config_complete ||
        handshake.config_complete_id != to_radio.want_config_id) {
        failure = "config handshake did not complete";
        goto cleanup;
    }

    /* Five payloads plus the terminating empty read. */
    if (rig.read_index != 6U) {
        failure = "FromRadio drain did not read until empty";
        goto cleanup;
    }

    struct mesh_ble_transport_stats stats = mesh_ble_transport_stats(ble);
    if (stats.frames_received != 5U) {
        failure = "unexpected frame count after handshake";
        goto cleanup;
    }

    /* Outbound packets go out as exactly one write each, unchunked. */
    size_t handshake_write_calls = rig.write_call_count;
    uint8_t outbound_packet[300];
    for (size_t i = 0; i < sizeof(outbound_packet); ++i) {
        outbound_packet[i] = (uint8_t)i;
    }

    if (mesh_ble_transport_send_packet(ble, outbound_packet, sizeof(outbound_packet)) != 0) {
        failure = "failed to queue outbound packet";
        goto cleanup;
    }

    if (rig.write_call_count - handshake_write_calls != 1U ||
        rig.write_lengths[handshake_write_calls] != sizeof(outbound_packet)) {
        failure = "outbound packet was not written as a single ToRadio write";
        goto cleanup;
    }

    uint8_t oversized[MESH_BLE_MAX_PACKET_SIZE + 1U];
    memset(oversized, 0xAB, sizeof(oversized));
    if (mesh_ble_transport_send_packet(ble, oversized, sizeof(oversized)) != -EMSGSIZE) {
        failure = "oversized packet should be rejected";
        goto cleanup;
    }

    if (mesh_ble_transport_connect(ble, rig.devices[0].address) != -EALREADY) {
        failure = "duplicate connect should return -EALREADY";
        goto cleanup;
    }

    if (mesh_ble_transport_disconnect(ble) != 0) {
        failure = "disconnect should succeed";
        goto cleanup;
    }

    /* The connection's own state goes. What describes the radio does not: the roster, because
       the radio's NodeDB is small enough to have evicted half of it by the next connect, and
       now MyNodeInfo and the config with it, because re-fetching those from nothing on every
       drop is what kept a flapping link from ever finishing a sync. */
    handshake = mesh_ble_transport_handshake_status(ble);
    if (handshake.request_in_flight || handshake.node_count == 0U || !handshake.has_my_info ||
        handshake.config_complete) {
        failure = "handshake state not cleared on disconnect";
        goto cleanup;
    }

    if (mesh_ble_transport_disconnect(ble) != -ENOTCONN) {
        failure = "second disconnect should return -ENOTCONN";
        goto cleanup;
    }

cleanup:
    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* Mirrors what a real node does on a fresh BlueZ cache: Connect returns, but the GATT
   characteristics only appear a few polls later. The connect must stay pending, not fail. */
MESH_TEST_CASE(ble_transport_connect_deferred_services, unit) {
    const char *failure = NULL;

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:05", "NodeFive", -50);
    rig.mock.services_resolved_after_polls = 2U;
    struct mesh_transport *const ble = rig.ble;

    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    if (mesh_test_ble_rig_connect(&rig) != 0) {
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
    if (rig.write_len != 0U) {
        failure = "want_config must wait for the characteristics";
        goto cleanup;
    }
    if (mesh_ble_transport_connect(ble, rig.devices[0].address) != -EINPROGRESS) {
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
    if (!mesh_ble_transport_is_connecting(ble) || rig.write_len != 0U) {
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
    if (connected == NULL || strcmp(connected, rig.devices[0].address) != 0) {
        failure = "connected address mismatch after deferred connect";
        goto cleanup;
    }
    if (rig.write_len == 0U || strcmp(rig.write_path, rig.toradio_path) != 0) {
        failure = "want_config write missing after deferred connect";
        goto cleanup;
    }

cleanup:
    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* A Properties.Get(ServicesResolved) that does not come back inside the central's property
   deadline is bluetoothd being busy, not the GATT database being absent -
   the state it is busiest in being the connect it is still scanning through. The poll is retried
   and MESH_BLE_SERVICES_TIMEOUT_MS remains the only bound on discovery; treating the timeout as
   fatal instead ended every connect a second in and retried on the auto-connect timer for ever. */
MESH_TEST_CASE(ble_transport_services_resolved_timeout_retries, unit) {
    const char *failure = NULL;

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:0A", "NodeTen", -50);
    /* Two polls time out, then the database is there on the first real answer. */
    rig.mock.services_resolved_timeout_polls = 2U;
    struct mesh_transport *const ble = rig.ble;

    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    if (mesh_test_ble_rig_connect(&rig) != 0) {
        failure = "connect should be accepted";
        goto cleanup;
    }
    /* The first poll happened inside connect() and timed out. The link must have survived it. */
    if (!mesh_ble_transport_is_connecting(ble)) {
        failure = "a timed-out ServicesResolved poll must not end the link";
        goto cleanup;
    }

    /* Second poll: the other timeout. Still connecting, still no link failure recorded. */
    test_sleep_ms(300U);
    ble->ops->tick(ble);
    if (!mesh_ble_transport_is_connecting(ble)) {
        failure = "a second timed-out poll must not end the link either";
        goto cleanup;
    }

    /* Third poll answers, and the connect completes off the back of a retry. */
    test_sleep_ms(300U);
    ble->ops->tick(ble);
    if (mesh_ble_transport_is_connecting(ble)) {
        failure = "connect should have completed once the poll answered";
        goto cleanup;
    }
    const char *connected = mesh_ble_transport_connected_address(ble);
    if (connected == NULL || strcmp(connected, rig.devices[0].address) != 0) {
        failure = "connected address mismatch after retried service discovery";
        goto cleanup;
    }
    if (rig.write_len == 0U || strcmp(rig.write_path, rig.toradio_path) != 0) {
        failure = "want_config write missing after retried service discovery";
        goto cleanup;
    }

    char error[128];
    if (ble->ops->take_error != NULL && ble->ops->take_error(ble, error, sizeof error)) {
        failure = "a retried timeout must not leave a link failure to report";
        goto cleanup;
    }

cleanup:
    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* Device1.Connect answers asynchronously; the link stays "connecting" (and the loop free)
   until the reply lands, and a refused connect drops back to disconnected. */
MESH_TEST_CASE(ble_transport_connect_async_reply, unit) {
    const char *failure = NULL;

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:08", "NodeEight", -50);
    rig.mock.connect_pending_polls = 2U;
    struct mesh_transport *const ble = rig.ble;

    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    if (mesh_test_ble_rig_connect(&rig) != 0) {
        failure = "connect should be accepted";
        goto cleanup;
    }
    if (!mesh_ble_transport_is_connecting(ble) || rig.write_len != 0U) {
        failure = "must stay connecting until the Connect reply";
        goto cleanup;
    }
    if (strcmp(ble->ops->status(ble), "connecting") != 0) {
        failure = "status should read connecting";
        goto cleanup;
    }

    ble->ops->tick(ble); /* poll 2: still pending */
    if (!mesh_ble_transport_is_connecting(ble) || rig.write_len != 0U) {
        failure = "reply arrived too early";
        goto cleanup;
    }
    ble->ops->tick(ble); /* poll 3: reply, services already resolved, handshake sent */
    if (mesh_ble_transport_connected_address(ble) == NULL || rig.write_len == 0U) {
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
    rig.mock.connect_result = -EIO;
    rig.mock.connect_pending_polls = 1U;
    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble restart failed";
        goto cleanup;
    }
    if (mesh_test_ble_rig_connect(&rig) != 0) {
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
    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* FromNum's subscribe is waited for on later turns, not in the connect: a stack that takes its
   time answering (a stale bond, or macOS's pairing dialog) leaves the link connecting, not
   failed, and the handshake goes out once it has answered. */
MESH_TEST_CASE(ble_transport_connect_waits_for_the_subscribe, unit) {
    const char *failure = NULL;

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:0B", "NodeEleven", -50);
    rig.mock.subscribe_pending_polls = 2U;
    struct mesh_transport *const ble = rig.ble;

    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    if (mesh_test_ble_rig_connect(&rig) != 0) {
        failure = "connect should be accepted";
        goto cleanup;
    }
    if (!mesh_ble_transport_is_connecting(ble) || rig.write_len != 0U) {
        failure = "must stay connecting while the subscribe is unanswered";
        goto cleanup;
    }
    ble->ops->tick(ble);
    if (!mesh_ble_transport_is_connecting(ble) || rig.write_len != 0U) {
        failure = "the subscribe was taken as answered too early";
        goto cleanup;
    }
    ble->ops->tick(ble);
    if (mesh_ble_transport_connected_address(ble) == NULL || rig.write_len == 0U) {
        failure = "connect should complete once the subscribe is answered";
        goto cleanup;
    }

cleanup:
    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* BlueZ says the device is gone: the link resets, the UI sees "running", and auto-connect can
   try again. Checked via the explicit probe tick() runs every couple of seconds. */
MESH_TEST_CASE(ble_transport_link_drop, unit) {
    const char *failure = NULL;

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:0A", "NodeTen", -50);
    /* tick() already probes once on connect */
    rig.mock.connected_drops_after_polls = 2U;
    struct mesh_transport *const ble = rig.ble;

    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    if (mesh_test_ble_rig_connect(&rig) != 0) {
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
    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* A failing GATT write is a dead link: send_text reports the error, marks the message FAILED
   and the link resets instead of pretending the message went out. */
MESH_TEST_CASE(ble_transport_write_failure, unit) {
    const char *failure = NULL;

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:0B", "NodeEleven", -50);
    /* the handshake write succeeds, the message does not */
    rig.mock.write_fail_after_calls = 1U;
    rig.mock.write_result_late = -EIO;
    struct mesh_transport *const ble = rig.ble;

    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    if (mesh_test_ble_rig_connect(&rig) != 0) {
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
    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* bluetoothd is not always on the bus when MeshClient launches - the first launch after the
   Brick wakes from sleep routinely beats it there - so the transport has to pick BlueZ up when
   it arrives instead of sitting in waiting-for-bluez until the app is restarted. */
MESH_TEST_CASE(ble_transport_recovers_when_bluez_arrives, unit) {
    const char *failure = NULL;

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:01", "NodeOne", -45);
    rig.mock.check_ready_result = -ENODEV;
    struct mesh_transport *const ble = rig.ble;

    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    if (strcmp(ble->ops->status(ble), "waiting-for-bluez") != 0) {
        failure = "no bluetoothd should park the transport at waiting-for-bluez";
        goto cleanup;
    }
    if (mesh_ble_transport_connect(ble, rig.devices[0].address) != -EAGAIN) {
        failure = "connecting without bluetoothd should be refused";
        goto cleanup;
    }

    /* bluetoothd arrives; the next loop turn is what has to notice. */
    rig.mock.check_ready_result = 0;
    mesh_test_ble_rig_reload(&rig);
    ble->ops->tick(ble);

    if (strcmp(ble->ops->status(ble), "running") != 0) {
        failure = "the transport should come up once bluetoothd is on the bus";
        goto cleanup;
    }
    struct inkwell_ble_device discovered[4];
    if (mesh_ble_transport_get_devices(ble, discovered, 4U) != 1U) {
        failure = "discovery should be running after the retry";
        goto cleanup;
    }

cleanup:
    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* And the reverse: bluetoothd leaving under a ready transport - Bluetooth toggled off, a resume
   that restarted it - must put the transport back to waiting rather than leave it pointing at
   an adapter that no longer exists. */
MESH_TEST_CASE(ble_transport_demotes_when_bluez_leaves, unit) {
    const char *failure = NULL;

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:01", "NodeOne", -45);
    struct mesh_transport *const ble = rig.ble;

    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    if (strcmp(ble->ops->status(ble), "running") != 0) {
        failure = "the transport should start ready";
        goto cleanup;
    }

    rig.mock.check_ready_result = -ENODEV;
    mesh_test_ble_rig_reload(&rig);
    ble->ops->tick(ble);

    if (strcmp(ble->ops->status(ble), "waiting-for-bluez") != 0) {
        failure = "losing bluetoothd should demote the transport";
        goto cleanup;
    }
    struct inkwell_ble_device discovered[4];
    if (mesh_ble_transport_get_devices(ble, discovered, 4U) != 0U) {
        failure = "devices found through the old BlueZ should be dropped";
        goto cleanup;
    }
    if (mesh_ble_transport_connect(ble, rig.devices[0].address) != -EAGAIN) {
        failure = "connecting after the demote should be refused";
        goto cleanup;
    }

cleanup:
    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* On a Mac the stack does not leave; the user takes Bluetooth away from the app in System
   Settings, and CoreBluetooth starts answering -EACCES under a running transport. That has to
   demote it exactly as a departed bluetoothd does, and giving access back has to bring it up. */
MESH_TEST_CASE(ble_transport_demotes_when_access_is_revoked, unit) {
    const char *failure = NULL;

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:01", "NodeOne", -45);
    struct mesh_transport *const ble = rig.ble;

    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    if (strcmp(ble->ops->status(ble), "running") != 0) {
        failure = "the transport should start ready";
        goto cleanup;
    }

    rig.mock.check_ready_result = -EACCES;
    mesh_test_ble_rig_reload(&rig);
    ble->ops->tick(ble);

    if (strcmp(ble->ops->status(ble), "running") == 0) {
        failure = "revoked access should demote the transport";
        goto cleanup;
    }
    struct inkwell_ble_device discovered[4];
    if (mesh_ble_transport_get_devices(ble, discovered, 4U) != 0U) {
        failure = "devices found before access was revoked should be dropped";
        goto cleanup;
    }

    rig.mock.check_ready_result = 0;
    mesh_test_ble_rig_reload(&rig);
    ble->ops->tick(ble);

    if (strcmp(ble->ops->status(ble), "running") != 0) {
        failure = "the transport should come back once access is restored";
        goto cleanup;
    }
    if (mesh_ble_transport_get_devices(ble, discovered, 4U) != 1U) {
        failure = "discovery should be running again after access is restored";
        goto cleanup;
    }

cleanup:
    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* Everything cached about BlueZ dies with it. A bond in flight and the pairing agent are the two
   that reset_link() does not cover: a stale pair_state answers every later Pair with -EBUSY, and
   a stale agent registration makes register_agent() a no-op against a daemon that never saw it,
   so PIN-mode nodes would stay unpairable until the app was restarted. */
MESH_TEST_CASE(ble_transport_pairing_survives_a_bluez_outage, unit) {
    const char *failure = NULL;
    unsigned agent_registrations = 0U;

    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:01", "NodeOne", -45);
    rig.devices[0].paired = false;
    /* The bond never completes on its own, so it is still in flight when BlueZ goes. */
    rig.mock.pair_pending_polls = 64U;
    rig.mock.agent_register_calls = &agent_registrations;
    struct mesh_transport *const ble = rig.ble;

    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    if (mesh_ble_transport_connect_and_pair(ble, rig.devices[0].address) != 0) {
        failure = "pairing should start";
        goto cleanup;
    }
    if (strcmp(ble->ops->status(ble), "pairing") != 0) {
        failure = "the link should be pairing";
        goto cleanup;
    }

    /* bluetoothd goes away mid-bond, then comes back. */
    rig.mock.check_ready_result = -ENODEV;
    mesh_test_ble_rig_reload(&rig);
    ble->ops->tick(ble);
    if (strcmp(ble->ops->status(ble), "waiting-for-bluez") != 0) {
        failure = "losing bluetoothd mid-bond should demote the transport";
        goto cleanup;
    }

    rig.mock.check_ready_result = 0;
    mesh_test_ble_rig_reload(&rig);
    ble->ops->tick(ble);
    if (strcmp(ble->ops->status(ble), "running") != 0) {
        failure = "the transport should come back up";
        goto cleanup;
    }
    if (agent_registrations != 2U) {
        failure = "the pairing agent should be registered again with the new bluetoothd";
        goto cleanup;
    }
    if (mesh_ble_transport_connect_and_pair(ble, rig.devices[0].address) != 0) {
        failure = "pairing should start again rather than report the old bond still in flight";
        goto cleanup;
    }

cleanup:
    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* Scanning and a link may not share the radio. A node allows a 1000 ms supervision timeout, and
   BlueZ's discovery is an active LE scan at a 50% duty cycle that bluetoothd restarts every ten
   seconds; held across a link long enough to matter, it takes the Brick's one antenna away for
   longer than that and the link drops mid-roster. So the scan is derived from the link state
   rather than paired with a connect: down for the whole of CONNECTING and CONNECTED, back up the
   moment there is no link to protect. */
/* The shipped hold is three seconds, which is a sleep this suite should not be paying. The knob
   exists so a bench can shorten it; the contract under test is the hold, not its length. */
#define SCAN_GRACE_MS 60U

MESH_TEST_CASE(ble_transport_scan_yields_to_the_link, unit) {
    const char *failure = NULL;

    setenv("MESHCLIENT_SCAN_RESUME_GRACE_MS", "60", 1);
    unsigned starts = 0U;
    unsigned stops = 0U;
    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:0A", "NodeTen", -50);
    rig.mock.start_discovery_calls = &starts;
    rig.mock.stop_discovery_calls = &stops;
    /* tick() already probes once on connect */
    rig.mock.connected_drops_after_polls = 2U;
    struct mesh_transport *const ble = rig.ble;

    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }
    if (starts != 1U || stops != 0U) {
        failure = "bring-up should start scanning exactly once";
        goto cleanup;
    }
    mesh_ble_transport_refresh_devices(ble);

    /* Down before Device1.Connect goes out, not a tick later: establishing the link needs the
       radio as much as holding it does. */
    if (mesh_ble_transport_connect(ble, rig.devices[0].address) != 0) {
        failure = "connect should be accepted";
        goto cleanup;
    }
    if (stops != 1U) {
        failure = "connect must stop the scan before it sends Connect";
        goto cleanup;
    }

    /* And it stays down: every turn re-derives it, so none of them may put it back. */
    ble->ops->tick(ble);
    ble->ops->tick(ble);
    if (mesh_ble_transport_connected_address(ble) == NULL) {
        failure = "expected a connected link";
        goto cleanup;
    }
    if (starts != 1U || stops != 1U) {
        failure = "a held scan must not be restarted while the link is up";
        goto cleanup;
    }

    /* The link drops. The scan is what finds the node again, so it has to come back - but not
       into the auto-connect retry that follows a drop within the second, which is what starting
       it here and stopping it there made it do. */
    if (mesh_ble_transport_check_link(ble) != 1) {
        failure = "first probe should find the link up";
        goto cleanup;
    }
    if (mesh_ble_transport_check_link(ble) != 0) {
        failure = "second probe should find the link down and reset it";
        goto cleanup;
    }
    ble->ops->tick(ble);
    ble->ops->tick(ble);
    if (starts != 1U || stops != 1U) {
        failure = "a teardown must hold the scan down for the reconnect that follows it";
        goto cleanup;
    }

    /* Past the hold, nothing came back for it, so the scan is the only thing that finds the
       node again. */
    test_sleep_ms(SCAN_GRACE_MS * 2U);
    ble->ops->tick(ble);
    if (starts != 2U || stops != 1U) {
        failure = "scanning should resume once the hold has expired";
        goto cleanup;
    }
    ble->ops->tick(ble);
    if (starts != 2U) {
        failure = "a scan already running must not be started again every turn";
        goto cleanup;
    }

cleanup:
    mesh_test_ble_rig_close(&rig);
    unsetenv("MESHCLIENT_SCAN_RESUME_GRACE_MS");
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* Enumeration is a blocking GetManagedObjects, and tick() used to make one every second whether
   or not there was a link - so a roster sync spent a blocking second per second in the loop that
   was supposed to be reading it. With the scan held from the connect onward the answer cannot
   change anyway, so the only correct number of calls while linked is none.
   Driven through tick() here; the 5 s refresh timer is the other periodic caller and shares the
   one guard in mesh_ble_refresh_devices_periodic(), which is why the guard is there rather than
   at each call site - fixing only this path left that one still enumerating mid-sync. */
MESH_TEST_CASE(ble_transport_enumeration_yields_to_the_link, unit) {
    const char *failure = NULL;

    unsigned list_calls = 0U;
    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:0A", "NodeTen", -50);
    rig.mock.list_calls = &list_calls;
    struct mesh_transport *const ble = rig.ble;

    if (mesh_test_ble_rig_start(&rig) != 0) {
        failure = "ble start failed";
        goto cleanup;
    }

    /* Disconnected: the device list is the only way to find a node, so it is refreshed. */
    ble->ops->tick(ble);
    if (list_calls == 0U) {
        failure = "a disconnected transport must still enumerate";
        goto cleanup;
    }

    if (mesh_ble_transport_connect(ble, rig.devices[0].address) != 0) {
        failure = "connect should be accepted";
        goto cleanup;
    }
    const unsigned linked_from = list_calls;
    for (unsigned i = 0; i < 8U; ++i) {
        ble->ops->tick(ble);
    }
    if (mesh_ble_transport_connected_address(ble) == NULL) {
        failure = "expected a connected link";
        goto cleanup;
    }
    if (list_calls != linked_from) {
        failure = "a link must not be interrupted to enumerate what cannot have changed";
        goto cleanup;
    }

    /* And what the last scan found is kept rather than cleared, so the Devices tab still lists
       it while the link is up. */
    size_t held = 0U;
    if (mesh_ble_transport_devices(ble, &held) == NULL || held != 1U) {
        failure = "the known device list must survive the link";
        goto cleanup;
    }

cleanup:
    mesh_test_ble_rig_close(&rig);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Runs one device refresh with stderr diverted into a file, and answers how many of the lines it
 * wrote mention `needle`.
 *
 * The behaviour under test is what the log is *told*, so the log is what the case has to read.
 * stderr is unbuffered, but it is flushed either side of the swap anyway: a line left in a buffer
 * across the dup2 would be counted against the wrong refresh, which is the one way this could
 * pass while the guard did nothing.
 */
static int ble_refresh_counting_log_lines(struct mesh_transport *ble, const char *needle) {
    char captured_path[] = "/tmp/mesh_ble_rosterXXXXXX";
    const int captured = mkstemp(captured_path);
    if (captured < 0) {
        return -1;
    }
    fflush(stderr);
    const int saved = dup(STDERR_FILENO);
    if (saved < 0 || dup2(captured, STDERR_FILENO) < 0) {
        if (saved >= 0) {
            (void)close(saved);
        }
        (void)close(captured);
        (void)unlink(captured_path);
        return -1;
    }

    (void)mesh_ble_transport_refresh_devices(ble);

    fflush(stderr);
    (void)dup2(saved, STDERR_FILENO);
    (void)close(saved);
    (void)close(captured);

    FILE *file = fopen(captured_path, "re");
    if (file == NULL) {
        (void)unlink(captured_path);
        return -1;
    }
    int hits = 0;
    char line[512];
    while (fgets(line, (int)sizeof line, file) != NULL) {
        if (strstr(line, needle) != NULL) {
            ++hits;
        }
    }
    (void)fclose(file);
    (void)unlink(captured_path);
    return hits;
}

/*
 * The roster is logged when it changes and not once a second for the length of the session.
 *
 * This enumeration runs from the tick for as long as the client is looking for a radio, so an
 * unguarded line per device per second was the largest single thing in the log on the card - and
 * all of it the same list restated. RSSI is deliberately outside what counts as a change: it moves
 * on almost every read, so counting it would put the per-second repetition straight back.
 */
MESH_TEST_CASE(ble_roster_is_logged_on_change_not_on_every_refresh, unit) {
    struct mesh_test_ble_rig rig;
    mesh_test_ble_rig_init(&rig, "AA:BB:CC:DD:EE:01", "Radio One", -55);
    MESH_TEST_FAIL_IF(!mesh_test_ble_rig_add_device(&rig, "AA:BB:CC:DD:EE:02", "Radio Two", -70),
                      "could not add a second advertiser");
    MESH_TEST_FAIL_IF(mesh_test_ble_rig_start(&rig) != 0, "the BLE transport did not start");

    const enum inkwell_log_level saved_level = inkwell_log_get_level();
    inkwell_log_set_level(INKWELL_LOG_LEVEL_DEBUG);

    /* Starting the transport enumerated once and announced what it found, so the roster has
       already been logged by the time this case gets a look in - which is what makes the next
       refresh the interesting one rather than the first. */
    const int unchanged = ble_refresh_counting_log_lines(rig.ble, "AA:BB:CC:DD:EE:01");

    /* Only the reading that drifts has moved, which is not news either. */
    rig.devices[0].rssi = -61;
    const int rssi_only = ble_refresh_counting_log_lines(rig.ble, "AA:BB:CC:DD:EE:01");

    /* A device saying something different about itself is. */
    snprintf(rig.devices[0].name, sizeof rig.devices[0].name, "Radio One Renamed");
    const int renamed = ble_refresh_counting_log_lines(rig.ble, "AA:BB:CC:DD:EE:01");

    /* So is one leaving. */
    rig.device_count = 1U;
    rig.mock.device_count = 1U;
    mesh_test_ble_rig_reload(&rig);
    const int departed = ble_refresh_counting_log_lines(rig.ble, "AA:BB:CC:DD:EE:01");
    /* And having been logged, it goes quiet again rather than repeating from then on. */
    const int settled = ble_refresh_counting_log_lines(rig.ble, "AA:BB:CC:DD:EE:01");

    inkwell_log_set_level(saved_level);
    mesh_test_ble_rig_close(&rig);

    MESH_TEST_FAIL_IF(unchanged < 0 || rssi_only < 0 || renamed < 0 || departed < 0 || settled < 0,
                      "could not capture the log around a refresh");
    MESH_TEST_FAIL_IF(unchanged != 0, "an unchanged roster was logged again");
    MESH_TEST_FAIL_IF(rssi_only != 0, "a drifting RSSI alone was logged as a roster change");
    MESH_TEST_FAIL_IF(renamed != 1, "a renamed device was not logged");
    MESH_TEST_FAIL_IF(departed != 1, "a device leaving did not re-log the roster");
    MESH_TEST_FAIL_IF(settled != 0, "the roster kept being logged after the change that caused it");
    record_success(test_name);
}
