#define _POSIX_C_SOURCE 200809L

/*
 * What the Status cards offer.
 *
 * The table in src/ui/status.c is read by three places that have to agree - nav.c walks the
 * cursor over it and runs what it lands on, actions.c names the press, and the fb renderer
 * hangs the buttons on the cards - so the cases below check the table itself and then that a
 * card's share of it is the same list, in the same order.
 */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/ui/nav.h"
#include "mesh/ui/status.h"
#include "mesh/ui/store.h"

#include <stdio.h>
#include <string.h>

MESH_TEST_CASE(ui_status_verbs_follow_the_link, unit) {
    struct mesh_ui_status_actions actions;

    /* Nothing attached: nothing to disconnect from and no configuration to re-read, so the
       screen is the readout it has always been. */
    mesh_ui_status_actions(&actions, false, false);
    MESH_TEST_FAIL_IF(actions.count != 0U, "a screen with no radio should offer no verb");

    /* Attached but still syncing: the link can be dropped, and that is all. */
    mesh_ui_status_actions(&actions, true, false);
    MESH_TEST_FAIL_IF(actions.count != 1U ||
                          actions.items[0].card != (uint8_t)MESH_UI_STATUS_CARD_LINK ||
                          actions.items[0].verb != (uint8_t)MESH_UI_STATUS_VERB_DISCONNECT,
                      "an attached radio should offer disconnect on the Link card");

    /* A radio that has answered the handshake offers a refresh whether or not the link is up:
       what a refresh re-reads is a configuration we hold, and the app decides what to do about
       a link that has since gone. */
    mesh_ui_status_actions(&actions, false, true);
    MESH_TEST_FAIL_IF(actions.count != 1U ||
                          actions.items[0].card != (uint8_t)MESH_UI_STATUS_CARD_RADIO ||
                          actions.items[0].verb != (uint8_t)MESH_UI_STATUS_VERB_REFRESH,
                      "a synced radio should offer refresh on the Radio card");

    mesh_ui_status_actions(&actions, true, true);
    MESH_TEST_FAIL_IF(actions.count != 2U, "a connected, synced radio offers both verbs");
    /* The order is the order the cursor walks and the order the cards draw, which is the order
       the screen stacks them: Link, then Mesh, then Radio. */
    MESH_TEST_FAIL_IF(actions.items[0].card > actions.items[1].card,
                      "the flat list must run down the screen, not up it");

    /* Every verb names a string, because the button draws it and the action bar names the same
       one. An entry with no label is a button with no word on it. */
    for (uint32_t i = 0U; i < actions.count; ++i) {
        MESH_TEST_FAIL_IF(actions.items[i].label == MESH_STR_NONE, "a verb with no word");
    }
    record_success(test_name);
}

MESH_TEST_CASE(ui_status_card_shares_are_slices_of_the_list, unit) {
    struct mesh_ui_status_actions actions;
    mesh_ui_status_actions(&actions, true, true);

    uint32_t first = 0U;
    MESH_TEST_FAIL_IF(mesh_ui_status_card_actions(&actions, MESH_UI_STATUS_CARD_LINK, &first) !=
                              1U ||
                          first != 0U,
                      "the Link card holds the first verb");
    MESH_TEST_FAIL_IF(mesh_ui_status_card_actions(&actions, MESH_UI_STATUS_CARD_MESH, &first) != 0U,
                      "the Mesh card holds none");
    MESH_TEST_FAIL_IF(mesh_ui_status_card_actions(&actions, MESH_UI_STATUS_CARD_RADIO, &first) !=
                              1U ||
                          first != 1U,
                      "the Radio card holds the second");

    /* Every verb belongs to exactly one card, so the shares add up to the whole list. Without
       this a card could be dropped from the table and the cursor would still step onto its
       verb, which is a button the screen never drew. */
    uint32_t total = 0U;
    for (int c = 0; c < MESH_UI_STATUS_CARD_COUNT; ++c) {
        total += mesh_ui_status_card_actions(&actions, (enum mesh_ui_status_card)c, NULL);
    }
    MESH_TEST_FAIL_IF(total != actions.count, "a verb belongs to no card, or to two");

    /* A NULL list is an empty one rather than a crash, on the same terms as a NULL snapshot in
       mesh_ui_actions_for(). */
    MESH_TEST_FAIL_IF(mesh_ui_status_card_actions(NULL, MESH_UI_STATUS_CARD_LINK, &first) != 0U,
                      "no list means no verbs");
    mesh_ui_status_actions(NULL, true, true);
    record_success(test_name);
}

/*
 * The cursor is clamped when a verb disappears under it.
 *
 * Status is the one screen whose row count is a function of the link rather than of a list, so
 * the row that vanishes is not the last one: dropping the link takes the *first* verb away and
 * leaves the second, and a cursor left where it was would be naming a button one past the end.
 * mesh_ui_nav_clamp() runs over every screen for exactly this, and the case exists because it
 * would be easy to add a screen it does not cover.
 */
MESH_TEST_CASE(ui_status_cursor_survives_the_link_dropping, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    const char *failure = NULL;
    struct mesh_ui_action action;
    struct mesh_ui_snapshot snapshot;
    mesh_test_nav_populate(&store);

    while (store.nav.screen != MESH_UI_SCREEN_STATUS) {
        const enum mesh_ui_screen before = store.nav.screen;
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
        if (store.nav.screen == before) {
            failure = "Right stopped moving before the Status tab";
            goto cleanup;
        }
    }
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    if (store.nav.cursor[MESH_UI_SCREEN_STATUS] != 1U) {
        failure = "Down should reach the Radio card's verb";
        goto cleanup;
    }

    /* The radio goes away. Disconnect goes with it and refresh moves up to index 0. */
    struct mesh_ui_device devices[1] = {
        {.identifier = "AA:BB:CC:DD:EE:01", .name = "NodeOne", .rssi = -45, .connected = false},
    };
    mesh_ui_store_set_discovery(&store, devices, 1U);
    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        failure = "expected a snapshot after the link dropped";
        goto cleanup;
    }
    if (mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_STATUS) != 1U) {
        failure = "a radio that has gone leaves only the refresh";
        goto cleanup;
    }
    if (snapshot.nav.cursor[MESH_UI_SCREEN_STATUS] != 0U) {
        failure = "the cursor must be clamped onto the verb that is left";
        goto cleanup;
    }
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_REFRESH_SETTINGS) {
        failure = "A should run the verb the cursor was clamped onto";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
