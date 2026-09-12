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

#include "mesh/ui/history.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/status.h"
#include "mesh/ui/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Down from the Link card lands on the Mesh card, driven through the real key handler.
 *
 * Every other case here asks mesh_ui_status_actions() the three booleans directly, which is the
 * table's own question and answers it correctly whatever the client is actually holding. What
 * none of them could see is the step before: whether a radio reporting the way a radio really
 * reports ever *produces* the third boolean. It did not - MESH_UI_HISTORY_RADIO_GAP_MS was one
 * LocalStats interval, so every sample started a segment, and the verb this file spent four
 * cases on was never once offered on hardware. The table was right and the screen was dead.
 *
 * So this one starts at the store, pushes the radio's report at the cadence the firmware sends
 * it, and presses Down. It is the only case here that would have caught that.
 */
MESH_TEST_CASE(status_cursor_reaches_the_mesh_card_from_a_radios_reports, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    MESH_TEST_FAIL_IF(!mesh_test_open_tab(&store, MESH_UI_SCREEN_STATUS), "no Status tab");

    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    struct mesh_ui_action action;
    uint64_t now = 1000U;

    /* One report in, and the card is still not offered: one reading is a level, not a trend. */
    now += MESH_UI_HISTORY_RADIO_REPORT_MS;
    mesh_ui_store_tick(&store, now);
    settings.stats.valid = true;
    settings.stats.uptime_seconds = 100U;
    settings.stats.channel_utilization = 5.0f;
    settings.stats.air_util_tx = 1.0f;
    mesh_ui_store_set_settings(&store, &settings);
    store.nav.status_verb = (uint8_t)MESH_UI_STATUS_VERB_DISCONNECT;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    MESH_TEST_FAIL_IF(store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_REFRESH,
                      "one reading offers no chart, so Down goes on to the Radio card");

    /* A second, at the interval the radio really sends them - the throttle is a quarter of an
       hour and it is tested a tick late, so this is the spacing hardware produces. */
    now += MESH_UI_HISTORY_RADIO_REPORT_MS + 60U * 1000U;
    mesh_ui_store_tick(&store, now);
    settings.stats.uptime_seconds = 1060U;
    settings.stats.channel_utilization = 7.0f;
    settings.stats.air_util_tx = 2.0f;
    mesh_ui_store_set_settings(&store, &settings);

    store.nav.status_verb = (uint8_t)MESH_UI_STATUS_VERB_DISCONNECT;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    MESH_TEST_FAIL_IF(store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_TREND,
                      "two reports at the radio's own cadence should put Down on the Mesh card");

    /* And A on it opens the chart rather than asking the radio for anything. */
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(!store.nav.trend_open, "A on the Mesh card should open the chart");
    record_success(test_name);
}

/*
 * And it is offered straight away on the run after, which is what persisting the trend bought.
 *
 * Without it the history starts empty at every launch, the radio's report is a quarter of an
 * hour apart, and the Mesh card is dead for the first half hour of every session - which on a
 * handheld picked up for a few minutes is a card that never works at all.
 */
MESH_TEST_CASE(status_mesh_card_is_live_on_the_run_after, unit) {
    char path[] = "/tmp/mesh_ui_status_relaunchXXXXXX";
    const int fd = mkstemp(path);
    MESH_TEST_FAIL_IF(fd < 0, "could not make a cache path");
    close(fd);

    struct mesh_ui_store first;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&first) != 0, "store init failed");
    mesh_test_nav_populate(&first);
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    uint64_t now = 1000U;
    for (uint32_t i = 0U; i < 3U; ++i) {
        now += MESH_UI_HISTORY_RADIO_REPORT_MS + 60U * 1000U;
        mesh_ui_store_tick(&first, now);
        settings.stats.valid = true;
        settings.stats.uptime_seconds = 100U + i * 960U;
        settings.stats.channel_utilization = 5.0f + (float)i;
        settings.stats.air_util_tx = 1.0f + (float)i;
        mesh_ui_store_set_settings(&first, &settings);
    }
    MESH_TEST_FAIL_IF(mesh_ui_store_save(&first, path) != 0, "save failed");

    /* The next launch: the cache, the devices, and not one word from the radio yet. */
    struct mesh_ui_store next;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&next) != 0, "store init failed");
    MESH_TEST_FAIL_IF(mesh_ui_store_load(&next, path) != 0, "load failed");
    mesh_test_nav_populate(&next);
    MESH_TEST_FAIL_IF(!mesh_test_open_tab(&next, MESH_UI_SCREEN_STATUS), "no Status tab");

    struct mesh_ui_action action;
    next.nav.status_verb = (uint8_t)MESH_UI_STATUS_VERB_DISCONNECT;
    mesh_ui_store_handle_key(&next, MESH_UI_KEY_DOWN, &action);
    MESH_TEST_FAIL_IF(next.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_TREND,
                      "a relaunch should reach the Mesh card before the radio reports again");

    remove(path);
    record_success(test_name);
}

MESH_TEST_CASE(ui_status_verbs_follow_the_link, unit) {
    struct mesh_ui_status_actions actions;

    /* Nothing attached and nothing watched: the screen is the readout it has always been. */
    mesh_ui_status_actions(&actions, false, false, false);
    MESH_TEST_FAIL_IF(actions.count != 0U, "a screen with nothing to act on should offer no verb");

    /* Attached but still syncing: the link can be dropped, and that is all. */
    mesh_ui_status_actions(&actions, true, false, false);
    MESH_TEST_FAIL_IF(actions.count != 1U ||
                          actions.items[0].card != (uint8_t)MESH_UI_STATUS_CARD_LINK ||
                          actions.items[0].verb != (uint8_t)MESH_UI_STATUS_VERB_DISCONNECT,
                      "an attached radio should offer disconnect on the Link card");

    /* A cached configuration and no radio is still no refresh. That one is a request over the
       air, so offering it with the link gone is offering a press whose only outcome is a
       complaint. */
    mesh_ui_status_actions(&actions, false, true, false);
    MESH_TEST_FAIL_IF(actions.count != 0U, "a refresh with no link is a press that cannot work");

    mesh_ui_status_actions(&actions, true, true, false);
    MESH_TEST_FAIL_IF(actions.count != 2U, "a connected, synced radio offers both verbs");
    MESH_TEST_FAIL_IF(actions.items[1].card != (uint8_t)MESH_UI_STATUS_CARD_RADIO ||
                          actions.items[1].verb != (uint8_t)MESH_UI_STATUS_VERB_REFRESH,
                      "a synced radio should offer refresh on the Radio card");

    /*
     * And with readings to draw, the chart the Mesh card opens - *second*, because Mesh is the
     * middle card and this list is written in the order the cards draw.
     *
     * That is the change the verb-keyed cursor bought. While the cursor was an index the list
     * had to be append-only, so the verb that arrived last had to go last whatever card it
     * belonged to, and Down walked Link, Radio, Mesh - down to the bottom card and then back up
     * to the middle one.
     */
    mesh_ui_status_actions(&actions, true, true, true);
    MESH_TEST_FAIL_IF(actions.count != 3U, "readings to draw should offer the trend as well");
    MESH_TEST_FAIL_IF(actions.items[1].card != (uint8_t)MESH_UI_STATUS_CARD_MESH ||
                          actions.items[1].verb != (uint8_t)MESH_UI_STATUS_VERB_TREND,
                      "the trend belongs to the Mesh card, whose readings it draws");

    /*
     * A trend with no link is the trend on its own, and that is the honest condition rather
     * than a relaxation.
     *
     * What the verb opens is a picture of what this client watched, which outlives the radio
     * going away - the history is ours. It was gated on the link anyway while the cursor was an
     * index, because a verb whose condition did not imply its neighbours' could slide in ahead
     * of a parked cursor. Nothing slides now: the cursor names a verb.
     */
    mesh_ui_status_actions(&actions, false, false, true);
    MESH_TEST_FAIL_IF(actions.count != 1U ||
                          actions.items[0].verb != (uint8_t)MESH_UI_STATUS_VERB_TREND,
                      "a trend outlives the radio, and so does the verb that opens it");

    /* Every verb names a string, because the button draws it and the action bar names the same
       one. An entry with no label is a button with no word on it. */
    mesh_ui_status_actions(&actions, true, true, true);
    for (uint32_t i = 0U; i < actions.count; ++i) {
        MESH_TEST_FAIL_IF(actions.items[i].label == MESH_STR_NONE, "a verb with no word");
    }
    record_success(test_name);
}

/*
 * Whatever the state, the list is the table in the table's own order.
 *
 * This is what replaced "the list only ever grows at its end", and it is a weaker rule doing a
 * stronger job. The old one existed because the cursor was an index: a verb appearing *ahead*
 * of it changed what the next A press did without the cursor moving, so every verb had to be
 * gated on the verbs before it and the list could only ever be a prefix. The cursor names a
 * verb now, so a verb may arrive or leave anywhere - what still may not happen is two verbs
 * swapping places, because the order the cursor walks is the order the cards draw and a reader
 * watching the highlight move is entitled to have it move down the screen.
 *
 * Every combination of the three facts is walked rather than the ones a radio happens to reach,
 * because the invariant is what a fourth verb has to be checked against.
 */
MESH_TEST_CASE(ui_status_verbs_stay_in_card_order, unit) {
    /* The order the cards draw, which is the order the table is written in. */
    static const uint8_t order[] = {
        (uint8_t)MESH_UI_STATUS_VERB_DISCONNECT,
        (uint8_t)MESH_UI_STATUS_VERB_TREND,
        (uint8_t)MESH_UI_STATUS_VERB_REFRESH,
    };

    for (int state = 0; state < 8; ++state) {
        const bool connected = (state & 1) != 0;
        const bool synced = (state & 2) != 0;
        const bool has_trend = (state & 4) != 0;
        struct mesh_ui_status_actions actions;
        mesh_ui_status_actions(&actions, connected, synced, has_trend);

        MESH_TEST_FAIL_IF(actions.count > MESH_UI_STATUS_ACTIONS_MAX, "more verbs than the cap");

        /* A subsequence of the table: every verb offered appears in it, once, after the one
           offered before it. A card ordering that held only in the fully-reported state would
           be exactly the coincidence the old prefix rule was read off. */
        uint32_t at = 0U;
        for (uint32_t i = 0U; i < actions.count; ++i) {
            uint32_t rank = (uint32_t)(sizeof order / sizeof order[0]);
            for (uint32_t r = at; r < sizeof order / sizeof order[0]; ++r) {
                if (order[r] == actions.items[i].verb) {
                    rank = r;
                    break;
                }
            }
            MESH_TEST_FAIL_IF(rank >= sizeof order / sizeof order[0],
                              "a verb the cards do not draw, or one drawn out of order");
            at = rank + 1U;
        }

        /* And each verb's own condition, stated once here so the table cannot quietly grow a
           dependency on its neighbours again. */
        MESH_TEST_FAIL_IF((mesh_ui_status_find(&actions, MESH_UI_STATUS_VERB_DISCONNECT) != NULL) !=
                              connected,
                          "disconnect is offered exactly when there is a link to drop");
        MESH_TEST_FAIL_IF((mesh_ui_status_find(&actions, MESH_UI_STATUS_VERB_REFRESH) != NULL) !=
                              (connected && synced),
                          "refresh is offered exactly when there is a radio to ask");
        MESH_TEST_FAIL_IF((mesh_ui_status_find(&actions, MESH_UI_STATUS_VERB_TREND) != NULL) !=
                              has_trend,
                          "the trend is offered exactly when there is a line to draw");
    }
    record_success(test_name);
}

/*
 * A cursor holding a verb lands somewhere sensible whatever the list does under it.
 *
 * The whole of what makes the list free to change shape. Four cases and each is one the index
 * could not answer: the verb is still there, so nothing moves; it has gone and something above
 * it survives, so the cursor steps up rather than to the top; it has gone and nothing above it
 * has, so the cursor takes the first survivor; and there is nothing on offer at all, where the
 * remembered verb is kept because a screen drawing no buttons has nothing to be wrong about -
 * which is what puts the reader back on their own button when the radio returns.
 */
MESH_TEST_CASE(ui_status_cursor_resolves_to_a_verb_on_offer, unit) {
    struct mesh_ui_status_actions all;
    mesh_ui_status_actions(&all, true, true, true);
    MESH_TEST_FAIL_IF(mesh_ui_status_verb_resolve(&all, MESH_UI_STATUS_VERB_REFRESH) !=
                          MESH_UI_STATUS_VERB_REFRESH,
                      "a verb still on offer is where the cursor stays");

    /* The radio goes away with the cursor on refresh, and the trend it was reading stays. The
       nearest verb above refresh is that trend, on the card just up the screen. */
    struct mesh_ui_status_actions trend_only;
    mesh_ui_status_actions(&trend_only, false, false, true);
    MESH_TEST_FAIL_IF(mesh_ui_status_verb_resolve(&trend_only, MESH_UI_STATUS_VERB_REFRESH) !=
                          MESH_UI_STATUS_VERB_TREND,
                      "a cursor whose verb has gone steps to the nearest one above it");

    /* And with the cursor on the first verb of a list that has lost it, the first survivor. */
    MESH_TEST_FAIL_IF(mesh_ui_status_verb_resolve(&trend_only, MESH_UI_STATUS_VERB_DISCONNECT) !=
                          MESH_UI_STATUS_VERB_TREND,
                      "nothing above it means the first verb on offer");

    struct mesh_ui_status_actions none;
    mesh_ui_status_actions(&none, false, false, false);
    MESH_TEST_FAIL_IF(mesh_ui_status_verb_resolve(&none, MESH_UI_STATUS_VERB_REFRESH) !=
                          MESH_UI_STATUS_VERB_REFRESH,
                      "an empty screen keeps the reader's place rather than resetting it");
    MESH_TEST_FAIL_IF(mesh_ui_status_find(&none, MESH_UI_STATUS_VERB_REFRESH) != NULL,
                      "and a kept place is still not a verb on offer");

    /* Stepping walks the list on offer and stops at its ends rather than wrapping - the same
       answer every other cursor in this client gives a press at the end of a list. */
    MESH_TEST_FAIL_IF(mesh_ui_status_verb_step(&all, MESH_UI_STATUS_VERB_DISCONNECT, +1) !=
                          MESH_UI_STATUS_VERB_TREND,
                      "Down from the Link card reaches the Mesh card");
    MESH_TEST_FAIL_IF(mesh_ui_status_verb_step(&all, MESH_UI_STATUS_VERB_TREND, +1) !=
                          MESH_UI_STATUS_VERB_REFRESH,
                      "and then the Radio card");
    MESH_TEST_FAIL_IF(mesh_ui_status_verb_step(&all, MESH_UI_STATUS_VERB_REFRESH, +1) !=
                          MESH_UI_STATUS_VERB_REFRESH,
                      "and stops there");
    MESH_TEST_FAIL_IF(mesh_ui_status_verb_step(&all, MESH_UI_STATUS_VERB_DISCONNECT, -1) !=
                          MESH_UI_STATUS_VERB_DISCONNECT,
                      "Up at the top of the list is not a move");
    /* Stepping from a verb that has gone starts from where the cursor is *drawn*, not from
       where it remembers being - the reader is moving away from a highlight they can see. */
    MESH_TEST_FAIL_IF(mesh_ui_status_verb_step(&trend_only, MESH_UI_STATUS_VERB_REFRESH, -1) !=
                          MESH_UI_STATUS_VERB_TREND,
                      "a step from a departed verb starts at the button on the frame");
    MESH_TEST_FAIL_IF(mesh_ui_status_verb_step(NULL, MESH_UI_STATUS_VERB_TREND, +1) !=
                          MESH_UI_STATUS_VERB_TREND,
                      "no list is no move");
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
    /* The Mesh card second and the Radio card third, which is the cards' own order - the flat
       list and the column now run the same way, and a card's share is a run of it. */
    MESH_TEST_FAIL_IF(mesh_ui_status_card_actions(&actions, MESH_UI_STATUS_CARD_MESH, &first) !=
                              1U ||
                          first != 1U,
                      "the Mesh card holds the trend");
    MESH_TEST_FAIL_IF(mesh_ui_status_card_actions(&actions, MESH_UI_STATUS_CARD_RADIO, &first) !=
                              1U ||
                          first != 2U,
                      "the Radio card holds the refresh");
    /* Which is a property of the table rather than of this state, so it is checked as one:
       walking the cards in the order they draw walks the list in the order it is offered. */
    uint32_t walked = 0U;
    for (int c = 0; c < MESH_UI_STATUS_CARD_COUNT; ++c) {
        uint32_t at = 0U;
        const uint32_t held =
            mesh_ui_status_card_actions(&actions, (enum mesh_ui_status_card)c, &at);
        if (held == 0U) {
            continue;
        }
        MESH_TEST_FAIL_IF(at != walked, "a card's verbs are not where the column draws them");
        walked += held;
    }

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
 * The cursor keeps its verb while the list changes shape under it.
 *
 * Status is the one screen whose list is a function of the link rather than of a roster, so the
 * entry that vanishes is not the last one: dropping the link takes the *first* verb away and
 * can leave the trend behind it. An index left where it was would name a different button; a
 * verb cannot, which is the whole of what this cursor is for.
 *
 * Three things are checked here and the last two were not possible before. A verb still on
 * offer keeps the cursor exactly where it was. A screen with nothing to offer holds the
 * reader's place rather than resetting it, so the radio coming back lands them on the button
 * they were standing on. And cursor[STATUS] takes no part in any of it - it is pinned at 0
 * because this screen has no rows, and a future reader picking it up would be reading a
 * position nothing maintains.
 */
MESH_TEST_CASE(ui_status_cursor_keeps_its_verb, unit) {
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
    if (store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_REFRESH) {
        failure = "Down should reach the Radio card's verb";
        goto cleanup;
    }
    if (store.nav.cursor[MESH_UI_SCREEN_STATUS] != 0U) {
        failure = "the Status cursor is a verb; the row cursor must stay out of it";
        goto cleanup;
    }

    /* The radio goes away, and both of its verbs with it - each is a request over the air. */
    struct mesh_ui_device devices[1] = {
        {.identifier = "AA:BB:CC:DD:EE:01", .name = "NodeOne", .rssi = -45, .connected = false},
    };
    mesh_ui_store_set_discovery(&store, devices, 1U);
    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        failure = "expected a snapshot after the link dropped";
        goto cleanup;
    }
    if (mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_STATUS) != 0U) {
        failure = "a radio that has gone and nothing watched leaves no verb behind it";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "A on a screen with no verbs must do nothing";
        goto cleanup;
    }
    /* Nor may a press that reaches nothing move the place being held. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    if (store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_REFRESH) {
        failure = "a screen with no buttons should hold the reader's place";
        goto cleanup;
    }

    /* And when it comes back, the cursor is on the verb it was left on rather than at the top
       of a list it never chose to be at the top of. */
    devices[0].connected = true;
    mesh_ui_store_set_discovery(&store, devices, 1U);
    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        failure = "expected a snapshot after the link came back";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_REFRESH_SETTINGS) {
        failure = "the cursor should come back on the verb it was left on";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * And a verb arriving ahead of the cursor changes nothing about what A does.
 *
 * The case the old index could not survive, and the reason §2.20 called this the change that
 * unblocks the screen. The cursor sits on the Radio card's refresh; the radio then reports
 * airtime twice, which puts a trend verb on the Mesh card - one card *up* the screen, so as an
 * index it would slide underneath the cursor and the next A press would open a chart instead
 * of re-reading the configuration.
 */
MESH_TEST_CASE(ui_status_a_new_verb_does_not_move_the_cursor, unit) {
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
    if (store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_REFRESH) {
        failure = "Down should reach the Radio card's verb";
        goto cleanup;
    }

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
    (void)mesh_ui_store_consume_updates(&store, &snapshot);

    if (mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_STATUS) != 3U) {
        failure = "the readings should have added a verb";
        goto cleanup;
    }
    if (store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_REFRESH) {
        failure = "a verb arriving above the cursor moved it";
        goto cleanup;
    }
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_REFRESH_SETTINGS || store.nav.trend_open) {
        failure = "A ran the verb that slid in rather than the one under the cursor";
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

    /* One press, because Mesh is the middle card and the list runs in the cards' order. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    if (store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_TREND) {
        failure = "Down should reach the Mesh card's verb";
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
    if (store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_TREND || !store.nav.trend_open) {
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
    if (store.nav.status_verb != (uint8_t)MESH_UI_STATUS_VERB_TREND) {
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
