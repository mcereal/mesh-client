#define _POSIX_C_SOURCE 200809L

/*
 * The Devices tab's rows, and the one of them that is not a device: the network address.
 *
 * The tab was a plain list of what discovery found until the network row arrived, so most of
 * what is worth testing here is the seam between the two - that the row is present exactly when
 * the list has no network row of its own, that its index is the last one, and that the presses
 * on it raise the two actions the app already knows how to act on.
 */

#include "framework/mesh_test.h"

#include "mesh/transport/tcp.h"
#include "mesh/ui/devices.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store.h"

#include <stdio.h>
#include <string.h>

/* One discovered radio, so the network row is never the only thing in the list. */
static void seed_devices(struct mesh_ui_store *store, size_t count) {
    struct mesh_ui_device devices[MESH_UI_MAX_DEVICES];
    memset(devices, 0, sizeof devices);
    for (size_t i = 0; i < count; ++i) {
        snprintf(devices[i].identifier, sizeof devices[i].identifier, "AA:BB:CC:DD:EE:%02zu", i);
        snprintf(devices[i].name, sizeof devices[i].name, "Node%zu", i);
        devices[i].kind = (uint8_t)MESH_UI_DEVICE_BLE;
        devices[i].in_range = true;
        devices[i].paired = true;
    }
    mesh_ui_store_set_discovery(store, devices, count);
}

/*
 * MESH_UI_NETWORK_HOST_MAX is MESH_TCP_TARGET_MAX said twice across the UI/transport seam, the
 * way MESH_WAYPOINT_NAME_MAX is. If they drift, the keyboard either refuses an address the
 * transport would have taken or takes one it will not - and the second is silent, because
 * mesh_ui_nav_settings_commit_text()'s neighbour here cuts what does not fit.
 */
MESH_TEST_CASE(network_host_limits_agree_across_the_seam, unit) {
    MESH_TEST_FAIL_IF(MESH_UI_NETWORK_HOST_MAX != MESH_TCP_TARGET_MAX,
                      "the UI's host limit and the transport's have drifted apart");
    record_success(test_name);
}

/* With nothing discovered the tab still has a row, and it is the network one. That is the whole
   point of the row: a handheld with no Bluetooth adapter and nothing plugged in used to reach
   an empty screen, and the transport that could have served it was configurable only from a
   text editor. */
MESH_TEST_CASE(devices_network_row_is_there_with_nothing_discovered, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_devices_row row;
    const bool ok =
        (mesh_ui_devices_row_count(store.devices, store.device_count) == 1U) &&
        mesh_ui_devices_row(store.devices, store.device_count, store.network_host, 0U, &row) &&
        row.type == (uint8_t)MESH_UI_DEVICES_ROW_NETWORK && row.host[0] == '\0';
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(!ok, "an empty Devices tab should still offer the network row");
    record_success(test_name);
}

/* And it is last, under whatever discovery did turn up. */
MESH_TEST_CASE(devices_network_row_is_the_last_row, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    seed_devices(&store, 2U);
    mesh_ui_store_set_network_host(&store, "192.168.1.50");

    struct mesh_ui_devices_row row;
    bool ok = (mesh_ui_devices_row_count(store.devices, store.device_count) == 3U);
    for (uint32_t i = 0; ok && i < 2U; ++i) {
        ok = mesh_ui_devices_row(store.devices, store.device_count, store.network_host, i, &row) &&
             row.type == (uint8_t)MESH_UI_DEVICES_ROW_DEVICE && row.device == &store.devices[i];
    }
    ok = ok &&
         mesh_ui_devices_row(store.devices, store.device_count, store.network_host, 2U, &row) &&
         row.type == (uint8_t)MESH_UI_DEVICES_ROW_NETWORK && strcmp(row.host, "192.168.1.50") == 0;
    /* Past the end is past the end, with no fourth row invented. */
    ok =
        ok && !mesh_ui_devices_row(store.devices, store.device_count, store.network_host, 3U, &row);
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(!ok, "the network row should be the last row, once");
    record_success(test_name);
}

/*
 * A live network link publishes a device row of its own - it is the only row a TCP link ever
 * gets, since nothing enumerates a host - so the synthetic one stands down. Two rows about one
 * address is the failure this guards: the same host twice, one of them saying "connected" and
 * the other offering to connect to it.
 */
MESH_TEST_CASE(devices_network_row_stands_down_for_a_live_link, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_device devices[2];
    memset(devices, 0, sizeof devices);
    snprintf(devices[0].identifier, sizeof devices[0].identifier, "AA:BB:CC:DD:EE:00");
    devices[0].kind = (uint8_t)MESH_UI_DEVICE_BLE;
    snprintf(devices[1].identifier, sizeof devices[1].identifier, "192.168.1.50");
    devices[1].kind = (uint8_t)MESH_UI_DEVICE_TCP;
    devices[1].connected = true;
    mesh_ui_store_set_discovery(&store, devices, 2U);
    mesh_ui_store_set_network_host(&store, "192.168.1.50");

    struct mesh_ui_devices_row row;
    const bool ok =
        (mesh_ui_devices_row_count(store.devices, store.device_count) == 2U) &&
        mesh_ui_devices_row(store.devices, store.device_count, store.network_host, 1U, &row) &&
        row.type == (uint8_t)MESH_UI_DEVICES_ROW_DEVICE;
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(!ok, "a published network row should replace the synthetic one");
    record_success(test_name);
}

/* Walks the cursor onto the network row from the top of the Devices tab. */
static void stand_on_network_row(struct mesh_ui_store *store) {
    struct mesh_ui_action action;
    store->nav.screen = MESH_UI_SCREEN_DEVICES;
    store->nav.cursor[MESH_UI_SCREEN_DEVICES] = 0U;
    const uint32_t rows = mesh_ui_devices_row_count(store->devices, store->device_count);
    for (uint32_t i = 1U; i < rows; ++i) {
        mesh_ui_store_handle_key(store, MESH_UI_KEY_DOWN, &action);
    }
}

/*
 * A on the row with no address opens the keyboard, and Done on it asks for a connect.
 *
 * The keyboard is where the address comes from at all, so a press that did nothing until one
 * existed would be a row that could never acquire one - which is exactly the state the tab was
 * in before this row: reachable only by editing launch.sh.
 */
MESH_TEST_CASE(devices_network_row_types_an_address, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    seed_devices(&store, 1U);
    stand_on_network_row(&store);

    struct mesh_ui_action action;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.keyboard_open || !store.nav.keyboard_network ||
        action.type != MESH_UI_ACTION_NONE) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "A on an unset network row should open the keyboard");
        return;
    }

    /* Typed the way a user would have to; the draft is what Done reads. */
    snprintf(store.nav.draft, sizeof store.nav.draft, "10.0.0.7:4403");
    store.nav.kb_row = MESH_UI_KB_CHAR_ROWS;
    store.nav.kb_col = (uint8_t)MESH_UI_KB_ACTION_SEND;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);

    const bool ok = (action.type == MESH_UI_ACTION_CONNECT) &&
                    action.kind == (uint8_t)MESH_UI_DEVICE_TCP &&
                    strcmp(action.identifier, "10.0.0.7:4403") == 0 && !store.nav.keyboard_open &&
                    !store.nav.keyboard_network && store.nav.screen == MESH_UI_SCREEN_DEVICES;
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(!ok, "Done should connect to the typed address and land back on Devices");
    record_success(test_name);
}

/* With an address written down, A is the ordinary connect the rows above it offer. */
MESH_TEST_CASE(devices_network_row_connects_to_a_configured_host, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    seed_devices(&store, 1U);
    mesh_ui_store_set_network_host(&store, "192.168.1.50");
    stand_on_network_row(&store);

    struct mesh_ui_action action;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    const bool ok = (action.type == MESH_UI_ACTION_CONNECT) &&
                    action.kind == (uint8_t)MESH_UI_DEVICE_TCP &&
                    strcmp(action.identifier, "192.168.1.50") == 0 && !store.nav.keyboard_open;
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(!ok, "A on a configured network row should connect to it");
    record_success(test_name);
}

/*
 * Y edits it, preloaded - and an emptied field forgets the host.
 *
 * Clearing is the only way the user can say "stop reaching for that": the address is what
 * auto-connect retries every thirty seconds, and refusing an empty draft the way the waypoint
 * keyboard does would leave a host that nothing on the device could remove.
 */
MESH_TEST_CASE(devices_network_row_clears_to_forget, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    seed_devices(&store, 1U);
    mesh_ui_store_set_network_host(&store, "192.168.1.50");
    stand_on_network_row(&store);

    struct mesh_ui_action action;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (!store.nav.keyboard_network || strcmp(store.nav.draft, "192.168.1.50") != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "Y should open the keyboard on the address it is editing");
        return;
    }
    /* Y on the network row must not arm the forget the rest of the tab's Y arms: there is no
       bond here, and a half-armed row would answer the next Y with somebody else's press. */
    if (store.nav.devices_forget_armed) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "Y on the network row should not arm a bond forget");
        return;
    }

    store.nav.draft[0] = '\0';
    store.nav.kb_row = MESH_UI_KB_CHAR_ROWS;
    store.nav.kb_col = (uint8_t)MESH_UI_KB_ACTION_SEND;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);

    const bool ok = (action.type == MESH_UI_ACTION_FORGET) &&
                    action.kind == (uint8_t)MESH_UI_DEVICE_TCP && !store.nav.keyboard_open;
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(!ok, "an emptied address should forget the network host");
    record_success(test_name);
}

/*
 * B out of the network keyboard gives back the Compose draft it parked.
 *
 * There is one parking slot and four keyboards share it, so a flavour that forgot to restore
 * would lose whatever was being written on the Messages tab - silently, since nothing on the
 * frame says a draft existed.
 */
MESH_TEST_CASE(devices_network_keyboard_gives_back_the_draft, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    seed_devices(&store, 1U);
    snprintf(store.nav.draft, sizeof store.nav.draft, "half a message");
    stand_on_network_row(&store);

    struct mesh_ui_action action;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.draft[0] != '\0') {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the network keyboard should start empty, not on the draft");
        return;
    }
    /* B with nothing left to delete closes the keyboard. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);

    const bool ok = !store.nav.keyboard_open && !store.nav.keyboard_network &&
                    strcmp(store.nav.draft, "half a message") == 0 &&
                    store.nav.screen == MESH_UI_SCREEN_DEVICES;
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(!ok, "leaving the network keyboard should restore the Compose draft");
    record_success(test_name);
}
