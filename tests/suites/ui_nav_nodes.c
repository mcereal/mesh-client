#define _POSIX_C_SOURCE 200809L

/* Navigating nodes and devices: favorites, disconnect/forget, PIN prompts. */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

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
        mesh_ui_node_detail_build(&handshake.nodes[0], false, 0U, NULL, false, &handshake,
                                  &store.history, items, MESH_UI_NODE_ITEMS_MAX);
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
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_TEMPERATURE, "A should open the chart");

    /* The publish that follows the press, which is where this used to be lost. */
    (void)mesh_ui_nav_clamp(&nav, &store);
    MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != row,
                      "a clamp under an open chart must not reset the detail's cursor");

    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_B, &action);
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
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_B, &action);
    MESH_TEST_FAIL_IF(nav.node_detail_open, "B off the detail should close it");
    MESH_TEST_FAIL_IF(nav.node_trend != MESH_UI_HISTORY_NONE,
                      "and no chart may outlive the detail it was a level of");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* The Nodes tab's pin: X from either level, and the detail's own row. The nav sends the state
   it wants rather than a bare toggle, so a press that races a NodeInfo cannot cancel itself. */
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
    /* Enough reported for a third and fourth group beyond the actions: the environment and the
       device metrics are separate Telemetry variants and separate cards. */
    handshake.nodes[0].environment.valid = true;
    handshake.nodes[0].environment.has_temperature = true;
    handshake.nodes[0].environment.temperature = 21.0f;
    handshake.nodes[0].metrics.valid = true;
    handshake.nodes[0].metrics.has_battery = true;
    handshake.nodes[0].metrics.battery_level = 82U;
    mesh_ui_store_set_handshake(&store, &handshake);

    struct mesh_ui_nav nav;
    mesh_ui_nav_init(&nav);
    nav.screen = MESH_UI_SCREEN_NODES;
    nav.node_detail_open = true;
    nav.node_detail_node = 0x2000U;

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count =
        mesh_ui_node_detail_build(&handshake.nodes[0], false, 0U, NULL, false, &handshake, NULL,
                                  items, MESH_UI_NODE_ITEMS_MAX);
    /* Where opening the detail leaves the cursor: the first row that is not a group title. */
    for (uint32_t i = 0U; i < count; ++i) {
        if (items[i].kind != MESH_UI_NODE_ROW_HEADING) {
            nav.cursor[MESH_UI_SCREEN_NODES] = i;
            break;
        }
    }
    uint32_t groups = 0U;
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
        (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_RIGHT, &action);
        const uint32_t at = nav.cursor[MESH_UI_SCREEN_NODES];
        MESH_TEST_FAIL_IF(at <= before, "Right should move forward to the next group");
        MESH_TEST_FAIL_IF(items[at].kind == MESH_UI_NODE_ROW_HEADING,
                          "the cursor may not land on a group title");
        MESH_TEST_FAIL_IF(at == 0U || items[at - 1U].kind != MESH_UI_NODE_ROW_HEADING,
                          "the landing should be the first row under a heading");
        MESH_TEST_FAIL_IF(nav.screen != MESH_UI_SCREEN_NODES, "the press should not change tab");
        seen[visited++] = at;
    }

    /* The last group: Right has nowhere to go and spends the press rather than changing tab. */
    const uint32_t last = nav.cursor[MESH_UI_SCREEN_NODES];
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_RIGHT, &action);
    MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != last,
                      "Right past the last group should stay put");
    MESH_TEST_FAIL_IF(nav.screen != MESH_UI_SCREEN_NODES,
                      "and must not fall through to the tab switch");

    /* Left off the top of a group goes to the group before, so the walk comes back the way it
       went. */
    for (uint32_t i = visited; i-- > 1U;) {
        (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_LEFT, &action);
        MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != seen[i - 1U],
                          "Left should retrace the groups Right walked");
    }
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_LEFT, &action);
    MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != seen[0],
                      "Left at the first group should stay put");
    MESH_TEST_FAIL_IF(nav.screen != MESH_UI_SCREEN_NODES,
                      "and must not fall through to the tab switch");

    /* And Left from inside a group is "the top of this one" before it is "the one before" -
       asked of the actions, the group whose every row is a stop of its own. */
    const uint32_t top = nav.cursor[MESH_UI_SCREEN_NODES];
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_DOWN, &action);
    MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] == top, "Down should move within a group");
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_LEFT, &action);
    MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != top,
                      "Left from inside a group should go to the top of it");

    /* The shoulders are deliberately not taken, which is what pays for the d-pad here. */
    (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_R1, &action);
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
    const uint32_t count =
        mesh_ui_node_detail_build(held, false, 0U, &store.traceroute, false, &store.handshake,
                                  &store.history, items, MESH_UI_NODE_ITEMS_MAX);
    MESH_TEST_FAIL_IF(count == 0U, "the node should produce rows");

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
                held, false, &store.traceroute, &store.handshake, &store.history, at);
            struct mesh_ui_node_span span;
            MESH_TEST_FAIL_IF(!mesh_ui_node_detail_span(items, count, rows, at, &span),
                              "no span for a stop");
            MESH_TEST_FAIL_IF(span.first > at || span.last < at, "a stop's span should hold it");
            MESH_TEST_FAIL_IF(span.card != (press == MESH_UI_NODE_PRESS_NONE),
                              "a press is a row and a fact stop is its card");
            if (press != MESH_UI_NODE_PRESS_NONE) {
                pressed[at] = true;
                for (uint32_t r = span.first; r <= span.last; ++r) {
                    mixed = mixed || (items[r].kind != MESH_UI_NODE_ROW_HEADING &&
                                      mesh_ui_node_detail_press_at(held, false, &store.traceroute,
                                                                   &store.handshake, &store.history,
                                                                   r) == MESH_UI_NODE_PRESS_NONE);
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
            for (uint32_t r = span.first; r <= span.last; ++r) {
                covered[r] = true;
            }
            if (!mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_DOWN, &action)) {
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
            MESH_TEST_FAIL_IF(!covered[r], "every row should be in view from some stop");
            MESH_TEST_FAIL_IF(mesh_ui_node_detail_press_at(held, false, &store.traceroute,
                                                           &store.handshake, &store.history,
                                                           r) != MESH_UI_NODE_PRESS_NONE &&
                                  !pressed[r],
                              "every row A acts on should be a stop");
        }
        MESH_TEST_FAIL_IF(nav.screen != MESH_UI_SCREEN_NODES,
                          "Down at the end should not change tab");

        /* Up retraces the walk, and spends the press at the top. */
        for (uint32_t i = visited; i-- > 1U;) {
            (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_UP, &action);
            MESH_TEST_FAIL_IF(nav.cursor[MESH_UI_SCREEN_NODES] != stops[i - 1U],
                              "Up should retrace the stops Down walked");
        }
        (void)mesh_ui_nav_handle_key(&nav, &store, MESH_UI_KEY_UP, &action);
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
        mesh_ui_node_detail_build(&handshake.nodes[0], false, 0U, NULL, false, &handshake,
                                  &store.history, items, MESH_UI_NODE_ITEMS_MAX);
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
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
        if (action.type != MESH_UI_ACTION_NONE) {
            mesh_ui_store_shutdown(&store);
            record_failure(test_name, "X on a lead row should do nothing");
            return;
        }
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }

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
    /* Walk to it rather than counting presses from 0: the cursor opens on the first row it may
       stand on, which is the row *under* the actions group's heading. */
    while (store.nav.cursor[MESH_UI_SCREEN_NODES] < favorite_row &&
           mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action)) {
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
    /* Past the filter and map rows, then past our own node, onto the second node in the list. */
    for (uint32_t step = 0; step < MESH_UI_NODES_LEAD_ROWS + 1U; ++step) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
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
    /* Past the lead rows, then past our own node, onto one that is not us. */
    for (uint32_t step = 0; step < MESH_UI_NODES_LEAD_ROWS + 1U; ++step) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
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

    while (store.nav.cursor[MESH_UI_SCREEN_NODES] < mute_row &&
           mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action)) {
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

/*
 * ---- the filter chips ---------------------------------------------------------------------
 *
 * The list's first row is a strip of chips and A steps it, which is three separate claims: the
 * press changes the filter, the filter changes how many rows the list has, and - the one worth
 * the test rather than a screenshot - the *row-to-node mapping* moves with it. That last is why
 * the filter went through mesh_ui_node_filter_at() rather than through an offset: a wrong
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

    /* A steps the chips and leaves the cursor where it is - this row is the one row the press
       cannot re-number. */
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_filter != MESH_UI_NODE_FILTER_DIRECT,
                              mesh_ui_store_shutdown(&store), "A steps the chips on");
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
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.node_detail_open || store.nav.node_detail_node != 0x2000U,
                              mesh_ui_store_shutdown(&store),
                              "the first row under a Direct filter is the node that was heard");
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);

    /* On to Pinned, which keeps the other one - so the same row is now a different node. */
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_FILTER_ROW;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_filter != MESH_UI_NODE_FILTER_PINNED,
                              mesh_ui_store_shutdown(&store), "and on to Pinned");
    for (uint32_t lead = 0; lead < MESH_UI_NODES_LEAD_ROWS; ++lead) {
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.node_detail_open || store.nav.node_detail_node != 0x3000U,
                              mesh_ui_store_shutdown(&store),
                              "the same row under a Pinned filter is the pinned node");
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);

    /* Three chips, so a third press is back where it started. */
    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_FILTER_ROW;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.node_filter != MESH_UI_NODE_FILTER_ALL,
                              mesh_ui_store_shutdown(&store), "and wraps back to All");

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
        mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.cursor[MESH_UI_SCREEN_NODES] != MESH_UI_NODES_MAP_ROW,
                              mesh_ui_store_shutdown(&store),
                              "the cursor can still reach the map row");

    store.nav.cursor[MESH_UI_SCREEN_NODES] = MESH_UI_NODES_FILTER_ROW;
    mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
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
        mesh_ui_node_filter_at(&handshake, MESH_UI_NODE_FILTER_DIRECT, 0U);
    MESH_TEST_FAIL_IF(first == NULL || first->node_id != 0x2000U, "and it is that node");
    MESH_TEST_FAIL_IF(
        mesh_ui_node_filter_matches(&handshake, &handshake.nodes[4], MESH_UI_NODE_FILTER_DIRECT),
        "a node the radio has forgotten is off radio on the row, so not Direct");
    MESH_TEST_FAIL_IF(mesh_ui_node_filter_at(&handshake, MESH_UI_NODE_FILTER_DIRECT, 1U) != NULL,
                      "past the end of a filter is NULL, not the next node along");

    /* Every filter agrees with the predicate it is built from, walked rather than asserted per
       node: a count and a walk that disagree is the bug the row mapping would show as a node
       opening somebody else's detail. */
    for (int f = 0; f < (int)MESH_UI_NODE_FILTER_COUNT; ++f) {
        const enum mesh_ui_node_filter filter = (enum mesh_ui_node_filter)f;
        uint32_t walked = 0U;
        while (mesh_ui_node_filter_at(&handshake, filter, walked) != NULL) {
            ++walked;
        }
        MESH_TEST_FAIL_IF(walked != mesh_ui_node_filter_count(&handshake, filter),
                          "a filter's count and its walk must be the same list");
    }

    /*
     * And a value the enum has never held reads as All rather than as an empty screen.
     *
     * nav.node_filter is a uint8_t and the nav is memcpy'd around, so this is reachable without
     * anyone writing a bug: what a reader can act on is a chip strip that has come back on All,
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
        mesh_ui_node_filter_at(&handshake, MESH_UI_NODE_FILTER_PINNED, 0U);
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
