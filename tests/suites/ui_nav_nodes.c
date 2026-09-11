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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * A on a reading the client has been watching opens that reading's chart; B closes it; and the
 * chart swallows everything else.
 *
 * The swallow is the half worth a test rather than a screenshot. A chart has no rows, so a press
 * that fell through would reach the detail's own cursor underneath - Down would move a cursor
 * nobody can see, and the next A would run whichever row it had landed on, which on this screen
 * includes "remove from radio".
 */
MESH_TEST_CASE(ui_nav_node_trend_opens_from_its_row, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 1U;
    handshake.nodes[0].node_id = 0x2000U;
    handshake.nodes[0].environment.valid = true;
    handshake.nodes[0].environment.has_temperature = true;
    handshake.nodes[0].environment.temperature = 21.0f;

    /* Two reports, because one reading is a level and the row only offers a chart once there is
       a line to draw. */
    mesh_ui_store_tick(&store, 1000U);
    mesh_ui_store_set_handshake(&store, &handshake);
    handshake.nodes[0].environment.temperature = 23.0f;
    mesh_ui_store_tick(&store, 2000U);
    mesh_ui_store_set_handshake(&store, &handshake);

    struct mesh_ui_nav nav;
    mesh_ui_nav_init(&nav);
    nav.screen = MESH_UI_SCREEN_NODES;
    nav.node_detail_open = true;
    nav.node_detail_node = 0x2000U;

    /* Find the temperature row the way the nav does, then stand on it. */
    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count =
        mesh_ui_node_detail_build(&handshake.nodes[0], false, 0U, NULL, false, &handshake,
                                  &store.history, items, MESH_UI_NODE_ITEMS_MAX);
    uint32_t row = count;
    for (uint32_t i = 0U; i < count; ++i) {
        if (items[i].trend_reading == MESH_UI_HISTORY_TEMPERATURE) {
            row = i;
            break;
        }
    }
    MESH_TEST_FAIL_IF(row == count, "a watched temperature should offer a chart from its row");
    nav.cursor[MESH_UI_SCREEN_NODES] = row;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_TEMPERATURE,
                      "A on the row should open that reading's chart");
    MESH_TEST_FAIL_IF(action.type != MESH_UI_ACTION_NONE,
                      "opening a chart asks the radio for nothing");

    /* The swallow: the picture has nothing to move, so Down must not reach the rows under it. */
    const uint32_t cursor = nav.cursor[MESH_UI_SCREEN_NODES];
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_DOWN, &action);
    MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != cursor,
                      "the chart should swallow the d-pad rather than walk the rows beneath it");
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_TEMPERATURE, "and stay open under it");

    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_B, &action);
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_NONE, "B should close the chart");
    MESH_TEST_FAIL_IF(!nav.node_detail_open, "and land back on the detail rather than the list");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * A chart with nothing left to draw closes itself, and closing the detail takes its chart.
 *
 * Both are the map's clamp one screen along. A picture of a reading nobody is holding any more
 * is worse than an empty list: an axis frame with its ends still labelled and no line in it
 * reads as a node that went perfectly quiet.
 */
MESH_TEST_CASE(ui_nav_node_trend_closes_when_it_empties, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.roster_owner = 0xAAAAU;
    handshake.node_count = 1U;
    handshake.nodes[0].node_id = 0x2000U;
    handshake.nodes[0].environment.valid = true;
    handshake.nodes[0].environment.has_temperature = true;
    handshake.nodes[0].environment.temperature = 21.0f;
    mesh_ui_store_tick(&store, 1000U);
    mesh_ui_store_set_handshake(&store, &handshake);

    struct mesh_ui_nav nav;
    mesh_ui_nav_init(&nav);
    nav.screen = MESH_UI_SCREEN_NODES;
    nav.node_detail_open = true;
    nav.node_detail_node = 0x2000U;
    nav.node_trend = MESH_UI_HISTORY_TEMPERATURE;

    /* A radio swap empties the history the way it empties the roster. */
    handshake.roster_owner = 0xBBBBU;
    handshake.nodes[0].environment.valid = false;
    mesh_ui_store_tick(&store, 2000U);
    mesh_ui_store_set_handshake(&store, &handshake);
    (void)mesh_ui_nav_clamp(&nav, &store);
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_NONE,
                      "a chart with no readings left should close");
    MESH_TEST_FAIL_IF(!nav.node_detail_open, "and leave the detail it was opened over");

    /* And the other way: closing the detail cannot leave a chart of it behind, or the next node
       opened would land straight on a chart of the last one's reading. */
    nav.node_trend = MESH_UI_HISTORY_TEMPERATURE;
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    nav.node_trend = MESH_UI_HISTORY_NONE;
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_B, &action);
    MESH_TEST_FAIL_IF(nav.node_detail_open, "B off the detail should close it");
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_NONE,
                      "and no chart may outlive the detail it was a level of");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* The Nodes tab's pin: X from either level, and the detail's own row. The nav sends the state
   it wants rather than a bare toggle, so a press that races a NodeInfo cannot cancel itself. */
MESH_TEST_CASE(ui_nav_node_favorite, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

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

    /* The list opens on its map row, which is about no node at all - so X there asks for
       nothing, and a step down is what reaches the first node. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "X on the map row should do nothing");
        return;
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);

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
    const uint32_t count =
        mesh_ui_node_detail_build(&store.handshake.nodes[1], false, 0U, NULL, false,
                                  &store.handshake, NULL, items, MESH_UI_NODE_ITEMS_MAX);
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
/*
 * The open node detail follows the node, not the row.
 *
 * Publication re-ranks the roster on every snapshot - a node that speaks jumps up the list,
 * and one that goes quiet slides down - so a detail that remembered "row 2" would be showing
 * a different radio a second later, with the cursor and the title still agreeing with each
 * other and both wrong. `node_detail_node` is an id and the detail is resolved through it on
 * every frame, which is also what lets the screen close itself when the node is gone.
 *
 * Pinned here because nothing else tested it and the map work ahead is where it would first
 * be quietly broken: a map selection is another index into another ordering of the same
 * roster.
 */
MESH_TEST_CASE(ui_nav_node_detail_follows_the_node, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 3U;
    handshake.nodes[0].node_id = 0x1000U;
    snprintf(handshake.nodes[0].short_name, sizeof handshake.nodes[0].short_name, "ME");
    handshake.nodes[1].node_id = 0x3000U;
    snprintf(handshake.nodes[1].short_name, sizeof handshake.nodes[1].short_name, "BRVO");
    handshake.nodes[2].node_id = 0x5000U;
    snprintf(handshake.nodes[2].short_name, sizeof handshake.nodes[2].short_name, "CHRL");
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_ui_store_consume_updates(&store, NULL);

    struct mesh_ui_action action;
    store.nav.screen = MESH_UI_SCREEN_NODES;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* past the map row */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.node_detail_open || store.nav.node_detail_node != 0x3000U) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "A on the second row should open that node");
        return;
    }

    /* The publish re-ranks: BRVO and CHRL swap places. The row under the cursor is now a
       different node, and the detail must not be. */
    handshake.nodes[1].node_id = 0x5000U;
    snprintf(handshake.nodes[1].short_name, sizeof handshake.nodes[1].short_name, "CHRL");
    handshake.nodes[2].node_id = 0x3000U;
    snprintf(handshake.nodes[2].short_name, sizeof handshake.nodes[2].short_name, "BRVO");
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_ui_store_consume_updates(&store, NULL);

    if (!store.nav.node_detail_open || store.nav.node_detail_node != 0x3000U) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "a re-ranked roster must not change which node is open");
        return;
    }
    const struct mesh_ui_node_summary *open =
        mesh_ui_node_detail_find(&store.handshake, store.nav.node_detail_node);
    if (open == NULL || strcmp(open->short_name, "BRVO") != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the detail resolved to the wrong node after a re-rank");
        return;
    }

    /* And a node that leaves the roster entirely - evicted, forgotten, or dropped with the
       radio it came from - closes the screen rather than leaving it resolving to nothing. */
    handshake.node_count = 2U;
    handshake.nodes[1].node_id = 0x5000U;
    snprintf(handshake.nodes[1].short_name, sizeof handshake.nodes[1].short_name, "CHRL");
    memset(&handshake.nodes[2], 0, sizeof handshake.nodes[2]);
    mesh_ui_store_set_handshake(&store, &handshake);
    /* Through a real snapshot, because that is where the nav is clamped: the screen closes as
       the frame is built, without waiting for a press that would otherwise land on the detail
       of a node that is no longer there. */
    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    if (snapshot == NULL) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "snapshot allocation failed");
        return;
    }
    mesh_ui_store_consume_updates(&store, snapshot);
    const bool still_open = store.nav.node_detail_open || snapshot->nav.node_detail_open;
    free(snapshot);
    if (still_open) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "a detail whose node is gone should close");
        return;
    }

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

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

    /* A message keyboard is displaced the same way and has to come back the same way. Its
       field is NONE, so the field alone cannot say one was open - and Y opens it with no
       compose overlay behind it, so anything less drops a half-typed message out of sight. */
    store.nav.screen = MESH_UI_SCREEN_MESSAGES;
    store.nav.thread_open = true;
    store.nav.keyboard_open = true;
    snprintf(store.nav.draft, sizeof store.nav.draft, "%s", "on my w");
    mesh_ui_store_open_passkey_prompt(&store, "NodePin", 0U, false);
    if (!store.nav.keyboard_passkey || store.nav.draft[0] != '\0') {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the prompt should take the message keyboard over cleanly");
        return;
    }
    mesh_ui_store_close_passkey_prompt(&store);
    if (!store.nav.keyboard_open || store.nav.keyboard_passkey ||
        store.nav.keyboard_field != (uint8_t)MESH_UI_FIELD_NONE ||
        store.nav.screen != MESH_UI_SCREEN_MESSAGES || strcmp(store.nav.draft, "on my w") != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "a displaced message keyboard should come back with its draft");
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
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);  /* past the map row */
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
    uint32_t count = mesh_ui_node_detail_build(node, false, 0U, NULL, false, &store.handshake, NULL,
                                               items, MESH_UI_NODE_ITEMS_MAX);
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
    (void)mesh_ui_node_detail_build(node, false, 0U, NULL, true, &store.handshake, NULL, items,
                                    MESH_UI_NODE_ITEMS_MAX);
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
