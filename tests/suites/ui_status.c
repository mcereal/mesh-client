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
    mesh_ui_status_actions(&actions, false, false, false);
    MESH_TEST_FAIL_IF(actions.count != 0U, "a screen with no radio should offer no verb");

    /* Attached but still syncing: the link can be dropped, and that is all. */
    mesh_ui_status_actions(&actions, true, false, false);
    MESH_TEST_FAIL_IF(actions.count != 1U ||
                          actions.items[0].card != (uint8_t)MESH_UI_STATUS_CARD_LINK ||
                          actions.items[0].verb != (uint8_t)MESH_UI_STATUS_VERB_DISCONNECT,
                      "an attached radio should offer disconnect on the Link card");

    /* A cached configuration and no radio is still no verbs. A refresh is a request over the
       air, so offering it with the link gone is offering a press whose only outcome is a
       complaint - and it is what would otherwise slide in ahead of the cursor; see below. */
    mesh_ui_status_actions(&actions, false, true, true);
    MESH_TEST_FAIL_IF(actions.count != 0U, "a refresh with no link is a press that cannot work");

    mesh_ui_status_actions(&actions, true, true, false);
    MESH_TEST_FAIL_IF(actions.count != 2U, "a connected, synced radio offers both verbs");
    MESH_TEST_FAIL_IF(actions.items[1].card != (uint8_t)MESH_UI_STATUS_CARD_RADIO ||
                          actions.items[1].verb != (uint8_t)MESH_UI_STATUS_VERB_REFRESH,
                      "a synced radio should offer refresh on the Radio card");
    /*
     * And with readings to draw, the chart the Mesh card opens - third, because the list may
     * only grow at its end.
     *
     * The order the cursor walks is deliberately *not* the order the cards draw here, which is
     * the one thing the trend verb changed: Mesh is the middle card and its verb is last. The
     * two orders were the same while the table was two entries long, and reading that
     * coincidence as a rule is what a card-ordered list would have done - at the cost of sliding
     * this verb in ahead of refresh the moment a radio reported twice.
     */
    mesh_ui_status_actions(&actions, true, true, true);
    MESH_TEST_FAIL_IF(actions.count != 3U, "readings to draw should offer the trend as well");
    MESH_TEST_FAIL_IF(actions.items[2].card != (uint8_t)MESH_UI_STATUS_CARD_MESH ||
                          actions.items[2].verb != (uint8_t)MESH_UI_STATUS_VERB_TREND,
                      "the trend belongs to the Mesh card, whose readings it draws");

    /* A trend with no link is no verbs at all. The chart itself would be honest - the history is
       ours and outlives the radio - but offering it there is what would put it at index 0, so
       the gate is about the list rather than about the press. */
    mesh_ui_status_actions(&actions, false, false, true);
    MESH_TEST_FAIL_IF(actions.count != 0U, "a verb with no link is a verb ahead of the cursor");

    /* Every verb names a string, because the button draws it and the action bar names the same
       one. An entry with no label is a button with no word on it. */
    for (uint32_t i = 0U; i < actions.count; ++i) {
        MESH_TEST_FAIL_IF(actions.items[i].label == MESH_STR_NONE, "a verb with no word");
    }
    record_success(test_name);
}

/*
 * The list only ever grows at its end.
 *
 * The Status cursor is an index, so a verb that appeared *ahead* of it would change what the
 * next A press does without the cursor moving - a client holding a cached configuration would
 * offer refresh alone, and auto-connect arriving would slide disconnect in underneath a cursor
 * still on index 0, so a press meant to re-read the settings would drop the link that had just
 * come up. Every state the two facts can be in is walked here rather than the two that happen
 * to be reachable today, because the invariant is what a third verb has to be checked against.
 */
MESH_TEST_CASE(ui_status_verbs_only_ever_append, unit) {
    struct mesh_ui_status_actions previous;
    memset(&previous, 0, sizeof previous);

    /* Every combination of the three facts, which is what makes this a check on the invariant
       rather than on the states a radio happens to reach. */
    static const uint8_t order[] = {
        (uint8_t)MESH_UI_STATUS_VERB_DISCONNECT,
        (uint8_t)MESH_UI_STATUS_VERB_REFRESH,
        (uint8_t)MESH_UI_STATUS_VERB_TREND,
    };
    for (int state = 0; state < 8; ++state) {
        const bool connected = (state & 1) != 0;
        const bool synced = (state & 2) != 0;
        const bool has_trend = (state & 4) != 0;
        struct mesh_ui_status_actions actions;
        mesh_ui_status_actions(&actions, connected, synced, has_trend);

        /* Whatever the state, what a shorter list holds is a prefix of the longest one this can
           produce: index 0 is always disconnect, index 1 always refresh, index 2 always the
           trend. A verb that is not offered is missing from the *end*, never from the middle. */
        for (uint32_t i = 0U; i < actions.count; ++i) {
            MESH_TEST_FAIL_IF(i >= sizeof order / sizeof order[0] ||
                                  actions.items[i].verb != order[i],
                              "a verb moved to a different index, so a parked cursor now means "
                              "something else");
        }
        MESH_TEST_FAIL_IF(actions.count > MESH_UI_STATUS_ACTIONS_MAX, "more verbs than the cap");
        previous = actions;
    }
    (void)previous;
    record_success(test_name);
}

MESH_TEST_CASE(ui_status_card_shares_are_slices_of_the_list, unit) {
    struct mesh_ui_status_actions actions;
    mesh_ui_status_actions(&actions, true, true, true);

    uint32_t first = 0U;
    MESH_TEST_FAIL_IF(mesh_ui_status_card_actions(&actions, MESH_UI_STATUS_CARD_LINK, &first) !=
                              1U ||
                          first != 0U,
                      "the Link card holds the first verb");
    MESH_TEST_FAIL_IF(mesh_ui_status_card_actions(&actions, MESH_UI_STATUS_CARD_RADIO, &first) !=
                              1U ||
                          first != 1U,
                      "the Radio card holds the second");
    /* And the Mesh card the third, which is the case that proves a card's share is a slice of
       the flat list rather than a run of it: the cards draw Link, Mesh, Radio and the verbs run
       Link, Radio, Mesh. */
    MESH_TEST_FAIL_IF(mesh_ui_status_card_actions(&actions, MESH_UI_STATUS_CARD_MESH, &first) !=
                              1U ||
                          first != 2U,
                      "the Mesh card holds the trend");

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
    mesh_ui_status_actions(NULL, true, true, true);
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

    /* The radio goes away, and both verbs go with it - every one of them is a request over the
       air. The cursor has to come back to 0 rather than sit past the end of an empty list. */
    struct mesh_ui_device devices[1] = {
        {.identifier = "AA:BB:CC:DD:EE:01", .name = "NodeOne", .rssi = -45, .connected = false},
    };
    mesh_ui_store_set_discovery(&store, devices, 1U);
    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        failure = "expected a snapshot after the link dropped";
        goto cleanup;
    }
    if (mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_STATUS) != 0U) {
        failure = "a radio that has gone leaves no verb behind it";
        goto cleanup;
    }
    if (snapshot.nav.cursor[MESH_UI_SCREEN_STATUS] != 0U) {
        failure = "the cursor must be clamped back onto an empty list";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "A on a screen with no verbs must do nothing";
        goto cleanup;
    }

    /* And when it comes back, the cursor lands on the first verb rather than on whichever one
       happens to sit at the index it was left at. */
    devices[0].connected = true;
    mesh_ui_store_set_discovery(&store, devices, 1U);
    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        failure = "expected a snapshot after the link came back";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_DISCONNECT) {
        failure = "the cursor should be on the first verb when the list comes back";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The chart the third verb opens, and the way out of it.
 *
 * Three things are checked and each of them is a way a screen with no cursor goes wrong. It
 * opens on the press rather than raising an action nobody handles; the presses that walk and
 * open things are *swallowed* rather than reaching the cards underneath, where Down would move a
 * cursor nobody can see and A would run whichever verb it had moved onto; and B leaves,
 * landing back on the verb that opened it rather than at the top of the list.
 */
MESH_TEST_CASE(ui_status_trend_opens_swallows_and_closes, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    const char *failure = NULL;
    struct mesh_ui_action action;
    mesh_test_nav_populate(&store);

    /* Two airtime reports, which is what makes there be a trend at all. */
    struct mesh_ui_settings settings = store.settings;
    settings.stats.valid = true;
    settings.stats.channel_utilization = 11.0f;
    settings.stats.air_util_tx = 3.0f;
    mesh_ui_store_tick(&store, 1000U);
    mesh_ui_store_set_settings(&store, &settings);
    settings.stats.channel_utilization = 24.0f;
    mesh_ui_store_tick(&store, 2000U);
    mesh_ui_store_set_settings(&store, &settings);

    while (store.nav.screen != MESH_UI_SCREEN_STATUS) {
        const enum mesh_ui_screen before = store.nav.screen;
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
        if (store.nav.screen == before) {
            failure = "Right stopped moving before the Status tab";
            goto cleanup;
        }
    }
    if (mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_STATUS) != 3U) {
        failure = "a synced radio with two reports should offer three verbs";
        goto cleanup;
    }

    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    if (store.nav.cursor[MESH_UI_SCREEN_STATUS] != 2U) {
        failure = "Down twice should reach the Mesh card's verb";
        goto cleanup;
    }

    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (!store.nav.trend_open) {
        failure = "A on the trend verb should open the chart";
        goto cleanup;
    }
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "opening a screen we already have the readings for asks the radio for nothing";
        goto cleanup;
    }

    /* Swallowed: the cursor must not move under the picture, and A must not run the verb the
       cursor would have landed on. */
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (store.nav.cursor[MESH_UI_SCREEN_STATUS] != 2U || !store.nav.trend_open) {
        failure = "a chart has no cursor, so the d-pad must not move one";
        goto cleanup;
    }
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "A over the chart reached a verb on the cards underneath";
        goto cleanup;
    }

    /* The shoulders still change tab, and the flag stays behind on the tab it belongs to: every
       tab keeps its own place, so coming back shows the chart that was left open. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_R1, &action);
    if (store.nav.screen == MESH_UI_SCREEN_STATUS) {
        failure = "the shoulders should still walk the tab strip from a chart";
        goto cleanup;
    }
    if (!store.nav.trend_open) {
        failure = "a chart left open should still be open when its tab comes back";
        goto cleanup;
    }
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_L1, &action);

    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    if (store.nav.trend_open) {
        failure = "B should leave the chart";
        goto cleanup;
    }
    if (store.nav.cursor[MESH_UI_SCREEN_STATUS] != 2U) {
        failure = "leaving the chart should land back on the verb that opened it";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * And a chart with nothing left to draw closes itself.
 *
 * The map's clamp one screen along: a radio swap empties the history the way it empties the
 * roster, and a picture of readings nobody is holding any more is an empty frame with its axes
 * still labelled - which reads as a mesh that went perfectly quiet rather than as a screen that
 * has lost its subject.
 */
MESH_TEST_CASE(ui_status_trend_closes_when_the_history_empties, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    const char *failure = NULL;
    struct mesh_ui_action action;
    struct mesh_ui_snapshot snapshot;
    mesh_test_nav_populate(&store);

    struct mesh_ui_settings settings = store.settings;
    settings.stats.valid = true;
    settings.stats.channel_utilization = 11.0f;
    mesh_ui_store_tick(&store, 1000U);
    mesh_ui_store_set_settings(&store, &settings);
    settings.stats.channel_utilization = 24.0f;
    mesh_ui_store_tick(&store, 2000U);
    mesh_ui_store_set_settings(&store, &settings);

    /* Whose roster this is, so that the swap below is one. A first handshake is not a swap:
       there is nothing to have been left. */
    struct mesh_ui_handshake_state handshake = store.handshake;
    handshake.roster_owner = 0xAAAAU;
    mesh_ui_store_set_handshake(&store, &handshake);

    store.nav.screen = MESH_UI_SCREEN_STATUS;
    store.nav.trend_open = true;

    /* The radio is swapped, which is the event that drops the roster and the history with it. */
    handshake.roster_owner = 0xBBBBU;
    mesh_ui_store_tick(&store, 3000U);
    mesh_ui_store_set_handshake(&store, &handshake);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    (void)action;

    if (store.nav.trend_open) {
        failure = "a chart with nothing left to draw should close rather than empty";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
