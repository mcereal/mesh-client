#define _POSIX_C_SOURCE 200809L

/* The UI store: state, persistence, refresh requests and the message list. */

#include "framework/mesh_test.h"

#include "mesh/core/message.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

MESH_TEST_CASE(ui_store_basic, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

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

MESH_TEST_CASE(ui_store_persistence, unit) {
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
    /* The detail the Nodes tab drills into rides along in the same cache, so a disconnected
       Brick can still be browsed. Each block is written on its own key line. */
    snprintf(handshake.nodes[0].user_id, sizeof(handshake.nodes[0].user_id), "%s", "!000010a2");
    handshake.nodes[0].hw_model = 9U;
    handshake.nodes[0].role = 2U;
    handshake.nodes[0].is_favorite = true;
    handshake.nodes[0].channel = 3U;
    handshake.nodes[0].public_key_len = 32U;
    memset(handshake.nodes[0].public_key, 0x5A, sizeof(handshake.nodes[0].public_key));
    handshake.nodes[0].position.valid = true;
    handshake.nodes[0].position.latitude_i = 447654321;
    handshake.nodes[0].position.longitude_i = -680012345;
    handshake.nodes[0].position.has_altitude = true;
    handshake.nodes[0].position.altitude = 312;
    handshake.nodes[0].position.sats_in_view = 9U;
    handshake.nodes[0].metrics.valid = true;
    handshake.nodes[0].metrics.has_battery = true;
    handshake.nodes[0].metrics.battery_level = 76U;
    handshake.nodes[0].metrics.has_uptime = true;
    handshake.nodes[0].metrics.uptime_seconds = 90061U;
    handshake.nodes[0].environment.valid = true;
    handshake.nodes[0].environment.has_temperature = true;
    handshake.nodes[0].environment.temperature = 21.5f;
    /* The two the radio does not tell us on a resync: whether the name is the node's own, and
       whether the radio still carried it. The restored roster is only worth more than a fresh
       sync if both come back. */
    handshake.nodes[0].has_user = true;
    handshake.nodes[0].in_nodedb = false;
    /* Whose roster this is; the session is handed it back at startup. */
    handshake.roster_owner = 0x1234U;
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

    const struct mesh_ui_node_summary *node = &store.handshake.nodes[0];
    if (strcmp(node->user_id, "!000010a2") != 0 || node->hw_model != 9U || node->role != 2U ||
        !node->is_favorite || node->channel != 3U || node->public_key_len != 32U ||
        node->public_key[31] != 0x5AU) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "node identity did not survive the cache");
        return;
    }
    if (store.handshake.roster_owner != 0x1234U) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the roster owner did not survive the cache");
        return;
    }
    if (!node->has_user || node->in_nodedb) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "node roster state did not survive the cache");
        return;
    }
    if (!node->position.valid || node->position.latitude_i != 447654321 ||
        node->position.longitude_i != -680012345 || !node->position.has_altitude ||
        node->position.altitude != 312 || node->position.sats_in_view != 9U) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "node position did not survive the cache");
        return;
    }
    if (!node->metrics.valid || node->metrics.battery_level != 76U ||
        node->metrics.uptime_seconds != 90061U) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "node metrics did not survive the cache");
        return;
    }
    if (!node->environment.valid || node->environment.temperature < 21.4f ||
        node->environment.temperature > 21.6f) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "node environment did not survive the cache");
        return;
    }

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* Regression: the store deliberately stays quiet when nothing changed, so a client that
   comes up with no devices and no handshake never got a snapshot - and the framebuffer
   backend never painted, leaving a black screen on the device. */
MESH_TEST_CASE(ui_store_refresh_request, unit) {
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

MESH_TEST_CASE(ui_store_messages, unit) {
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
    MESH_TEST_FAIL_IF(!mesh_ui_store_consume_updates(&store, &snapshot),
                      "setting messages should raise an update");
    MESH_TEST_FAIL_IF((snapshot.update_flags & MESH_UI_UPDATE_MESSAGES) == 0U,
                      "the messages flag should be set");
    MESH_TEST_FAIL_IF(snapshot.messages.count != 2U || snapshot.messages.dropped != 7U,
                      "message list did not reach the snapshot");

    /* Setting the same list again is not a change and must not wake the UI. */
    mesh_ui_store_set_messages(&store, &list);
    struct mesh_ui_snapshot repeat;
    memset(&repeat, 0, sizeof(repeat));
    MESH_TEST_FAIL_IF(mesh_ui_store_consume_updates(&store, &repeat),
                      "an unchanged message list should not raise an update");

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

    MESH_TEST_FAIL_IF(!ok, "messages did not survive the save/load roundtrip");

    record_success(test_name);
}

/* Regression for the cache-erasure bug: the transport's log starts empty on every run, so a
   publish that ignored the restored history would blank the store and the next save would
   erase the conversation permanently. */
MESH_TEST_CASE(ui_message_list_merge, unit) {
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
    MESH_TEST_FAIL_IF(merged.count != 2U || strcmp(merged.entries[0].text, "older") != 0 ||
                          strcmp(merged.entries[1].text, "newer") != 0,
                      "an empty live list should preserve cached history");
    MESH_TEST_FAIL_IF(merged.dropped != 1U, "the cached dropped count should carry through");

    /* Live messages append after the history, newest last. */
    live.count = 1U;
    live.entries[0].packet_id = 200U;
    snprintf(live.entries[0].text, sizeof(live.entries[0].text), "live");
    mesh_ui_message_list_merge(&cached, &live, &merged);
    MESH_TEST_FAIL_IF(merged.count != 3U || strcmp(merged.entries[0].text, "older") != 0 ||
                          strcmp(merged.entries[2].text, "live") != 0,
                      "live messages should append after cached history");

    /* A message re-received after a restart must not appear twice. */
    live.entries[0].packet_id = 101U;
    snprintf(live.entries[0].text, sizeof(live.entries[0].text), "newer");
    mesh_ui_message_list_merge(&cached, &live, &merged);
    MESH_TEST_FAIL_IF(merged.count != 2U || strcmp(merged.entries[0].text, "older") != 0 ||
                          merged.entries[1].packet_id != 101U,
                      "a cached entry re-received live should not be duplicated");

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
    MESH_TEST_FAIL_IF(merged.count != 2U, "packet id 0 should not collapse distinct messages");

    /* When live traffic fills every slot, the oldest history is dropped and counted. */
    struct mesh_ui_message_list full_live;
    memset(&full_live, 0, sizeof(full_live));
    full_live.count = MESH_UI_MAX_MESSAGES;
    for (uint32_t i = 0; i < MESH_UI_MAX_MESSAGES; ++i) {
        full_live.entries[i].packet_id = 1000U + i;
    }
    mesh_ui_message_list_merge(&cached, &full_live, &merged);
    MESH_TEST_FAIL_IF(merged.count != MESH_UI_MAX_MESSAGES || merged.entries[0].packet_id != 1000U,
                      "live messages should win every slot when the list is full");
    MESH_TEST_FAIL_IF(merged.dropped != 1U + 2U,
                      "squeezed-out history should be added to the dropped count");

    record_success(test_name);
}

MESH_TEST_CASE(ui_canned_load, unit) {
    const char *failure = NULL;

    char path[] = "/tmp/meshclient-canned-XXXXXX";
    int fd = mkstemp(path);
    MESH_TEST_FAIL_IF(fd < 0, "mkstemp failed");
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
