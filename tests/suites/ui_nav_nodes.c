#define _POSIX_C_SOURCE 200809L

/* Navigating nodes and devices: favorites, disconnect/forget, PIN prompts. */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The Nodes tab's pin: X from either level, and the detail's own row. The nav sends the state
   it wants rather than a bare toggle, so a press that races a NodeInfo cannot cancel itself. */
MESH_TEST_CASE(ui_nav_node_favorite, unit) {
    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 2U;
    handshake.nodes[0].node_id = 0x1000U;
    snprintf(handshake.nodes[0].short_name, sizeof handshake.nodes[0].short_name, "ME");
    handshake.nodes[1].node_id = 0x3000U;
    snprintf(handshake.nodes[1].short_name, sizeof handshake.nodes[1].short_name, "BRVO");
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_ui_store_consume_updates(&store, NULL);

    struct mesh_ui_action action;
    store.nav.screen = MESH_UI_SCREEN_NODES;

    /* Our own node cannot be pinned: it already outranks everything. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "X on our own node should do nothing");
        return;
    }

    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (action.type != MESH_UI_ACTION_TOGGLE_FAVORITE || action.dest != 0x3000U ||
        action.number != 1U) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "X on the node list should ask for a pin");
        return;
    }

    /* Once the app has flipped the flag, the same press asks for the opposite. */
    handshake.nodes[1].is_favorite = true;
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_ui_store_consume_updates(&store, NULL);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (action.type != MESH_UI_ACTION_TOGGLE_FAVORITE || action.number != 0U) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "X on a pinned node should ask for an unpin");
        return;
    }

    /* And the detail's own row does the same thing, wherever it happens to sit. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* open the detail */
    if (!store.nav.node_detail_open) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "A should open the detail");
        return;
    }
    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(&store.handshake.nodes[1], false, 0U, NULL,
                                                     false, items, MESH_UI_NODE_ITEMS_MAX);
    uint32_t favorite_row = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].action == MESH_UI_NODE_ACTION_FAVORITE) {
            favorite_row = i;
        }
    }
    if (favorite_row >= count || strcmp(items[favorite_row].value, "yes") != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the detail should carry a pin row showing the current state");
        return;
    }
    for (uint32_t i = 0; i < favorite_row; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_TOGGLE_FAVORITE || action.dest != 0x3000U ||
        action.number != 0U || store.nav.thread_open) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "A on the pin row should ask for an unpin, not open a thread");
        return;
    }

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* X and Y on the Devices tab: drop the link, and forget a bond on the second press. */
MESH_TEST_CASE(ui_nav_devices_disconnect_forget, unit) {
    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }

    const struct mesh_ui_device devices[] = {
        {.identifier = "AA:BB:CC:DD:EE:01",
         .name = "NodeOne",
         .rssi = -45,
         .connected = true,
         .paired = true,
         .kind = (uint8_t)MESH_UI_DEVICE_BLE},
        {.identifier = "/dev/ttyUSB0",
         .name = "USB node",
         .connected = false,
         .paired = true,
         .kind = (uint8_t)MESH_UI_DEVICE_SERIAL},
    };
    mesh_ui_store_set_discovery(&store, devices, sizeof devices / sizeof devices[0]);
    mesh_ui_store_consume_updates(&store, NULL);
    store.nav.screen = MESH_UI_SCREEN_DEVICES;

    struct mesh_ui_action action;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (action.type != MESH_UI_ACTION_DISCONNECT ||
        strcmp(action.identifier, devices[0].identifier) != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "X should drop the connected link");
        return;
    }

    /* One press of Y only arms it: a bond dropped by accident costs a re-pair. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (action.type != MESH_UI_ACTION_NONE || !store.nav.devices_forget_armed) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the first Y should only arm the forget");
        return;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (action.type != MESH_UI_ACTION_FORGET ||
        strcmp(action.identifier, devices[0].identifier) != 0 || store.nav.devices_forget_armed) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the second Y should forget the node");
        return;
    }

    /* Anything else stands it down, and a USB port has no bond to forget at all. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    if (store.nav.devices_forget_armed) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "moving the cursor should stand the forget down");
        return;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "a USB port has nothing to forget");
        return;
    }

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* The PIN prompt: raised by the app mid-connect, answered (or cancelled) from the keyboard. */
MESH_TEST_CASE(ui_nav_passkey_prompt, unit) {
    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }

    /* Something half-written in the compose draft must survive a prompt landing on top of it. */
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "half a message");
    mesh_ui_store_open_passkey_prompt(&store, "NodePin", 0U, false);
    if (!store.nav.keyboard_open || !store.nav.keyboard_passkey || store.nav.draft[0] != '\0') {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the prompt should open an empty keyboard");
        return;
    }

    /* Row 0 of every layer is the digits, so the cursor starts on one. */
    struct mesh_ui_action action;
    const char *pin = "632090";
    for (const char *c = pin; *c != '\0'; ++c) {
        const uint8_t col = (uint8_t)((*c == '0') ? 9 : (*c - '1'));
        store.nav.kb_row = 0U;
        store.nav.kb_col = col;
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    }
    if (strcmp(store.nav.draft, pin) != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the digits should land in the draft");
        return;
    }

    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_SUBMIT_PASSKEY || strcmp(action.text, pin) != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "Send should submit the PIN");
        return;
    }
    if (store.nav.keyboard_open || store.nav.keyboard_passkey) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the prompt should close once answered");
        return;
    }
    if (strcmp(store.nav.draft, "half a message") != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the parked compose draft should come back");
        return;
    }

    /* A seventh digit is refused: BlueZ passkeys stop at 999999, so it could only produce a
       pairing failure the user cannot see the cause of. */
    mesh_ui_store_open_passkey_prompt(&store, "NodePin", 0U, false);
    for (int i = 0; i < 8; ++i) {
        store.nav.kb_row = 0U;
        store.nav.kb_col = 0U; /* "1" */
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    }
    if (strcmp(store.nav.draft, "111111") != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the prompt should stop at six digits");
        return;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_SUBMIT_PASSKEY || strcmp(action.text, "111111") != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "six digits should be what is submitted");
        return;
    }

    /* Landing on top of an open keyboard gives it back afterwards, text and target both. */
    store.nav.keyboard_open = true;
    store.nav.keyboard_field = MESH_UI_FIELD_USER_LONG_NAME;
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "Base Camp");
    mesh_ui_store_open_passkey_prompt(&store, "NodePin", 0U, false);
    if (store.nav.keyboard_field != MESH_UI_FIELD_NONE || store.nav.draft[0] != '\0') {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the prompt should take the keyboard over cleanly");
        return;
    }
    mesh_ui_store_close_passkey_prompt(&store);
    if (!store.nav.keyboard_open ||
        store.nav.keyboard_field != (uint8_t)MESH_UI_FIELD_USER_LONG_NAME ||
        strcmp(store.nav.draft, "Base Camp") != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the displaced keyboard should come back");
        return;
    }
    mesh_ui_nav_init(&store.nav);

    /* B with nothing typed abandons the bond rather than silently leaving BlueZ waiting. */
    mesh_ui_store_open_passkey_prompt(&store, "NodePin", 0U, false);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (action.type != MESH_UI_ACTION_CANCEL_PAIRING || store.nav.keyboard_passkey) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "B should cancel the pairing");
        return;
    }

    /* Numeric comparison: the number is pre-filled, so Send is the whole answer. */
    mesh_ui_store_open_passkey_prompt(&store, "NodePin", 123456U, true);
    if (!store.nav.pairing_confirm || strcmp(store.nav.draft, "123456") != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "a confirmation should pre-fill its digits");
        return;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    if (action.type != MESH_UI_ACTION_SUBMIT_PASSKEY || strcmp(action.text, "123456") != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "Send should confirm the displayed number");
        return;
    }

    /* And the app taking the prompt down (BlueZ gave up, say) leaves nothing behind. */
    mesh_ui_store_open_passkey_prompt(&store, "NodePin", 0U, false);
    mesh_ui_store_close_passkey_prompt(&store);
    if (store.nav.keyboard_open || store.nav.keyboard_passkey) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "closing the prompt should close the keyboard");
        return;
    }

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * The node detail's two new rows. Mute is a bare toggle because the admin verb is one; remove
 * is the only row on the tab that arms first, because it is the only one that takes its own
 * row away with it.
 */
MESH_TEST_CASE(ui_nav_node_mute_remove, unit) {
    const char *failure = NULL;

    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        record_failure(test_name, "store init failed");
        return;
    }
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action); /* Nodes */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);  /* a node that is not us */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.node_detail_open) {
        failure = "A should open the node detail";
        goto cleanup;
    }
    const struct mesh_ui_node_summary *node =
        mesh_ui_node_detail_find(&store.handshake, store.nav.node_detail_node);
    if (node == NULL) {
        failure = "the open node should be findable";
        goto cleanup;
    }

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    uint32_t count =
        mesh_ui_node_detail_build(node, false, 0U, NULL, false, items, MESH_UI_NODE_ITEMS_MAX);
    uint32_t mute_row = count;
    uint32_t remove_row = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].action == MESH_UI_NODE_ACTION_MUTE) {
            mute_row = i;
        }
        if (items[i].action == MESH_UI_NODE_ACTION_REMOVE) {
            remove_row = i;
        }
    }
    if (mute_row >= count || remove_row >= count || remove_row < mute_row ||
        strcmp(items[remove_row].value, "press A") != 0) {
        failure = "the detail should end with mute and then remove";
        goto cleanup;
    }
    /* The armed spelling is the only thing the flag changes. */
    (void)mesh_ui_node_detail_build(node, false, 0U, NULL, true, items, MESH_UI_NODE_ITEMS_MAX);
    if (strcmp(items[remove_row].value, "A again to remove") != 0) {
        failure = "arming should change what the remove row says";
        goto cleanup;
    }

    for (uint32_t i = 0; i < mute_row; ++i) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_TOGGLE_MUTE || action.dest != node->node_id) {
        failure = "A on the mute row should emit a toggle for that node";
        goto cleanup;
    }

    /* Remove: the first press only arms, and moving off the row stands it down again. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.node_remove_armed || action.type != MESH_UI_ACTION_NONE) {
        failure = "the first press on remove should only arm it";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    if (store.nav.node_remove_armed) {
        failure = "moving off the remove row should stand it down";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.node_remove_armed || action.type != MESH_UI_ACTION_REMOVE_NODE ||
        action.dest != node->node_id) {
        failure = "the second press should emit the removal";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}
