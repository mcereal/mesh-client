#define _POSIX_C_SOURCE 200809L

/* Navigating nodes and devices: favorites, disconnect/forget, PIN prompts. */

#include "framework/mesh_test.h"
#include "mesh/i18n/strings.h"
#include "support/ui_fixture.h"

/* For enum mesh_traceroute_state, which the UI's traceroute carries as a byte. */
#include "mesh/core/session.h"
#include "mesh/ui/actions.h"
#include "mesh/ui/history.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/nodes.h"
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
        mesh_ui_node_detail_build(&handshake.nodes[0], false, 0U, NULL, &handshake, &store.history,
                                  false, items, MESH_UI_NODE_ITEMS_MAX);
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
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_TEMPERATURE,
                      "A on the row should open that reading's chart");
    MESH_TEST_FAIL_IF(action.type != MESH_UI_ACTION_NONE,
                      "opening a chart asks the radio for nothing");

    /* The swallow: the picture has nothing to move, so Down must not reach the rows under it. */
    const uint32_t cursor = nav.cursor[MESH_UI_SCREEN_NODES];
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_DOWN, &action);
    MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != cursor,
                      "the chart should swallow the d-pad rather than walk the rows beneath it");
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_TEMPERATURE, "and stay open under it");

    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_B, &action);
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_NONE, "B should close the chart");
    MESH_TEST_FAIL_IF(!nav.node_detail_open, "and land back on the detail rather than the list");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * Closing the chart lands back on the row it was opened from.
 *
 * The regression is a clamp away rather than a press away, which is what made it invisible: a
 * chart is a picture, so the obvious thing for the Nodes tab's row count to answer while one is
 * open is zero - and the clamp's empty-list arm reads a zero as "put the cursor back to the top".
 * mesh_ui_store_consume_updates() clamps on the publish right after the press, so by the time B
 * was pressed the detail's cursor had already been reset and every chart exited onto "Message
 * this node". The map gets away with answering zero because it parks the list position in
 * node_list_cursor; a chart parks nothing, because the cursor it stands on is the detail's own.
 */
/*
 * Y turns a node's chart into the readings behind it, and Up and Down move through them.
 *
 * Three things in one case because they are one gesture: the press exists, the scroll it enables
 * does something, and the scroll is held to what there is - and a toggle that flipped a flag
 * nothing scrolled would pass two of those separately while being useless.
 *
 * The clamp is the half that cannot be tested by pressing. It runs on the publish after the
 * press, against the readings in the span and the body rows the backend last reported, so a
 * scroll pushed past the end comes back rather than leaving the list drawing off the bottom of a
 * window it was never in.
 */
MESH_TEST_CASE(ui_nav_node_trend_lists_its_readings, unit) {
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

    /* Eight readings, so there is more of the list than the four body rows below pretend to
       hold - a list that fits has nothing to scroll and would pass this by doing nothing. */
    for (uint32_t i = 0U; i < 8U; ++i) {
        handshake.nodes[0].environment.temperature = 20.0f + (float)i;
        mesh_ui_store_tick(&store, 1000U + i * 1000U);
        mesh_ui_store_set_handshake(&store, &handshake);
    }
    /* What the backend would have said its body holds. The clamp reads it, so without it the
       list can be scrolled anywhere - see `trend_scroll`. */
    mesh_ui_store_set_page_rows(&store, 4U);

    struct mesh_ui_nav nav;
    mesh_ui_nav_init(&nav);
    nav.screen = MESH_UI_SCREEN_NODES;
    nav.node_detail_open = true;
    nav.node_detail_node = 0x2000U;
    nav.node_trend = MESH_UI_HISTORY_TEMPERATURE;

    const char *failure = NULL;
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);

    if (nav.trend_table) {
        failure = "a chart opens as a picture";
        goto cleanup;
    }
    /* Up and Down do nothing while the picture is up, and are still swallowed rather than
       reaching the rows underneath. */
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_DOWN, &action);
    if (nav.trend_scroll != 0U) {
        failure = "a picture has nothing to scroll";
        goto cleanup;
    }

    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_Y, &action);
    if (!nav.trend_table) {
        failure = "Y should turn the chart into its readings";
        goto cleanup;
    }
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "listing the readings asks the radio for nothing";
        goto cleanup;
    }

    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_DOWN, &action);
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_DOWN, &action);
    if (nav.trend_scroll != 2U) {
        failure = "Down should move the window over the readings";
        goto cleanup;
    }
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_UP, &action);
    if (nav.trend_scroll != 1U) {
        failure = "and Up should move it back";
        goto cleanup;
    }

    /* Past the end, then clamped: eight readings in a window of four leaves four to scroll. */
    for (uint32_t i = 0U; i < 20U; ++i) {
        (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_DOWN, &action);
    }
    (void)mesh_ui_nav_clamp(&nav, &store);
    if (nav.trend_scroll != 4U) {
        failure = "the scroll should be held to the readings there are";
        goto cleanup;
    }

    /* And the span picker resets it, because a different span is a different set of rows. */
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_LEFT, &action);
    if (nav.trend_scroll != 0U) {
        failure = "picking another span should put the list back at its newest reading";
        goto cleanup;
    }

    /*
     * And the bar names both, which is the other half of the rule: a keycap that does something
     * has to be in the table, and one that does not must not be. The scroll is the interesting
     * half - it is named against the readings and the body rows together, so a list that fits is
     * a gesture the bar stays quiet about.
     */
    {
        struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
        if (snapshot == NULL) {
            failure = "snapshot allocation failed";
            goto cleanup;
        }
        snapshot->nav = nav;
        snapshot->nav.trend_table = true;
        snapshot->handshake = handshake;
        snapshot->handshake_valid = true;
        snapshot->history = store.history;
        snapshot->page_rows = 4U;
        struct inkcell_action_bar bar;
        bool named_y = false;
        bool named_scroll = false;
        mesh_ui_actions_for(snapshot, &bar);
        for (size_t i = 0U; i < bar.count; ++i) {
            named_y = named_y || bar.items[i].button == INKCELL_BUTTON_Y;
            named_scroll = named_scroll || bar.items[i].button == INKCELL_BUTTON_UP_DOWN;
        }
        /* Eight readings in a body of four: more than fits, so both presses are real. */
        const bool overflowed = named_y && named_scroll;

        /* And a body with room for all of them, where the scroll is not. */
        snapshot->page_rows = 32U;
        named_scroll = false;
        mesh_ui_actions_for(snapshot, &bar);
        for (size_t i = 0U; i < bar.count; ++i) {
            named_scroll = named_scroll || bar.items[i].button == INKCELL_BUTTON_UP_DOWN;
        }
        const bool quiet = !named_scroll;
        free(snapshot);
        if (!overflowed) {
            failure = "the bar should name Y, and the scroll while the list overflows";
            goto cleanup;
        }
        if (!quiet) {
            failure = "a list that fits has nothing to scroll and must not be offered one";
            goto cleanup;
        }
    }

    /* Y again is the picture, and closing the chart leaves no scroll behind for the next one. */
    nav.trend_scroll = 3U;
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_Y, &action);
    if (nav.trend_table || nav.trend_scroll != 0U) {
        failure = "Y should turn it back, and take the position with it";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(ui_nav_node_trend_keeps_the_row_it_was_opened_from, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.node_count = 1U;
    handshake.nodes[0].node_id = 0x2000U;
    handshake.nodes[0].environment.valid = true;
    handshake.nodes[0].environment.has_temperature = true;
    handshake.nodes[0].environment.temperature = 21.0f;
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

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count =
        mesh_ui_node_detail_build(&handshake.nodes[0], false, 0U, NULL, &handshake, &store.history,
                                  false, items, MESH_UI_NODE_ITEMS_MAX);
    uint32_t row = count;
    for (uint32_t i = 0U; i < count; ++i) {
        if (items[i].trend_reading == MESH_UI_HISTORY_TEMPERATURE) {
            row = i;
            break;
        }
    }
    MESH_TEST_FAIL_IF(row == count, "the temperature row should be chartable");
    MESH_TEST_FAIL_IF(row == 0U, "the row must not be row 0, or this proves nothing");
    nav.cursor[MESH_UI_SCREEN_NODES] = row;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_TEMPERATURE, "A should open the chart");

    /* The publish that follows the press, which is where this used to be lost. */
    (void)mesh_ui_nav_clamp(&nav, &store);
    MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != row,
                      "a clamp under an open chart must not reset the detail's cursor");

    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_B, &action);
    MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != row,
                      "B out of a chart should land on the row it was opened from");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * One reading is a level, not a trend, so its row offers no chart.
 *
 * mesh_ui_history_series() answers on a drawable *segment* rather than on a count, which is
 * mesh_ui_history_has_airtime()'s rule and matters more here because this answer decides a
 * press: counted, a node heard once would offer a chart, and A would open an axis frame with its
 * ends labelled and nothing between them.
 */
MESH_TEST_CASE(ui_nav_node_trend_needs_a_line_to_draw, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.node_count = 1U;
    handshake.nodes[0].node_id = 0x2000U;
    handshake.nodes[0].environment.valid = true;
    handshake.nodes[0].environment.has_temperature = true;
    handshake.nodes[0].environment.temperature = 21.0f;
    mesh_ui_store_tick(&store, 1000U);
    mesh_ui_store_set_handshake(&store, &handshake);

    MESH_TEST_FAIL_IF(mesh_ui_node_detail_trend_row(&handshake.nodes[0], false, NULL, &handshake,
                                                    &store.history, MESH_UI_HISTORY_TEMPERATURE,
                                                    NULL),
                      "a single reading is a level and offers no chart");

    /* A second report, far enough after the first to be inside the node gap, makes a line. */
    handshake.nodes[0].environment.temperature = 23.0f;
    mesh_ui_store_tick(&store, 2000U);
    mesh_ui_store_set_handshake(&store, &handshake);
    MESH_TEST_FAIL_IF(!mesh_ui_node_detail_trend_row(&handshake.nodes[0], false, NULL, &handshake,
                                                     &store.history, MESH_UI_HISTORY_TEMPERATURE,
                                                     NULL),
                      "two readings one tick apart are a line");

    /* And two readings either side of a silence longer than the node gap are two samples the
       ring holds and no stroke at all, which is not a chart either. */
    struct mesh_ui_history gapped;
    mesh_ui_history_reset(&gapped);
    mesh_ui_history_note_environment(&gapped, 1000U, 0x2000U, true, 210, false, 0);
    mesh_ui_history_note_environment(&gapped, 1000U + MESH_UI_HISTORY_NODE_GAP_MS + 1000U, 0x2000U,
                                     true, 230, false, 0);
    MESH_TEST_FAIL_IF(mesh_ui_node_detail_trend_row(&handshake.nodes[0], false, NULL, &handshake,
                                                    &gapped, MESH_UI_HISTORY_TEMPERATURE, NULL),
                      "two readings with no line between them are not a chart");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * A chart closes when its *row* goes, not only when its readings do.
 *
 * A reading is an optional field of an optional Telemetry variant, so a node that reports a
 * temperature and then reports without one takes the row away while the history stays exactly as
 * drawable as it was. Asked of the history the chart stayed open over a row that no longer
 * existed - the renderer fell back to drawing the detail while the nav, the action bar and the
 * key handler all still believed a picture was up, so the reader got a list whose d-pad was
 * swallowed until they pressed B.
 */
MESH_TEST_CASE(ui_nav_node_trend_closes_when_its_row_goes, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.node_count = 1U;
    handshake.nodes[0].node_id = 0x2000U;
    handshake.nodes[0].environment.valid = true;
    handshake.nodes[0].environment.has_temperature = true;
    handshake.nodes[0].environment.temperature = 21.0f;
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
    nav.node_trend = MESH_UI_HISTORY_TEMPERATURE;

    (void)mesh_ui_nav_clamp(&nav, &store);
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_TEMPERATURE,
                      "a chart over a row that is still there stays open");

    /* The node keeps reporting, and stops reporting a temperature. The history it already gave
       us is untouched and still perfectly drawable. */
    handshake.nodes[0].environment.has_temperature = false;
    handshake.nodes[0].environment.has_pressure = true;
    handshake.nodes[0].environment.barometric_pressure = 1013.0f;
    mesh_ui_store_tick(&store, 3000U);
    mesh_ui_store_set_handshake(&store, &handshake);
    MESH_TEST_FAIL_IF(
        mesh_ui_history_series(&store.history, 0x2000U, MESH_UI_HISTORY_TEMPERATURE) == NULL,
        "the readings we were given are still there, which is the whole trap");

    (void)mesh_ui_nav_clamp(&nav, &store);
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_NONE,
                      "a chart whose row has gone should close rather than swallow the d-pad");
    MESH_TEST_FAIL_IF(!nav.node_detail_open, "and leave the detail it was opened over");

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
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_B, &action);
    MESH_TEST_FAIL_IF(nav.node_detail_open, "B off the detail should close it");
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_NONE,
                      "and no chart may outlive the detail it was a level of");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* The Nodes tab's pin: X from either level, and the detail's own row. The nav sends the state
   it wants rather than a bare toggle, so a press that races a NodeInfo cannot cancel itself. */
/*
 * The sheet of verbs takes the presses it owns and no others.
 *
 * It is the one screen in this tab whose handler is reached before the tab routing, so it is the
 * one that can swallow a keycap the action bar is still naming. It did: a blanket return over
 * every key left "L/R tabs" on the bar with the shoulders dead under it, which is exactly the
 * keycap-that-does-nothing that src/ui/tables/actions.c exists to prevent.
 *
 * So the four presses it does not own are pinned here beside the two it does. X and Y are in that
 * list because they reach the node by id and go on meaning pin and write from inside the sheet -
 * the bar not naming them is about the rows below being the same two verbs, not about the presses
 * being off.
 */
MESH_TEST_CASE(ui_nav_node_actions_lets_the_other_presses_through, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    const char *failure = NULL;
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);

    /* Onto a node that is not us, in to its detail, and in again to its verbs. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    for (uint32_t step = 0; step < MESH_UI_NODES_LEAD_ROWS + 1U; ++step) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.node_detail_open || !store.nav.node_actions_open) {
        failure = "two presses of A should reach the node's verbs";
        goto cleanup;
    }
    const uint32_t node_id = store.nav.node_detail_node;

    /* SELECT explains this screen, which is the topic keyed on its own route. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_SELECT, &action);
    if (!store.nav.help_open) {
        failure = "SELECT should open the sheet's help, as the bar says it does";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    if (store.nav.help_open || !store.nav.node_actions_open) {
        failure = "B should close the help and leave the sheet where it was";
        goto cleanup;
    }

    /* X and Y still reach the node, which they find by id rather than by the cursor - so the
       sheet's own cursor standing on some other verb changes nothing about them. */
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (action.type != MESH_UI_ACTION_TOGGLE_FAVORITE || action.dest != node_id) {
        failure = "X on the sheet should still pin the node the sheet is about";
        goto cleanup;
    }

    /* And the shoulders change tab, which is the press the bar names last and the one a blanket
       return took away. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    if (store.nav.screen == MESH_UI_SCREEN_NODES) {
        failure = "a shoulder should still change tab from inside the sheet";
        goto cleanup;
    }
    /* And the tab is left as it was found, so coming back lands on the verbs rather than on the
       list - the same thing an open detail or an open map does. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_L1, &action);
    if (store.nav.screen != MESH_UI_SCREEN_NODES || !store.nav.node_actions_open) {
        failure = "coming back to the tab should land on the sheet it was left on";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Left and Right walk the detail's groups, a card at a time.
 *
 * The screen this is on is the longest list in the client - a repeater reporting everything is a
 * hundred and twenty rows - and Up and Down cross it a row at a time past four dozen facts that
 * no press does anything to. Three things are worth pinning rather than looking at:
 *
 *   - the landing is never a heading, because the cursor may not stand on one and a jump that
 *     put it there would leave A promising "select" over a group title;
 *   - Left is "the top of this group, then the one before", so three presses of Left walk three
 *     cards rather than landing one row short of each;
 *   - the press is spent at either end rather than falling through to the tab switch, which is
 *     the one way this could take the reader off the node entirely.
 */
MESH_TEST_CASE(ui_nav_node_detail_walks_its_groups, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 1U;
    handshake.nodes[0].node_id = 0x2000U;
    /* Enough reported for a third and fourth group beyond the one the actions row stands in:
       the environment and the device metrics are separate Telemetry variants and separate cards.
       Two environment readings rather than one, and reported twice, so that card ends up holding
       two presses - which is what the last case here needs and what the card of verbs used to be
       the only source of. */
    handshake.nodes[0].environment.valid = true;
    handshake.nodes[0].environment.has_temperature = true;
    handshake.nodes[0].environment.temperature = 21.0f;
    handshake.nodes[0].environment.has_humidity = true;
    handshake.nodes[0].environment.relative_humidity = 54.0f;
    handshake.nodes[0].metrics.valid = true;
    handshake.nodes[0].metrics.has_battery = true;
    handshake.nodes[0].metrics.battery_level = 82U;
    mesh_ui_store_tick(&store, 1000U);
    mesh_ui_store_set_handshake(&store, &handshake);
    handshake.nodes[0].environment.temperature = 23.0f;
    handshake.nodes[0].environment.relative_humidity = 57.0f;
    mesh_ui_store_tick(&store, 2000U);
    mesh_ui_store_set_handshake(&store, &handshake);

    struct mesh_ui_nav nav;
    mesh_ui_nav_init(&nav);
    nav.screen = MESH_UI_SCREEN_NODES;
    nav.node_detail_open = true;
    nav.node_detail_node = 0x2000U;

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count =
        mesh_ui_node_detail_build(&handshake.nodes[0], false, 0U, NULL, &handshake, &store.history,
                                  false, items, MESH_UI_NODE_ITEMS_MAX);
    /* Where opening the detail leaves the cursor: the first row that is not a group title. */
    for (uint32_t i = 0U; i < count; ++i) {
        if (items[i].kind != MESH_UI_NODE_ROW_HEADING) {
            nav.cursor[MESH_UI_SCREEN_NODES] = i;
            break;
        }
    }
    /* Headings plus one, because the screen opens with a group that has none: the row that opens
       the node's verbs stands above the first heading, and a run of rows before any heading is a
       group exactly as a run after one is. Counted rather than assumed so that a heading arriving
       over that row - or the row going away - is a failure here rather than an off-by-one in the
       walk below. */
    uint32_t groups = items[0].kind == MESH_UI_NODE_ROW_HEADING ? 0U : 1U;
    for (uint32_t i = 0U; i < count; ++i) {
        if (items[i].kind == MESH_UI_NODE_ROW_HEADING) {
            groups += 1U;
        }
    }
    MESH_TEST_FAIL_IF(groups < 3U, "this node should report enough for three groups");

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);

    /* Right walks forward, one group per press, and never lands on a title. */
    uint32_t seen[MESH_UI_NODE_ITEMS_MAX];
    uint32_t visited = 0U;
    seen[visited++] = nav.cursor[MESH_UI_SCREEN_NODES];
    for (uint32_t i = 1U; i < groups; ++i) {
        const uint32_t before = nav.cursor[MESH_UI_SCREEN_NODES];
        (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_RIGHT, &action);
        const uint32_t at = nav.cursor[MESH_UI_SCREEN_NODES];
        MESH_TEST_FAIL_IF(at <= before, "Right should move forward to the next group");
        MESH_TEST_FAIL_IF(items[at].kind == MESH_UI_NODE_ROW_HEADING,
                          "the cursor may not land on a group title");
        MESH_TEST_FAIL_IF(at == 0U || items[at - 1U].kind != MESH_UI_NODE_ROW_HEADING,
                          "the landing should be the first row under a heading");
        MESH_TEST_FAIL_IF(items[at].kind == MESH_UI_NODE_ROW_ACTION,
                          "and no group but the first holds a verb any more");
        MESH_TEST_FAIL_IF(nav.screen != MESH_UI_SCREEN_NODES, "the press should not change tab");
        seen[visited++] = at;
    }

    /* The last group: Right has nowhere to go and spends the press rather than changing tab. */
    const uint32_t last = nav.cursor[MESH_UI_SCREEN_NODES];
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_RIGHT, &action);
    MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != last,
                      "Right past the last group should stay put");
    MESH_TEST_FAIL_IF(nav.screen != MESH_UI_SCREEN_NODES,
                      "and must not fall through to the tab switch");

    /* Left off the top of a group goes to the group before, so the walk comes back the way it
       went. */
    for (uint32_t i = visited; i-- > 1U;) {
        (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_LEFT, &action);
        MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != seen[i - 1U],
                          "Left should retrace the groups Right walked");
    }
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_LEFT, &action);
    MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != seen[0],
                      "Left at the first group should stay put");
    MESH_TEST_FAIL_IF(nav.screen != MESH_UI_SCREEN_NODES,
                      "and must not fall through to the tab switch");

    /* And Left from inside a group is "the top of this one" before it is "the one before".
     *
     * Asked of the environment card, which is the group with two stops in it now that the verbs
     * are a screen of their own: its temperature and its humidity have both been watched, so each
     * is a press and Down moves between them without leaving the card. */
    while (mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_RIGHT, &action)) {
    }
    const uint32_t top = nav.cursor[MESH_UI_SCREEN_NODES];
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_DOWN, &action);
    MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] == top, "Down should move within a group");
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_LEFT, &action);
    MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != top,
                      "Left from inside a group should go to the top of it");

    /* The shoulders are deliberately not taken, which is what pays for the d-pad here. */
    (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_R1, &action);
    MESH_TEST_FAIL_IF(nav.screen == MESH_UI_SCREEN_NODES,
                      "the shoulders should still change tab from inside a node");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * Up and Down walk stops rather than rows.
 *
 * Every row A acts on is one, and a card of facts is one - or one per page, when it is taller than
 * the window the backend last drew it in. What is worth pinning rather than looking at is the
 * property the stops exist to keep: every row of the detail is inside the span the window keeps in
 * view for *some* stop, so no fact is left somewhere no press can scroll to. And Up retraces Down,
 * and both ends spend the press.
 */
MESH_TEST_CASE(ui_nav_node_detail_walks_its_stops, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 1U;
    struct mesh_ui_node_summary *node = &handshake.nodes[0];
    node->node_id = 0x2000U;
    /* An identity of eleven rows: two pages in a small window, one in a large one. */
    snprintf(node->long_name, sizeof node->long_name, "Echo Repeater");
    snprintf(node->short_name, sizeof node->short_name, "ECHO");
    snprintf(node->user_id, sizeof node->user_id, "!00002000");
    node->role = 2U;
    node->hw_model = 43U;
    node->public_key_len = 32U;
    node->is_licensed = true;
    /* A reading watched twice, so its card holds a press among its facts. */
    node->environment.valid = true;
    node->environment.has_temperature = true;
    node->environment.temperature = 21.0f;
    node->environment.has_pressure = true;
    node->environment.barometric_pressure = 1013.0f;
    /* And enough facts after it that the card outgrows a small window, where the facts under
       the reading need stops of their own. */
    node->environment.has_iaq = true;
    node->environment.iaq = 42U;
    node->environment.has_lux = true;
    node->environment.lux = 300.0f;
    node->environment.has_voltage = true;
    node->environment.voltage = 5.0f;
    node->environment.has_current = true;
    node->environment.current = 120.0f;
    node->metrics.valid = true;
    node->metrics.has_voltage = true;
    node->metrics.voltage = 4.1f;
    mesh_ui_store_tick(&store, 1000U);
    mesh_ui_store_set_handshake(&store, &handshake);
    node->environment.temperature = 22.0f;
    mesh_ui_store_tick(&store, 2000U);
    mesh_ui_store_set_handshake(&store, &handshake);

    const struct mesh_ui_node_summary *held = mesh_ui_node_detail_find(&store.handshake, 0x2000U);
    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(
        held, false, 0U, mesh_ui_store_traceroute_view(&store, held->node_id), &store.handshake,
        &store.history, false, items, MESH_UI_NODE_ITEMS_MAX);
    MESH_TEST_FAIL_IF(count == 0U, "the node should produce rows");
    uint8_t heights[MESH_UI_NODE_ITEMS_MAX];
    for (uint32_t i = 0U; i < count; ++i) {
        heights[i] = mesh_ui_node_item_steps(&items[i]);
    }

    /* A window the identity does not fit - scale 6 on the Brick is eight rows - and one every
       card fits, where a page would be a press that changes nothing on the panel. */
    static const uint32_t windows[] = {8U, 40U};
    for (size_t w = 0U; w < sizeof windows / sizeof windows[0]; ++w) {
        const uint32_t rows = windows[w];
        store.page_rows = rows;
        struct mesh_ui_nav nav;
        mesh_ui_nav_init(&nav);
        nav.screen = MESH_UI_SCREEN_NODES;
        nav.node_detail_open = true;
        nav.node_detail_node = 0x2000U;
        /* Where opening the detail leaves the cursor: the first row that is not a group title. */
        for (uint32_t i = 0U; i < count; ++i) {
            if (items[i].kind != MESH_UI_NODE_ROW_HEADING) {
                nav.cursor[MESH_UI_SCREEN_NODES] = i;
                break;
            }
        }

        struct mesh_ui_action action;
        memset(&action, 0, sizeof action);
        bool covered[MESH_UI_NODE_ITEMS_MAX] = {false};
        bool pressed[MESH_UI_NODE_ITEMS_MAX] = {false};
        uint32_t stops[MESH_UI_NODE_ITEMS_MAX];
        uint32_t visited = 0U;
        uint32_t card_stops = 0U;
        bool paged = false;
        bool mixed = false;
        for (;;) {
            const uint32_t at = nav.cursor[MESH_UI_SCREEN_NODES];
            MESH_TEST_FAIL_IF(items[at].kind == MESH_UI_NODE_ROW_HEADING,
                              "the cursor may not land on a group title");
            stops[visited++] = at;
            const enum mesh_ui_node_press press = mesh_ui_node_detail_press_at(
                held, false, mesh_ui_store_traceroute_view(&store, held->node_id), &store.handshake,
                &store.history, at);
            struct mesh_ui_node_span span;
            MESH_TEST_FAIL_IF(!mesh_ui_node_detail_span(items, count, rows, at, &span),
                              "no span for a stop");
            MESH_TEST_FAIL_IF(span.first > at || span.last < at, "a stop's span should hold it");
            MESH_TEST_FAIL_IF(span.card != (press == MESH_UI_NODE_PRESS_NONE),
                              "a press is a row and a fact stop is its card");
            if (press != MESH_UI_NODE_PRESS_NONE) {
                pressed[at] = true;
                for (uint32_t r = span.first; r <= span.last; ++r) {
                    mixed = mixed ||
                            (items[r].kind != MESH_UI_NODE_ROW_HEADING &&
                             mesh_ui_node_detail_press_at(
                                 held, false, mesh_ui_store_traceroute_view(&store, held->node_id),
                                 &store.handshake, &store.history, r) == MESH_UI_NODE_PRESS_NONE);
                }
            } else {
                card_stops += 1U;
                uint32_t steps = 0U;
                for (uint32_t r = span.first; r <= span.last; ++r) {
                    steps += mesh_ui_node_item_steps(&items[r]);
                }
                MESH_TEST_FAIL_IF(steps > rows, "a page should fit the window, heading and all");
                paged = paged || items[span.first].kind != MESH_UI_NODE_ROW_HEADING;
            }
            /* What the window really shows from this stop, measured the way the renderer opens
               it - a span taller than the window is not all on screen. */
            const struct inkcell_list window =
                inkcell_list_begin_span(count, at, span.first, span.last, rows, heights);
            for (uint32_t r = window.first; r < window.first + window.visible; ++r) {
                covered[r] = true;
            }
            if (!mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_DOWN, &action)) {
                break;
            }
            MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] <= at, "Down should move forward");
        }
        MESH_TEST_FAIL_IF(visited >= count, "Down should skip the facts, not walk every row");
        MESH_TEST_FAIL_IF(card_stops == 0U, "a card of facts should be a stop");
        MESH_TEST_FAIL_IF(paged != (rows < 12U),
                          "the identity should take pages only when the window cannot hold it");
        MESH_TEST_FAIL_IF(!mixed, "the environment card should hold a press among its facts");
        for (uint32_t r = 0U; r < count; ++r) {
            MESH_TEST_FAIL_IF(!covered[r], "every row should be on screen from some stop");
            MESH_TEST_FAIL_IF(mesh_ui_node_detail_press_at(
                                  held, false, mesh_ui_store_traceroute_view(&store, held->node_id),
                                  &store.handshake, &store.history, r) != MESH_UI_NODE_PRESS_NONE &&
                                  !pressed[r],
                              "every row A acts on should be a stop");
        }
        MESH_TEST_FAIL_IF(nav.screen != MESH_UI_SCREEN_NODES,
                          "Down at the end should not change tab");

        /* Up retraces the walk, and spends the press at the top. */
        for (uint32_t i = visited; i-- > 1U;) {
            (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_UP, &action);
            MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != stops[i - 1U],
                              "Up should retrace the stops Down walked");
        }
        (void)mesh_ui_nav_handle_key(&nav, &store, INKCELL_KEY_UP, &action);
        MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != stops[0],
                          "Up at the top should stay");
    }

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * What A does on a row, asked once.
 *
 * The nav returns false on every row that is not an action or a charted reading, and the action
 * bar used to name "A select" over all of them - two rows in three of this screen. One answer,
 * so the bar and the press cannot disagree again.
 */
MESH_TEST_CASE(ui_node_detail_press_matches_the_row, unit) {
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
    mesh_ui_store_tick(&store, 1000U);
    mesh_ui_store_set_handshake(&store, &handshake);
    handshake.nodes[0].environment.temperature = 23.0f;
    mesh_ui_store_tick(&store, 2000U);
    mesh_ui_store_set_handshake(&store, &handshake);

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count =
        mesh_ui_node_detail_build(&handshake.nodes[0], false, 0U, NULL, &handshake, &store.history,
                                  false, items, MESH_UI_NODE_ITEMS_MAX);
    MESH_TEST_FAIL_IF(count == 0U, "the node should produce rows");

    uint32_t facts = 0U;
    uint32_t selects = 0U;
    uint32_t trends = 0U;
    for (uint32_t row = 0U; row < count; ++row) {
        const enum mesh_ui_node_press press = mesh_ui_node_detail_press_at(
            &handshake.nodes[0], false, NULL, &handshake, &store.history, row);
        switch (items[row].kind) {
        case MESH_UI_NODE_ROW_ACTION:
            MESH_TEST_FAIL_IF(press != MESH_UI_NODE_PRESS_SELECT, "an action row runs its verb");
            selects += 1U;
            break;
        case MESH_UI_NODE_ROW_METER:
            if (items[row].trend_reading != MESH_UI_HISTORY_NONE) {
                MESH_TEST_FAIL_IF(press != MESH_UI_NODE_PRESS_TREND,
                                  "a watched reading opens its chart");
                trends += 1U;
                break;
            }
            /* fall through: a bar with nothing watched behind it is a fact like any other */
            MESH_TEST_FAIL_IF(press != MESH_UI_NODE_PRESS_NONE, "an unwatched bar is a fact");
            facts += 1U;
            break;
        default:
            MESH_TEST_FAIL_IF(press != MESH_UI_NODE_PRESS_NONE,
                              "a fact and a heading are not presses");
            facts += 1U;
            break;
        }
    }
    MESH_TEST_FAIL_IF(selects == 0U || trends == 0U || facts == 0U,
                      "this node should offer all three kinds of row");
    /* Past the end answers NONE rather than reading off the end of the build. */
    MESH_TEST_FAIL_IF(mesh_ui_node_detail_press_at(&handshake.nodes[0], false, NULL, &handshake,
                                                   &store.history,
                                                   count + 5U) != MESH_UI_NODE_PRESS_NONE,
                      "a row past the end is not a press");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* Whether this node's detail draws a measured route, which is the "Route out" group and the
   stops under it. */
static bool detail_draws_a_route(const struct mesh_ui_store *store,
                                 const struct mesh_ui_node_summary *node) {
    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(
        node, false, 0U, mesh_ui_store_traceroute_view(store, node->node_id), &store->handshake,
        NULL, false, items, MESH_UI_NODE_ITEMS_MAX);
    for (uint32_t i = 0U; i < count; ++i) {
        if (items[i].kind == MESH_UI_NODE_ROW_HEADING &&
            strcmp(items[i].label, inkcell_str(MESH_STR_NODE_HEAD_ROUTE_OUT)) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * Tracing one node does not erase another node's route.
 *
 * The store held one traceroute until the log was added, so the second trace of a session took
 * the first one's path off the screen - a measurement the user had waited up to a minute for,
 * gone because they looked at a different node. Each node's detail now answers from its own
 * record, and a node nothing was traced to still has nothing to draw.
 */
MESH_TEST_CASE(ui_nav_node_detail_keeps_each_nodes_route, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    const char *failure = NULL;

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.roster_owner = 0x1000U;
    handshake.node_count = 3U;
    handshake.nodes[0].node_id = 0x2000U;
    handshake.nodes[1].node_id = 0x3000U;
    handshake.nodes[2].node_id = 0x4000U;
    mesh_ui_store_set_handshake(&store, &handshake);

    for (uint32_t i = 0U; i < 2U; ++i) {
        struct mesh_ui_traceroute trace;
        memset(&trace, 0, sizeof trace);
        trace.state = MESH_TRACEROUTE_DONE;
        trace.target = 0x2000U + i * 0x1000U;
        trace.completed = 1750000000U + i;
        trace.forward_count = 2U;
        trace.forward[0].node_id = 0x1000U;
        snprintf(trace.forward[0].name, sizeof trace.forward[0].name, "US");
        trace.forward[1].node_id = trace.target;
        trace.forward[1].has_snr = true;
        trace.forward[1].snr_quarter_db = 18;
        snprintf(trace.forward[1].name, sizeof trace.forward[1].name, "N%u", (unsigned)i);
        mesh_ui_store_set_traceroute(&store, &trace);
    }

    if (!detail_draws_a_route(&store, &store.handshake.nodes[1])) {
        failure = "the node traced last should draw its route";
        goto cleanup;
    }
    if (!detail_draws_a_route(&store, &store.handshake.nodes[0])) {
        failure = "the node traced before it should still draw its own";
        goto cleanup;
    }
    if (detail_draws_a_route(&store, &store.handshake.nodes[2])) {
        failure = "a node nothing was traced to has no route to draw";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

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

    /* The list opens on its filter row, and the map row is under it - two rows that are about
       no node at all, so X on either asks for nothing and it takes two steps down to reach the
       first node. Counted from MESH_UI_NODES_LEAD_ROWS rather than written out, so a third lead
       row arrives here as a compile-time fact rather than as a mystery failure. */
    for (uint32_t lead = 0; lead < MESH_UI_NODES_LEAD_ROWS; ++lead) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
        if (action.type != MESH_UI_ACTION_NONE) {
            mesh_ui_store_shutdown(&store);
            record_failure(test_name, "X on a lead row should do nothing");
            return;
        }
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }

    /* Our own node cannot be pinned: it already outranks everything. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (action.type != MESH_UI_ACTION_NONE) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "X on our own node should do nothing");
        return;
    }

    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (action.type != MESH_UI_ACTION_TOGGLE_FAVORITE || action.number != 0U) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "X on a pinned node should ask for an unpin");
        return;
    }

    /* And the sheet's own row does the same thing, wherever it happens to sit. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* open the detail */
    if (!store.nav.node_detail_open) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "A should open the detail");
        return;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action); /* and its "Actions" row */
    if (!store.nav.node_actions_open) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the detail's first row should open the node's verbs");
        return;
    }
    struct mesh_ui_node_item items[MESH_UI_NODE_ACTIONS_MAX];
    const uint32_t count = mesh_ui_node_actions_build(&store.handshake.nodes[1], false, NULL, false,
                                                      items, MESH_UI_NODE_ACTIONS_MAX);
    uint32_t favorite_row = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].action == MESH_UI_NODE_ACTION_FAVORITE) {
            favorite_row = i;
        }
    }
    if (favorite_row >= count || strcmp(items[favorite_row].value, "yes") != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the sheet should carry a pin row showing the current state");
        return;
    }
    /* Walk to it rather than counting presses from 0, which is the habit rather than a need
       here: every row of this screen is a stop, so the two happen to agree. */
    while (store.nav.node_actions_cursor < favorite_row &&
           mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action)) {
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
    /* Past the filter and map rows, then past our own node, onto the second node in the list. */
    for (uint32_t step = 0; step < MESH_UI_NODES_LEAD_ROWS + 1U; ++step) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_X, &action);
    if (action.type != MESH_UI_ACTION_DISCONNECT ||
        strcmp(action.identifier, devices[0].identifier) != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "X should drop the connected link");
        return;
    }

    /* One press of Y only arms it: a bond dropped by accident costs a re-pair. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    if (action.type != MESH_UI_ACTION_NONE || !store.nav.devices_forget_armed) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the first Y should only arm the forget");
        return;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    if (action.type != MESH_UI_ACTION_FORGET ||
        strcmp(action.identifier, devices[0].identifier) != 0 || store.nav.devices_forget_armed) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the second Y should forget the node");
        return;
    }

    /* Anything else stands it down, and a USB port has no bond to forget at all. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    if (store.nav.devices_forget_armed) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "moving the cursor should stand the forget down");
        return;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
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
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

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
        store.nav.kb.row = 0U;
        store.nav.kb.col = col;
        mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    }
    if (strcmp(store.nav.draft, pin) != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the digits should land in the draft");
        return;
    }

    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
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
        store.nav.kb.row = 0U;
        store.nav.kb.col = 0U; /* "1" */
        mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    }
    if (strcmp(store.nav.draft, "111111") != 0) {
        mesh_ui_store_shutdown(&store);
        record_failure(test_name, "the prompt should stop at six digits");
        return;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
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
    mesh_ui_store_handle_key(&store, INKCELL_KEY_START, &action);
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
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action); /* Nodes */
    /* Past the lead rows, then past our own node, onto one that is not us. */
    for (uint32_t step = 0; step < MESH_UI_NODES_LEAD_ROWS + 1U; ++step) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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

    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.node_actions_open) {
        failure = "the detail's first row should open the node's verbs";
        goto cleanup;
    }

    struct mesh_ui_node_item items[MESH_UI_NODE_ACTIONS_MAX];
    uint32_t count =
        mesh_ui_node_actions_build(node, false, NULL, false, items, MESH_UI_NODE_ACTIONS_MAX);
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
        failure = "the sheet should carry mute and then remove";
        goto cleanup;
    }
    /* The armed spelling is the only thing the flag changes. */
    (void)mesh_ui_node_actions_build(node, false, NULL, true, items, MESH_UI_NODE_ACTIONS_MAX);
    if (strcmp(items[remove_row].value, "A again to remove") != 0) {
        failure = "arming should change what the remove row says";
        goto cleanup;
    }

    while (store.nav.node_actions_cursor < mute_row &&
           mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action)) {
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (action.type != MESH_UI_ACTION_TOGGLE_MUTE || action.dest != node->node_id) {
        failure = "A on the mute row should emit a toggle for that node";
        goto cleanup;
    }

    /* Remove: the first press only arms, and moving off the row stands it down again. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (!store.nav.node_remove_armed || action.type != MESH_UI_ACTION_NONE) {
        failure = "the first press on remove should only arm it";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
    if (store.nav.node_remove_armed) {
        failure = "moving off the remove row should stand it down";
        goto cleanup;
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    if (store.nav.node_remove_armed || action.type != MESH_UI_ACTION_REMOVE_NODE ||
        action.dest != node->node_id) {
        failure = "the second press should emit the removal";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The node a filtered row is about, through the one mapping the screen and the nav both use.
 *
 * The default sort, because what these cases are about is the filter: mesh_ui_node_view_build()
 * leaves the published order exactly as it found it under that member, so this is the walk
 * mesh_ui_node_filter_at() used to be - against the pair that replaced it.
 */
static const struct mesh_ui_node_summary *
nodes_filter_at(const struct mesh_ui_handshake_state *handshake, enum mesh_ui_node_filter filter,
                uint32_t index) {
    struct mesh_ui_node_view view;
    mesh_ui_node_view_build(handshake, filter, MESH_UI_NODE_SORT_DEFAULT, &view);
    return mesh_ui_node_view_at(handshake, &view, index);
}

/*
 * ---- the filter row -------------------------------------------------------------------------
 *
 * The list's first row is a control and A steps it, which is three separate claims: the
 * press changes the filter, the filter changes how many rows the list has, and - the one worth
 * the test rather than a screenshot - the *row-to-node mapping* moves with it. That last is why
 * the filter goes through mesh_ui_node_view_at() rather than through an offset: a wrong
 * answer there is a plausible node rather than an obviously shifted one, so X pins somebody
 * else's radio and nothing on the frame looks wrong.
 */
MESH_TEST_CASE(ui_nav_nodes_filter_steps_and_renumbers_the_rows, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 3U;
    handshake.nodes[0].node_id = 0x1000U; /* us: neither pinned nor heard */
    handshake.nodes[1].node_id = 0x2000U; /* heard directly, not pinned */
    handshake.nodes[1].has_hops_away = true;
    handshake.nodes[1].hops_away = 0U;
    handshake.nodes[1].snr = 6.5f;
    handshake.nodes[2].node_id = 0x3000U; /* pinned, and three hops out */
    handshake.nodes[2].is_favorite = true;
    handshake.nodes[2].has_hops_away = true;
    handshake.nodes[2].hops_away = 3U;
    handshake.nodes[2].snr = 2.0f;
    for (uint32_t i = 0; i < handshake.node_count; ++i) {
        handshake.nodes[i].in_nodedb = true;
    }
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_ui_store_consume_updates(&store, NULL);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    store.nav.screen = MESH_UI_SCREEN_NODES;

    MESH_TEST_FAIL_IF_CLEANUP(store.nav.cursor[MESH_UI_SCREEN_NODES] != MESH_UI_NODES_FILTER_ROW,
                              mesh_ui_store_shutdown(&store),
                              "the list opens on the row that filters it");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_filter != MESH_UI_NODE_FILTER_ALL,
                              mesh_ui_store_shutdown(&store), "and it opens on All");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_NODES) !=
                                  MESH_UI_NODES_LEAD_ROWS + 3U,
                              mesh_ui_store_shutdown(&store),
                              "All shows the whole roster under the two lead rows");

    /* A steps the filter and leaves the cursor where it is - this row is the one row the press
       cannot re-number. Left and Right do the same thing and are what the row and the bar name;
       ui_nav_nodes_controls_take_the_d_pad holds that half. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_filter != MESH_UI_NODE_FILTER_DIRECT,
                              mesh_ui_store_shutdown(&store), "A steps the filter on");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.cursor[MESH_UI_SCREEN_NODES] != MESH_UI_NODES_FILTER_ROW,
                              mesh_ui_store_shutdown(&store), "and stays on the row it pressed");
    MESH_TEST_FAIL_IF_CLEANUP(action.type != MESH_UI_ACTION_NONE, mesh_ui_store_shutdown(&store),
                              "a filter asks the radio for nothing");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_NODES) !=
                                  MESH_UI_NODES_LEAD_ROWS + 1U,
                              mesh_ui_store_shutdown(&store),
                              "Direct keeps the one node heard with no relay in the way");

    /*
     * And the row under the lead rows is that node, not the roster's first.
     *
     * Walked with the key and read through the press rather than through a helper of the test's
     * own: what this is checking is that A on row LEAD opens the node the row drew, which is the
     * only question the mapping exists to answer.
     */
    for (uint32_t lead = 0; lead < MESH_UI_NODES_LEAD_ROWS; ++lead) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.node_detail_open || store.nav.node_detail_node != 0x2000U,
                              mesh_ui_store_shutdown(&store),
                              "the first row under a Direct filter is the node that was heard");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);

    /* On to Pinned, which keeps the other one - so the same row is now a different node. */
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_FILTER_ROW;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_filter != MESH_UI_NODE_FILTER_PINNED,
                              mesh_ui_store_shutdown(&store), "and on to Pinned");
    for (uint32_t lead = 0; lead < MESH_UI_NODES_LEAD_ROWS; ++lead) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.node_detail_open || store.nav.node_detail_node != 0x3000U,
                              mesh_ui_store_shutdown(&store),
                              "the same row under a Pinned filter is the pinned node");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);

    /* Three filters, so a third press is back where it started. */
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_FILTER_ROW;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_filter != MESH_UI_NODE_FILTER_ALL,
                              mesh_ui_store_shutdown(&store), "and wraps back to All");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * ---- the d-pad on the two control rows -------------------------------------------------------
 *
 * The filter and the sort are edited with Left and Right, which is the press the row's pencil and
 * the action bar both name, and the reason this list's top no longer reads as a caption over it.
 *
 * Four claims, and the last two are the ones that pay for the first two. The axis steps the
 * control *both ways*, which A alone could not do - five sorts wrapped in one direction meant
 * four presses to undo one. It leaves the cursor where it is, which is what makes it safe: these
 * are the only two rows of this list that what they change cannot re-number. On any row under
 * them Left and Right are the tab switch again, unchanged. And the shoulders are the tab switch
 * on *every* row including these two, which is the whole reason the d-pad's axis could be spent
 * here at all - the same split the map and the trend chart already make.
 */
MESH_TEST_CASE(ui_nav_nodes_controls_take_the_d_pad, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    store.nav.screen = MESH_UI_SCREEN_NODES;
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_FILTER_ROW;

    /* Forward, and the cursor has not moved off the row that did it. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_filter != MESH_UI_NODE_FILTER_DIRECT,
                              mesh_ui_store_shutdown(&store), "Right steps the filter on");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.screen != MESH_UI_SCREEN_NODES,
                              mesh_ui_store_shutdown(&store),
                              "and does not fall through to the tab switch");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.cursor[MESH_UI_SCREEN_NODES] != MESH_UI_NODES_FILTER_ROW,
                              mesh_ui_store_shutdown(&store), "and stays on the row it pressed");

    /* And back the way it came, which is the half A never had. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_filter != MESH_UI_NODE_FILTER_ALL,
                              mesh_ui_store_shutdown(&store), "Left steps the filter back");

    /* Left off the first of the set wraps rather than leaving the tab, for the same reason
       Right off the last one does: the row is a ring and the shoulders are the way out. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_filter != MESH_UI_NODE_FILTER_PINNED,
                              mesh_ui_store_shutdown(&store), "and wraps rather than escaping");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.screen != MESH_UI_SCREEN_NODES,
                              mesh_ui_store_shutdown(&store), "still on the Nodes tab");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action); /* back to All */

    /* The sort row, the same axis over a set of five. */
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_SORT_ROW;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_sort != MESH_UI_NODE_SORT_HEARD,
                              mesh_ui_store_shutdown(&store), "Right steps the sort on");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_sort != MESH_UI_NODE_SORT_DEFAULT,
                              mesh_ui_store_shutdown(&store), "Left steps the sort back");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.cursor[MESH_UI_SCREEN_NODES] != MESH_UI_NODES_SORT_ROW,
                              mesh_ui_store_shutdown(&store), "and stays on the sort row");

    /*
     * The shoulders still walk the tabs from a control row, which is what pays for all of the
     * above: a reader who has landed on the filter is never stuck on this tab.
     */
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_FILTER_ROW;
    const uint8_t filter_before = store.nav.node_filter;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.screen == MESH_UI_SCREEN_NODES,
                              mesh_ui_store_shutdown(&store),
                              "the shoulder leaves the tab from a control row");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_filter != filter_before,
                              mesh_ui_store_shutdown(&store),
                              "and does not also step the control it was standing on");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_L1, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.screen != MESH_UI_SCREEN_NODES,
                              mesh_ui_store_shutdown(&store), "and back again");

    /*
     * And on a node row the d-pad is the tab switch, unchanged. This is the claim the whole
     * arrangement rests on: the axis is spent on two rows, not on the screen.
     */
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_LEAD_ROWS;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.screen == MESH_UI_SCREEN_NODES,
                              mesh_ui_store_shutdown(&store),
                              "Right on a node row is still the tab switch");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * A roster with nothing in it has no control rows, so the d-pad is the tab switch there.
 *
 * The one state where row 0 is not the filter row: mesh_ui_nav_row_count() answers 0 for an
 * empty roster and the screen draws the empty state rather than a list, so there is no filter
 * on the panel to step. Taking Left and Right for it anyway would be the d-pad going dead on
 * the first screen a client with no radio attached shows - a reader trying to leave the tab,
 * silently changing a control they cannot see.
 *
 * mesh_ui_nav_confirm() has always made this check one line in (`cursor >= rows`), so A was
 * never wrong here; it is the d-pad arm that had to be told.
 */
MESH_TEST_CASE(ui_nav_nodes_an_empty_roster_keeps_the_tab_switch, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    /* No handshake and no roster, which is what the client looks like before a radio answers. */
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    store.nav.screen = MESH_UI_SCREEN_NODES;
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_FILTER_ROW;

    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_NODES) != 0U,
                              mesh_ui_store_shutdown(&store),
                              "an empty roster is a list with no rows at all");

    const uint8_t filter_before = store.nav.node_filter;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_filter != filter_before,
                              mesh_ui_store_shutdown(&store),
                              "Right must not step a filter that is not on the panel");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.screen == MESH_UI_SCREEN_NODES,
                              mesh_ui_store_shutdown(&store),
                              "Right on an empty Nodes list is still the tab switch");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * A filter that keeps nothing keeps its own two rows, and the map row still opens the map.
 *
 * The failure this is against is a list that empties itself: the chip that emptied it is on the
 * first row, so a screen that answered zero rows would have taken away the control that puts it
 * back - and the clamp would have parked the cursor at 0 on a list with no rows at all. It is
 * the Waypoints tab's rule ("the row that makes a place is the last one, and it says why it
 * cannot be pressed rather than disappearing") applied to the row that filters.
 */
MESH_TEST_CASE(ui_nav_nodes_filter_that_keeps_nothing_keeps_its_own_rows, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 2U;
    handshake.nodes[0].node_id = 0x1000U;
    handshake.nodes[1].node_id = 0x2000U; /* nothing pinned, nothing heard directly */
    for (uint32_t i = 0; i < handshake.node_count; ++i) {
        handshake.nodes[i].in_nodedb = true;
    }
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_ui_store_consume_updates(&store, NULL);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    store.nav.screen = MESH_UI_SCREEN_NODES;
    store.nav.node_filter = (uint8_t)MESH_UI_NODE_FILTER_PINNED;
    mesh_ui_store_consume_updates(&store, NULL);

    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_node_filter_count(&store.handshake, MESH_UI_NODE_FILTER_PINNED) != 0U,
        mesh_ui_store_shutdown(&store), "nothing is pinned in this roster");
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_NODES) != MESH_UI_NODES_LEAD_ROWS,
        mesh_ui_store_shutdown(&store), "an empty filter still leaves the filter and map rows");

    /* And both of them still work: the map row opens, and A on the filter row puts the roster
       back. Walked in that order because the map is the row a stranded reader reaches first. */
    for (uint32_t lead = 0; lead < MESH_UI_NODES_MAP_ROW; ++lead) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.cursor[MESH_UI_SCREEN_NODES] != MESH_UI_NODES_MAP_ROW,
                              mesh_ui_store_shutdown(&store),
                              "the cursor can still reach the map row");

    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_FILTER_ROW;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_filter != MESH_UI_NODE_FILTER_ALL,
                              mesh_ui_store_shutdown(&store),
                              "and A wraps Pinned back round to All");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_NODES) !=
                                  MESH_UI_NODES_LEAD_ROWS + 2U,
                              mesh_ui_store_shutdown(&store), "which brings the nodes back");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * "Direct" is mesh_ui_node_signal_heard()'s question and not a looser one.
 *
 * The three nodes here are the three ways a looser test gets it wrong, and each of them is a
 * node the list already refuses to draw a staircase on: an unset `hops_away` is the firmware
 * declining to say rather than a zero, an SNR of exactly 0.0 is the session layer's own "no
 * reading", and a node arriving over MQTT was not heard on the air at all. A filter with its
 * own opinion would put all three in Direct and then draw them with no signal beside them,
 * which is the column and the chip disagreeing about the same fact.
 */
MESH_TEST_CASE(ui_nav_nodes_filter_direct_asks_the_lists_own_question, unit) {
    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.node_count = 5U;
    for (uint32_t i = 0; i < handshake.node_count; ++i) {
        handshake.nodes[i].in_nodedb = true;
    }
    /* Heard: the firmware said zero hops and there is a reading behind it. */
    handshake.nodes[0].node_id = 0x2000U;
    handshake.nodes[0].has_hops_away = true;
    handshake.nodes[0].snr = 4.2f;
    /* The firmware never said how far away it is. */
    handshake.nodes[1].node_id = 0x2001U;
    handshake.nodes[1].snr = 4.2f;
    /* It said zero hops and there is no reading. */
    handshake.nodes[2].node_id = 0x2002U;
    handshake.nodes[2].has_hops_away = true;
    handshake.nodes[2].snr = 0.0f;
    /* Zero hops with a reading, and it came in over MQTT. */
    handshake.nodes[3].node_id = 0x2003U;
    handshake.nodes[3].has_hops_away = true;
    handshake.nodes[3].snr = 4.2f;
    handshake.nodes[3].via_mqtt = true;
    /*
     * And the one mesh_ui_node_signal_heard() cannot see, which is the renderer's *first*
     * branch rather than one of its conditions: a node heard perfectly well and since dropped
     * from the radio's NodeDB. mesh_session_resolve_nodedb_membership() clears that flag from
     * the sync epoch alone and leaves `snr` and `hops_away` exactly as they were, so the
     * reading is still good and the list still draws "off radio" where the staircase would go.
     */
    handshake.nodes[4].node_id = 0x2004U;
    handshake.nodes[4].has_hops_away = true;
    handshake.nodes[4].snr = 4.2f;
    handshake.nodes[4].in_nodedb = false;

    MESH_TEST_FAIL_IF(mesh_ui_node_filter_count(&handshake, MESH_UI_NODE_FILTER_DIRECT) != 1U,
                      "only the node the list would draw a staircase on is Direct");
    const struct mesh_ui_node_summary *first =
        nodes_filter_at(&handshake, MESH_UI_NODE_FILTER_DIRECT, 0U);
    MESH_TEST_FAIL_IF(first == NULL || first->node_id != 0x2000U, "and it is that node");
    MESH_TEST_FAIL_IF(
        mesh_ui_node_filter_matches(&handshake, &handshake.nodes[4], MESH_UI_NODE_FILTER_DIRECT),
        "a node the radio has forgotten is off radio on the row, so not Direct");
    MESH_TEST_FAIL_IF(nodes_filter_at(&handshake, MESH_UI_NODE_FILTER_DIRECT, 1U) != NULL,
                      "past the end of a filter is NULL, not the next node along");

    /* Every filter agrees with the predicate it is built from, walked rather than asserted per
       node: a count and a walk that disagree is the bug the row mapping would show as a node
       opening somebody else's detail. */
    for (int f = 0; f < (int)MESH_UI_NODE_FILTER_COUNT; ++f) {
        const enum mesh_ui_node_filter filter = (enum mesh_ui_node_filter)f;
        uint32_t walked = 0U;
        while (nodes_filter_at(&handshake, filter, walked) != NULL) {
            ++walked;
        }
        MESH_TEST_FAIL_IF(walked != mesh_ui_node_filter_count(&handshake, filter),
                          "a filter's count and its walk must be the same list");
    }

    /*
     * And a value the enum has never held reads as All rather than as an empty screen.
     *
     * nav.node_filter is a uint8_t and the nav is memcpy'd around, so this is reachable without
     * anyone writing a bug: what a reader can act on is a filter row that has come back on All,
     * and what they cannot act on is a node list that is empty for no stated reason.
     */
    const enum mesh_ui_node_filter bogus = (enum mesh_ui_node_filter)200;
    MESH_TEST_FAIL_IF(mesh_ui_node_filter_count(&handshake, bogus) != handshake.node_count,
                      "an out-of-range filter keeps everything");
    MESH_TEST_FAIL_IF(
        !mesh_ui_node_filter_matches(&handshake, &handshake.nodes[4], MESH_UI_NODE_FILTER_ALL),
        "and All keeps the off-radio node, which is the list as it has always been");
    MESH_TEST_FAIL_IF(mesh_ui_node_filter_step(bogus, +1) != MESH_UI_NODE_FILTER_DIRECT,
                      "and steps on from All rather than from nowhere");
    MESH_TEST_FAIL_IF(mesh_ui_node_filter_step(MESH_UI_NODE_FILTER_ALL, -1) !=
                          (enum mesh_ui_node_filter)(MESH_UI_NODE_FILTER_COUNT - 1),
                      "a backwards step wraps rather than going negative");

    record_success(test_name);
}

/*
 * "Pinned" is what the list draws a star on, and it never draws one on us.
 *
 * A radio can carry a stale `is_favorite` on its own NodeDB entry - which the Nodes list already
 * knows, because it suppresses the star on our own row for exactly that reason, and because
 * both X and the detail's pin row refuse to toggle it. A filter reading `is_favorite` alone put
 * our own node under Pinned with no star beside it and no press that could clear it: a row the
 * reader did not put there and cannot take away.
 */
MESH_TEST_CASE(ui_nav_nodes_filter_pinned_never_keeps_our_own_node, unit) {
    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 2U;
    handshake.nodes[0].node_id = 0x1000U; /* us, flagged by the radio's own NodeDB entry */
    handshake.nodes[0].is_favorite = true;
    handshake.nodes[0].in_nodedb = true;
    handshake.nodes[1].node_id = 0x3000U; /* a node the reader actually pinned */
    handshake.nodes[1].is_favorite = true;
    handshake.nodes[1].in_nodedb = true;

    MESH_TEST_FAIL_IF(mesh_ui_node_filter_count(&handshake, MESH_UI_NODE_FILTER_PINNED) != 1U,
                      "our own node is not a pin however the radio has it flagged");
    const struct mesh_ui_node_summary *only =
        nodes_filter_at(&handshake, MESH_UI_NODE_FILTER_PINNED, 0U);
    MESH_TEST_FAIL_IF(only == NULL || only->node_id != 0x3000U,
                      "and the one that is kept is the one the reader pinned");

    /*
     * And with no MyInfo yet, nothing is ours - which is the honest answer rather than a
     * convenient one. A roster published before the handshake completes has no node number to
     * compare against, and guessing would drop a real pin from the list.
     */
    handshake.has_my_info = false;
    MESH_TEST_FAIL_IF(mesh_ui_node_filter_count(&handshake, MESH_UI_NODE_FILTER_PINNED) != 2U,
                      "before MyInfo lands no row is ours, so both pins stand");

    record_success(test_name);
}

/*
 * ---- the sort chips -------------------------------------------------------------------------
 *
 * The row under the filter is an *order*, and the three claims are the filter's three one axis
 * over: the press changes the sort, the sort changes which node row 3 is about, and the number
 * of rows does not move. That last is what separates the two strips and the reason the nav's row
 * count still asks the filter alone - a sort that quietly dropped a row would be a cursor that
 * walks off the end of a list the screen says is longer.
 */
MESH_TEST_CASE(ui_nav_nodes_sort_steps_and_renumbers_the_rows, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    /*
     * Three nodes in a published order that no single field agrees with, which is the whole
     * point: the app ranked the pinned node first and the rest by last heard, so "Recent" and
     * "Name" each have to produce a different list or they are not doing anything.
     */
    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 3U;
    handshake.nodes[0].node_id = 0x3000U; /* pinned, heard longest ago, named last */
    handshake.nodes[0].is_favorite = true;
    handshake.nodes[0].last_heard = 100U;
    snprintf(handshake.nodes[0].short_name, sizeof handshake.nodes[0].short_name, "%s", "ZULU");
    handshake.nodes[1].node_id = 0x2001U; /* heard most recently, named second */
    handshake.nodes[1].last_heard = 300U;
    snprintf(handshake.nodes[1].short_name, sizeof handshake.nodes[1].short_name, "%s", "mike");
    handshake.nodes[2].node_id = 0x2002U; /* in between, and first alphabetically */
    handshake.nodes[2].last_heard = 200U;
    snprintf(handshake.nodes[2].short_name, sizeof handshake.nodes[2].short_name, "%s", "Alfa");
    for (uint32_t i = 0; i < handshake.node_count; ++i) {
        handshake.nodes[i].in_nodedb = true;
    }
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_ui_store_consume_updates(&store, NULL);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    store.nav.screen = MESH_UI_SCREEN_NODES;
    const uint32_t rows = mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_NODES);

    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_sort != MESH_UI_NODE_SORT_DEFAULT,
                              mesh_ui_store_shutdown(&store),
                              "the list opens on the order the app published");

    /* Down onto the sort row, and A steps it. The cursor stays: everything the press moves is
       below the row it was pressed on. */
    mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.cursor[MESH_UI_SCREEN_NODES] != MESH_UI_NODES_SORT_ROW,
                              mesh_ui_store_shutdown(&store), "the sort row is under the filter's");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_sort != MESH_UI_NODE_SORT_HEARD,
                              mesh_ui_store_shutdown(&store), "A steps the filter on");
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.cursor[MESH_UI_SCREEN_NODES] != MESH_UI_NODES_SORT_ROW,
                              mesh_ui_store_shutdown(&store), "and stays on the row it pressed");
    MESH_TEST_FAIL_IF_CLEANUP(action.type != MESH_UI_ACTION_NONE, mesh_ui_store_shutdown(&store),
                              "an order asks the radio for nothing");
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_nav_row_count(&store.nav, &store, MESH_UI_SCREEN_NODES) != rows,
        mesh_ui_store_shutdown(&store), "and the list is exactly as long as it was");

    /*
     * The first row under the lead rows is now the node heard most recently rather than the
     * pinned one - read through the press, because what this is checking is that A on that row
     * opens the node the row drew.
     */
    for (uint32_t lead = 0; lead < MESH_UI_NODES_LEAD_ROWS - 1U; ++lead) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.node_detail_open || store.nav.node_detail_node != 0x2001U,
                              mesh_ui_store_shutdown(&store),
                              "Recent puts the node that spoke last over the node that is pinned");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);

    /* On to Name, where the same row is a different node again - and case is folded, so a
       lower-case name does not sort below every capital one. */
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_SORT_ROW;
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_sort != MESH_UI_NODE_SORT_NAME,
                              mesh_ui_store_shutdown(&store), "and on to Name");
    for (uint32_t lead = 0; lead < MESH_UI_NODES_LEAD_ROWS - 1U; ++lead) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.node_detail_open || store.nav.node_detail_node != 0x2002U,
                              mesh_ui_store_shutdown(&store),
                              "A to Z is the reader's alphabet, not the byte order of the case");
    mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);

    /* Five chips, so the fifth press is back where it started. */
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_SORT_ROW;
    for (uint32_t press = 0; press < (uint32_t)MESH_UI_NODE_SORT_COUNT - 2U; ++press) {
        mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    }
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_sort != MESH_UI_NODE_SORT_DEFAULT,
                              mesh_ui_store_shutdown(&store), "and wraps back to the app's order");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * Distance: nearest first, and an honest answer when there is nothing to measure from.
 *
 * Two halves. The first is the ordering, including where a node with no fix goes - below
 * everything measurable, in the order it was published, rather than at distance zero which is
 * where a struct memset to nothing would put it. The second is the sort saying it cannot work:
 * with no position of our own the list is the published order under a chip reading "Distance",
 * and the screen needs to be told so it can say so rather than looking broken.
 */
MESH_TEST_CASE(ui_nav_nodes_sort_by_distance_and_the_fix_it_needs, unit) {
    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 4U;
    for (uint32_t i = 0; i < handshake.node_count; ++i) {
        handshake.nodes[i].in_nodedb = true;
    }
    /* Us, at the origin of everything below. */
    handshake.nodes[0].node_id = 0x1000U;
    handshake.nodes[0].position.valid = true;
    handshake.nodes[0].position.latitude_i = 515000000; /* 51.5 N, 0.0 E */
    handshake.nodes[0].position.longitude_i = 0;
    /* Roughly 11 km north. */
    handshake.nodes[1].node_id = 0x2001U;
    handshake.nodes[1].position.valid = true;
    handshake.nodes[1].position.latitude_i = 516000000;
    handshake.nodes[1].position.longitude_i = 0;
    /* Nothing to place, and published between the two that can be placed. */
    handshake.nodes[2].node_id = 0x2002U;
    /* Roughly 1.1 km north: nearer than 0x2001 and published after it. */
    handshake.nodes[3].node_id = 0x2003U;
    handshake.nodes[3].position.valid = true;
    handshake.nodes[3].position.latitude_i = 515100000;
    handshake.nodes[3].position.longitude_i = 0;

    struct mesh_ui_node_view view;
    mesh_ui_node_view_build(&handshake, MESH_UI_NODE_FILTER_ALL, MESH_UI_NODE_SORT_DISTANCE, &view);
    MESH_TEST_FAIL_IF(view.count != 4U, "a sort reorders the roster, it does not shorten it");
    const uint32_t expected[4] = {0x1000U, 0x2003U, 0x2001U, 0x2002U};
    for (uint32_t i = 0; i < 4U; ++i) {
        const struct mesh_ui_node_summary *node = mesh_ui_node_view_at(&handshake, &view, i);
        MESH_TEST_FAIL_IF(node == NULL || node->node_id != expected[i],
                          "nearest first, and the node with no fix last rather than at zero");
    }
    /* And back the other way, which is how a list drawn beside an open node finds its row: by
       the node, in the order the sort put it, and past the end for one the view does not hold. */
    MESH_TEST_FAIL_IF(mesh_ui_node_view_find(&handshake, &view, 0x2001U) != 2U,
                      "a node is found at the row the sort put it on");
    MESH_TEST_FAIL_IF(mesh_ui_node_view_find(&handshake, &view, 0x4242U) != view.count,
                      "a node the view does not hold is past its end");
    MESH_TEST_FAIL_IF(!mesh_ui_node_sort_available(&handshake, MESH_UI_NODE_SORT_DISTANCE),
                      "with a fix of our own the sort has something to say");

    /*
     * Take our own fix away and nothing can be measured. Every other sort is still available:
     * they read the node in front of them, and a roster where nobody has a name is a sort with
     * nothing to reorder rather than a sort that cannot run.
     */
    handshake.nodes[0].position.valid = false;
    MESH_TEST_FAIL_IF(mesh_ui_node_sort_available(&handshake, MESH_UI_NODE_SORT_DISTANCE),
                      "without one it says so rather than drawing an order it did not make");
    for (int o = 0; o < (int)MESH_UI_NODE_SORT_COUNT; ++o) {
        if (o == (int)MESH_UI_NODE_SORT_DISTANCE) {
            continue;
        }
        MESH_TEST_FAIL_IF(!mesh_ui_node_sort_available(&handshake, (enum mesh_ui_node_sort)o),
                          "and it is the only one of them that needs anything of ours");
    }
    mesh_ui_node_view_build(&handshake, MESH_UI_NODE_FILTER_ALL, MESH_UI_NODE_SORT_DISTANCE, &view);
    for (uint32_t i = 0; i < view.count; ++i) {
        const struct mesh_ui_node_summary *node = mesh_ui_node_view_at(&handshake, &view, i);
        MESH_TEST_FAIL_IF(node == NULL || node->node_id != handshake.nodes[i].node_id,
                          "and the list it draws is the one it was published in");
    }

    record_success(test_name);
}

/*
 * A sort permutes; it never selects.
 *
 * The claim mesh_ui_nav_row_count() rests on, and the one that cannot be seen on a screenshot:
 * it asks the filter how many rows the list has and never asks the sort, so a sort that dropped
 * or duplicated a node would be a cursor walking off the end of a list, or two rows opening one
 * node. Every filter against every sort, because the pairing is where a fresh member of either
 * enum would go wrong.
 *
 * Also the out-of-range arm, which is reachable without anybody writing a bug: nav.node_sort is
 * a byte on a struct that is memcpy'd around, and what a reader can act on is a strip that has
 * come back to its first chip.
 */
MESH_TEST_CASE(ui_nav_nodes_sort_permutes_but_never_selects, unit) {
    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    handshake.node_count = 6U;
    for (uint32_t i = 0; i < handshake.node_count; ++i) {
        handshake.nodes[i].node_id = 0x1000U + i;
        handshake.nodes[i].in_nodedb = true;
        handshake.nodes[i].last_heard = (uint32_t)(600U - i * 50U);
        /* Two digits and the N is the whole of a five-byte short_name, so the counter is
           taken modulo what the format has room for rather than left as a uint32_t a reader
           of this line - the compiler included - has to assume could be ten digits. */
        const unsigned label = (unsigned)(handshake.node_count - i) % 100U;
        snprintf(handshake.nodes[i].short_name, sizeof handshake.nodes[i].short_name, "N%02u",
                 label);
    }
    /* A spread of the things each sort reads, and gaps in all of them: what is under test is the
       arithmetic, so every "cannot say" arm has to be walked as well as every comparison. */
    handshake.nodes[1].is_favorite = true;
    handshake.nodes[1].has_hops_away = true;
    handshake.nodes[1].hops_away = 2U;
    handshake.nodes[2].snr = 5.0f;
    handshake.nodes[2].has_hops_away = true;
    handshake.nodes[2].hops_away = 0U;
    handshake.nodes[3].last_heard = 0U;
    handshake.nodes[3].short_name[0] = '\0';
    handshake.nodes[4].is_favorite = true;
    handshake.nodes[4].position.valid = true;
    handshake.nodes[4].position.latitude_i = 515000000;
    handshake.nodes[4].position.longitude_i = 1000000;
    handshake.nodes[0].position.valid = true; /* ours, so Distance has a fix to work from */
    handshake.nodes[0].position.latitude_i = 515000000;
    handshake.nodes[0].position.longitude_i = 0;

    for (int f = 0; f < (int)MESH_UI_NODE_FILTER_COUNT; ++f) {
        const enum mesh_ui_node_filter filter = (enum mesh_ui_node_filter)f;
        const uint32_t kept = mesh_ui_node_filter_count(&handshake, filter);
        for (int o = 0; o <= (int)MESH_UI_NODE_SORT_COUNT; ++o) {
            /* One past the end is the out-of-range value, taken through the same checks. */
            const enum mesh_ui_node_sort sort = (enum mesh_ui_node_sort)o;
            struct mesh_ui_node_view view;
            mesh_ui_node_view_build(&handshake, filter, sort, &view);
            MESH_TEST_FAIL_IF(view.count != kept,
                              "the list is as long under every sort as the filter says it is");
            MESH_TEST_FAIL_IF(mesh_ui_node_view_at(&handshake, &view, view.count) != NULL,
                              "past the end is NULL, not the next node along");

            uint32_t seen = 0U;
            for (uint32_t i = 0; i < view.count; ++i) {
                const struct mesh_ui_node_summary *node =
                    mesh_ui_node_view_at(&handshake, &view, i);
                MESH_TEST_FAIL_IF(node == NULL, "every row of a built view is a node");
                MESH_TEST_FAIL_IF(
                    !mesh_ui_node_filter_matches(&handshake, node, filter),
                    "and every one of them is a node the chip above it said it would keep");
                const uint32_t bit = 1U << (node->node_id - 0x1000U);
                MESH_TEST_FAIL_IF((seen & bit) != 0U, "no node is on two rows at once");
                seen |= bit;
            }
        }
    }

    MESH_TEST_FAIL_IF(mesh_ui_node_sort_step((enum mesh_ui_node_sort)200, +1) !=
                          MESH_UI_NODE_SORT_HEARD,
                      "an out-of-range sort steps on from the app's order rather than from "
                      "nowhere");
    MESH_TEST_FAIL_IF(mesh_ui_node_sort_step(MESH_UI_NODE_SORT_DEFAULT, -1) !=
                          (enum mesh_ui_node_sort)(MESH_UI_NODE_SORT_COUNT - 1),
                      "and a backwards step wraps rather than going negative");
    MESH_TEST_FAIL_IF(mesh_ui_node_sort_label((enum mesh_ui_node_sort)200) !=
                          mesh_ui_node_sort_label(MESH_UI_NODE_SORT_DEFAULT),
                      "and it is named for the chip it has come back to");

    record_success(test_name);
}
