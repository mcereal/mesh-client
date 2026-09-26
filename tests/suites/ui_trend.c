#define _POSIX_C_SOURCE 200809L

/*
 * The frame a chart draws inside: how far back it looks, how far up it goes, and what happens to
 * a reading the reader's span has left outside the picture.
 *
 * Its own suite rather than more cases in ui_history for the reason that file gives for existing:
 * the subject is different in kind. ui_history is about what the client remembers; this is about
 * what a picture of that memory is allowed to claim - and every case here is a way a chart can be
 * wrong while looking perfectly convincing, which is the one class of bug a screenshot cannot
 * catch.
 */

#include "inkcell/ui/anim.h"
#include "inkcell/ui/layout.h"

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/ui/actions.h"
#include "mesh/ui/history.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "mesh/ui/trend.h"

#include <stdlib.h>
#include <string.h>

/* The airtime domain: both readings are already permille, which is the identity scale. */
static const struct inkcell_scale k_permille = {0, 0};

static void push_every(struct inkcell_series *series, uint32_t from, uint32_t step, uint32_t count,
                       int32_t value) {
    for (uint32_t i = 0U; i < count; ++i) {
        inkcell_series_push(series, from + i * step, value);
    }
}

/*
 * The complaint this whole thing answers: a quiet mesh drawn on the whole domain is a flat line
 * along the bottom of an empty rectangle.
 *
 * 1.1% busy on 0-100% puts every reading inside the first eleven thousandths of the plot, which
 * on a 240-pixel body is under three pixels of travel - a line that cannot be seen to be doing
 * anything. Contracted to the rung above it the same readings have a fifth of the picture to move
 * in, and the axis label is what says so.
 */
MESH_TEST_CASE(trend_contracts_the_ceiling_to_a_quiet_mesh, unit) {
    const struct inkcell_scale zoomed = inkcell_trend_domain(k_permille, 11);
    MESH_TEST_FAIL_IF(zoomed.min != 0, "the floor may not move");
    MESH_TEST_FAIL_IF(zoomed.max != 20, "1.1% should be drawn on the 2% rung");
    MESH_TEST_FAIL_IF(inkcell_scale_permille(zoomed, 11) < 500,
                      "the reading should sit in the body of the plot rather than on its floor");
    record_success(test_name);
}

/*
 * And the reason it is a ladder rather than the highest reading: two visits to one screen have to
 * be comparable.
 *
 * A domain taken from the data moves on every sample, so the shape of the line says nothing about
 * the size of the reading - which is the auto-scaling lie, and on a battery it draws a cliff for
 * two percent. Readings anywhere inside one rung come back on the same ceiling.
 */
MESH_TEST_CASE(trend_rungs_are_fixed_rather_than_fitted, unit) {
    const struct inkcell_scale a = inkcell_trend_domain(k_permille, 11);
    const struct inkcell_scale b = inkcell_trend_domain(k_permille, 19);
    MESH_TEST_FAIL_IF(a.max != b.max, "two readings inside one rung should share a ceiling");

    /* A battery is the case the rule was written for: full-ish readings keep the whole domain, so
       an overnight fall of two percent is drawn as two percent of it. */
    const struct inkcell_scale battery = inkcell_trend_domain((struct inkcell_scale){0, 100}, 90);
    MESH_TEST_FAIL_IF(battery.max != 100, "a nearly full battery should keep its own domain");
    MESH_TEST_FAIL_IF(inkcell_scale_permille(battery, 88) > 900,
                      "a two-percent fall should be two percent of the plot");
    record_success(test_name);
}

/*
 * The gridlines land on values their labels can say, from the floor up.
 *
 * Every contracted airtime ceiling is a gridline, so the top label is still the rung the picture
 * contracted to; a permille axis worded in whole percent is never ruled between two of them; and
 * the node readings' own domains come out on the steps a person would rule them at.
 */
MESH_TEST_CASE(trend_ticks_rule_round_values_the_labels_can_say, unit) {
    int32_t ticks[INKCELL_TREND_TICKS_MAX];

    static const int32_t k_ceilings[] = {10, 20, 50, 100, 250, 500, 1000};
    for (size_t i = 0U; i < sizeof k_ceilings / sizeof k_ceilings[0]; ++i) {
        const uint32_t n = inkcell_trend_ticks((struct inkcell_scale){0, k_ceilings[i]}, 10, ticks,
                                               INKCELL_TREND_TICKS_MAX);
        MESH_TEST_FAIL_IF(n < 2U, "a contracted ceiling should be ruled at both ends");
        MESH_TEST_FAIL_IF(ticks[0] != 0, "the baseline should be the first gridline");
        MESH_TEST_FAIL_IF(ticks[n - 1U] != k_ceilings[i],
                          "a contracted ceiling should be a gridline");
        for (uint32_t j = 0U; j < n; ++j) {
            MESH_TEST_FAIL_IF(ticks[j] % 10 != 0, "a permille gridline should be a whole percent");
        }
    }

    /* The identity domain is permille said out loud: 0 to 100% in fifths. */
    uint32_t n =
        inkcell_trend_ticks((struct inkcell_scale){0, 0}, 10, ticks, INKCELL_TREND_TICKS_MAX);
    MESH_TEST_FAIL_IF(n != 6U || ticks[5] != 1000, "the identity domain should rule every 20%");

    n = inkcell_trend_ticks(
        (struct inkcell_scale){INKCELL_TEMPERATURE_FLOOR, INKCELL_TEMPERATURE_CEILING}, 10, ticks,
        INKCELL_TREND_TICKS_MAX);
    MESH_TEST_FAIL_IF(n != 7U || ticks[1] - ticks[0] != 200,
                      "a temperature should be ruled every twenty degrees");

    n = inkcell_trend_ticks((struct inkcell_scale){INKCELL_RSSI_FLOOR, INKCELL_RSSI_CEILING}, 1,
                            ticks, INKCELL_TREND_TICKS_MAX);
    MESH_TEST_FAIL_IF(n != 6U || ticks[0] != INKCELL_RSSI_FLOOR || ticks[5] != INKCELL_RSSI_CEILING,
                      "RSSI should be ruled floor to ceiling every 20 dBm");

    /* A descending domain is ruled from its floor downwards, and the count never passes the
       buffer the caller gave. */
    n = inkcell_trend_ticks((struct inkcell_scale){100, 0}, 1, ticks, INKCELL_TREND_TICKS_MAX);
    MESH_TEST_FAIL_IF(n != 6U || ticks[0] != 100 || ticks[5] != 0,
                      "a descending domain should be ruled from its floor down to its ceiling");
    n = inkcell_trend_ticks((struct inkcell_scale){0, 1000}, 1, ticks, 3U);
    MESH_TEST_FAIL_IF(n != 3U, "the count should never pass the room the caller gave");
    record_success(test_name);
}

/*
 * The three cases where contracting would be a guess, and the domain comes back untouched.
 *
 * A descending domain reads backwards and its "ceiling" is the smaller number, so a rung picked
 * by the ascending rule would cut the wrong end off it. A reading at the top of the domain has
 * nothing to contract to. And a domain with no width has no span to divide.
 */
MESH_TEST_CASE(trend_leaves_a_domain_it_cannot_contract, unit) {
    const struct inkcell_scale descending = {20, -20};
    MESH_TEST_FAIL_IF(inkcell_trend_domain(descending, 0).max != descending.max ||
                          inkcell_trend_domain(descending, 0).min != descending.min,
                      "a descending domain should be left alone");

    const struct inkcell_scale full = inkcell_trend_domain((struct inkcell_scale){0, 100}, 100);
    MESH_TEST_FAIL_IF(full.max != 100, "a reading at the ceiling should keep the ceiling");

    const struct inkcell_scale narrow = {7, 7};
    /* The identity domain, which is the one min == max case that is not a mistake: it means the
       readings are already permille, so it contracts against 1000 and comes back stated. */
    MESH_TEST_FAIL_IF(inkcell_trend_domain(narrow, 3).min != 7,
                      "the identity domain's floor is still its floor");
    record_success(test_name);
}

/*
 * A reading the picture does not draw does not move the axis either.
 *
 * A stroke needs two ends, so a reading that starts a segment with nothing continuing it - the
 * first report after a silence longer than the series' own gap, then silence again - is on no
 * line at all. Counted towards the ceiling it holds the axis open over the readings that *are*
 * drawn, which is precisely the flattening contracting the ceiling exists to undo: a node coming
 * back from an outage with one reading of 30% and going quiet again would put an afternoon of 1%
 * readings along the floor.
 */
MESH_TEST_CASE(trend_ceiling_ignores_a_reading_nothing_draws, unit) {
    struct inkcell_series series;
    inkcell_series_reset(&series, 60U * 1000U);
    /* Three readings a few seconds apart: one line, all of it low. */
    push_every(&series, 1000U, 5U * 1000U, 3U, 11);
    /* Then a silence past the gap, and a single high reading with nothing after it. */
    inkcell_series_push(&series, 1000U + 10U * 60U * 1000U, 300);

    MESH_TEST_FAIL_IF(!inkcell_series_starts_segment(&series, 3U),
                      "a reading after a long silence starts a segment");
    MESH_TEST_FAIL_IF(inkcell_series_starts_segment(&series, 1U),
                      "a reading a few seconds after the last one continues it");

    const struct inkcell_series *const list[] = {&series};
    struct inkcell_trend frame;
    MESH_TEST_FAIL_IF(!inkcell_trend_frame(list, 1U, k_permille, INKCELL_TREND_SPAN_ALL, &frame),
                      "four readings should frame");
    MESH_TEST_FAIL_IF(frame.scale.max > 20,
                      "an isolated reading held the ceiling open over the line that is drawn");

    /* And the same reading, once something continues it, is on a line and does count. */
    inkcell_series_push(&series, 1000U + 10U * 60U * 1000U + 5U * 1000U, 290);
    MESH_TEST_FAIL_IF(!inkcell_trend_frame(list, 1U, k_permille, INKCELL_TREND_SPAN_ALL, &frame),
                      "five readings should frame");
    MESH_TEST_FAIL_IF(frame.scale.max < 300, "a reading on a line has to fit under the ceiling");
    record_success(test_name);
}

/* The strip's four spans, in the order it draws them, walked by the d-pad and wrapping at both
   ends - a press that did nothing at the end of a set of four would be a keycap the action bar is
   still naming. */
MESH_TEST_CASE(trend_span_steps_and_wraps, unit) {
    MESH_TEST_FAIL_IF(inkcell_trend_span_step(INKCELL_TREND_SPAN_15M, 1) != INKCELL_TREND_SPAN_1H,
                      "Right should widen the span");
    MESH_TEST_FAIL_IF(inkcell_trend_span_step(INKCELL_TREND_SPAN_15M, -1) != INKCELL_TREND_SPAN_ALL,
                      "Left off the first segment should wrap to the last");
    MESH_TEST_FAIL_IF(inkcell_trend_span_step(INKCELL_TREND_SPAN_ALL, 1) != INKCELL_TREND_SPAN_15M,
                      "Right off the last segment should wrap to the first");
    /* A byte off the nav that names no span is the widest one, which is the answer that cannot
       mislead: it shows every reading there is. */
    MESH_TEST_FAIL_IF(inkcell_trend_span_ms(99U) != 0U, "an unknown span should be ALL");
    MESH_TEST_FAIL_IF(inkcell_trend_span_ms(INKCELL_TREND_SPAN_1H) != 60U * 60U * 1000U,
                      "an hour should be an hour");
    record_success(test_name);
}

/*
 * The window the span cuts, and the one property that makes the picker honest: it only ever
 * narrows.
 *
 * A span wider than the readings leaves the window at the readings' own two ends, so the caption
 * under the axis names what is drawn rather than what was asked for - and a reader who picks six
 * hours over twenty minutes of history gets the twenty minutes across the whole plot instead of a
 * line crushed into the last twentieth of it.
 */
MESH_TEST_CASE(trend_span_narrows_the_window_and_never_pads_it, unit) {
    struct inkcell_series series;
    inkcell_series_reset(&series, 0U);
    /* An hour of readings, one every five minutes. */
    push_every(&series, 1000U, 5U * 60U * 1000U, 13U, 40);

    const struct inkcell_series *const list[] = {&series};
    struct inkcell_trend frame;

    MESH_TEST_FAIL_IF(!inkcell_trend_frame(list, 1U, k_permille, INKCELL_TREND_SPAN_ALL, &frame),
                      "an hour of readings should frame");
    MESH_TEST_FAIL_IF(frame.to - frame.from != 60U * 60U * 1000U,
                      "ALL should be the readings' own span");

    MESH_TEST_FAIL_IF(!inkcell_trend_frame(list, 1U, k_permille, INKCELL_TREND_SPAN_15M, &frame),
                      "a narrowed span should still frame");
    MESH_TEST_FAIL_IF(frame.to - frame.from != 15U * 60U * 1000U,
                      "a quarter hour should be a quarter hour");

    MESH_TEST_FAIL_IF(!inkcell_trend_frame(list, 1U, k_permille, INKCELL_TREND_SPAN_6H, &frame),
                      "a span wider than the readings should still frame");
    MESH_TEST_FAIL_IF(frame.to - frame.from != 60U * 60U * 1000U,
                      "a span wider than the readings should leave the window at the readings");
    record_success(test_name);
}

/*
 * The order the frame does its two jobs in, which is the whole reason it is one function.
 *
 * The window is cut first and the ceiling is picked from what is left inside it. Done the other
 * way round, narrowing to the last quarter hour would leave the axis held open by a busy spell
 * that is no longer on the panel - an empty plot with a correct-looking label, which is this
 * component's way of being wrong quietly.
 */
MESH_TEST_CASE(trend_ceiling_follows_the_window_rather_than_the_ring, unit) {
    struct inkcell_series series;
    inkcell_series_reset(&series, 0U);
    /* Half an hour of a busy mesh, then a quarter hour of a quiet one. */
    push_every(&series, 1000U, 5U * 60U * 1000U, 6U, 300);
    push_every(&series, 1000U + 30U * 60U * 1000U, 5U * 60U * 1000U, 4U, 11);

    const struct inkcell_series *const list[] = {&series};
    struct inkcell_trend all;
    struct inkcell_trend recent;
    MESH_TEST_FAIL_IF(!inkcell_trend_frame(list, 1U, k_permille, INKCELL_TREND_SPAN_ALL, &all),
                      "the whole record should frame");
    MESH_TEST_FAIL_IF(!inkcell_trend_frame(list, 1U, k_permille, INKCELL_TREND_SPAN_15M, &recent),
                      "the last quarter hour should frame");
    MESH_TEST_FAIL_IF(all.scale.max < 300, "the busy spell should hold the axis open");
    MESH_TEST_FAIL_IF(recent.scale.max >= all.scale.max,
                      "a window with the busy spell off it should contract to what is left");
    record_success(test_name);
}

/*
 * And what a narrowed window does to the readings it left behind: they are dropped, not stacked
 * on the left-hand edge.
 *
 * inkcell_series_project_over() holds a sample outside the window at the edge it fell off, which
 * is right for a window taken from the series themselves - nothing is ever outside one. Over a
 * window the reader narrowed it draws every older reading at one x: a vertical stroke up the side
 * of the plot, in the data's own colour, that is not a reading of anything.
 */
MESH_TEST_CASE(trend_projection_drops_readings_outside_the_window, unit) {
    struct inkcell_series series;
    inkcell_series_reset(&series, 0U);
    push_every(&series, 1000U, 5U * 60U * 1000U, 13U, 40);

    const struct inkcell_series *const list[] = {&series};
    struct inkcell_trend frame;
    MESH_TEST_FAIL_IF(!inkcell_trend_frame(list, 1U, k_permille, INKCELL_TREND_SPAN_15M, &frame),
                      "a narrowed span should frame");

    struct inkcell_polyline clipped;
    struct inkcell_polyline clamped;
    inkcell_series_project_within(&series, k_permille, frame.from, frame.to, &clipped);
    inkcell_series_project_over(&series, k_permille, frame.from, frame.to, &clamped);

    MESH_TEST_FAIL_IF(clipped.count != 4U, "a quarter hour of five-minute readings is four points");
    MESH_TEST_FAIL_IF(clamped.count != series.count, "the clamping projection keeps every sample");
    MESH_TEST_FAIL_IF(!clipped.items[0].gap, "the first point drawn always starts a segment");
    MESH_TEST_FAIL_IF(clipped.items[0].x != 0 || clipped.items[clipped.count - 1U].x != 1000,
                      "the surviving readings should span the plot");
    for (uint32_t i = 1U; i < clipped.count; ++i) {
        MESH_TEST_FAIL_IF(clipped.items[i].x <= clipped.items[i - 1U].x,
                          "no two surviving readings should share an x");
        MESH_TEST_FAIL_IF(clipped.items[i].gap, "an unbroken run should stay one line");
    }
    /* And the shape it replaced: nine of the thirteen clamped points piled on the left edge. */
    uint32_t stacked = 0U;
    for (uint32_t i = 0U; i < clamped.count; ++i) {
        stacked += clamped.items[i].x == 0 ? 1U : 0U;
    }
    MESH_TEST_FAIL_IF(stacked < 2U,
                      "the clamping projection is the one that stacks; it still does");
    record_success(test_name);
}

/* A series with nothing inside the window comes back empty rather than as a line of clamped
   points - which is what lets the chart know there is nothing to draw and say so. */
MESH_TEST_CASE(trend_projection_of_an_empty_window_is_empty, unit) {
    struct inkcell_series series;
    inkcell_series_reset(&series, 0U);
    /* Both at one tick of the client's clock, which is also the case inkcell_series_window()
       refuses: two readings the clock could not separate are not an axis. */
    inkcell_series_push(&series, 1000U, 40);
    inkcell_series_push(&series, 1000U, 45);

    struct inkcell_polyline points;
    inkcell_series_project_within(&series, k_permille, 500000U, 600000U, &points);
    MESH_TEST_FAIL_IF(points.count != 0U, "a window past every reading should hold no points");

    const struct inkcell_series *const list[] = {&series};
    struct inkcell_trend frame;
    MESH_TEST_FAIL_IF(inkcell_trend_frame(list, 1U, k_permille, INKCELL_TREND_SPAN_ALL, &frame),
                      "readings a tick apart are not a window to lay an axis along");
    record_success(test_name);
}

/*
 * The span is one choice for both charts, and it is not a place.
 *
 * Every other level flag on the nav records where a tab is standing, one per tab, so that each
 * tab keeps its own. A span is how the reader likes their charts read - the theme's kind of
 * setting rather than the cursor's - so opening a node's temperature after narrowing the airtime
 * chart finds the same span already picked.
 */
MESH_TEST_CASE(trend_span_is_one_choice_across_both_charts, unit) {
    struct mesh_ui_nav nav;
    mesh_ui_nav_init(&nav);
    MESH_TEST_FAIL_IF(nav.trend_span != (uint8_t)INKCELL_TREND_SPAN_ALL,
                      "a chart should open on everything it has");
    record_success(test_name);
}

/*
 * Opens the airtime chart on a store with two readings behind it, the way the Status tab does.
 *
 * The same walk ui_status.c makes, because a chart reached any other way is a chart reached by
 * setting the flag - and the presses these cases are about are exactly the ones that would then
 * be tested against a nav nothing had clamped.
 */
static bool open_airtime_chart(struct mesh_ui_store *store) {
    struct mesh_ui_action action;
    if (mesh_ui_store_init(store) != 0) {
        return false;
    }
    mesh_test_nav_populate(store);

    struct mesh_ui_settings settings = store->settings;
    settings.stats.valid = true;
    settings.stats.channel_utilization = 11.0f;
    settings.stats.air_util_tx = 3.0f;
    mesh_ui_store_tick(store, 1000U);
    mesh_ui_store_set_settings(store, &settings);
    settings.stats.channel_utilization = 24.0f;
    mesh_ui_store_tick(store, 2000U);
    mesh_ui_store_set_settings(store, &settings);

    while (store->nav.screen != MESH_UI_SCREEN_RADIO) {
        const enum mesh_ui_screen before = store->nav.screen;
        (void)mesh_ui_store_handle_key(store, INKCELL_KEY_R1, &action);
        if (store->nav.screen == before) {
            return false;
        }
    }
    /* Past the Link card's two verbs to the Mesh card's. */
    (void)mesh_ui_store_handle_key(store, INKCELL_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(store, INKCELL_KEY_DOWN, &action);
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(store, INKCELL_KEY_A, &action);
    return store->nav.trend_open;
}

/*
 * Left and Right walk the picker; the shoulders still walk the tabs.
 *
 * This is the map's split, and the regression it guards is the state before the picker existed:
 * a chart with a control on it that the d-pad fell straight past, into a change of tab. Both
 * halves are checked in one case on purpose - taking the d-pad without leaving the shoulders
 * alone is how the map's exception would have become the client's rule.
 */
MESH_TEST_CASE(trend_left_and_right_walk_the_span_not_the_tabs, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(!open_airtime_chart(&store), "the airtime chart did not open");

    const char *failure = NULL;
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);

    const uint8_t before = store.nav.trend_span;
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_RADIO) {
        failure = "Left on a chart changed tab instead of walking the span";
        goto cleanup;
    }
    if (store.nav.trend_span == before) {
        failure = "Left on a chart should walk the span picker";
        goto cleanup;
    }
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    if (store.nav.trend_span != before) {
        failure = "Right should walk back to where Left came from";
        goto cleanup;
    }
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "picking a span asks the radio for nothing";
        goto cleanup;
    }
    /* And the shoulders, which are what pay for the d-pad being spent here. */
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    if (store.nav.screen == MESH_UI_SCREEN_RADIO) {
        failure = "the shoulders should still walk the tab strip from a chart";
        goto cleanup;
    }

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * And the action bar names it, which is the other half of the same rule: a keycap that does
 * something has to be in the table, exactly as a keycap that does nothing must not.
 *
 * Both charts, from one snapshot each, because the bar is one function called by both arms - and
 * a bar that named the span press on the airtime chart alone would be describing a difference the
 * two screens do not have.
 */
MESH_TEST_CASE(trend_action_bar_names_the_span_press, unit) {
    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    MESH_TEST_FAIL_IF(snapshot == NULL, "snapshot allocation failed");

    const char *failure = NULL;
    for (int pass = 0; pass < 2 && failure == NULL; ++pass) {
        memset(snapshot, 0, sizeof *snapshot);
        snapshot->nav.settings_section = MESH_UI_SETTINGS_NO_SECTION;
        snapshot->nav.settings_channel = MESH_UI_SETTINGS_NO_CHANNEL;
        if (pass == 0) {
            snapshot->nav.screen = MESH_UI_SCREEN_RADIO;
            snapshot->nav.devices_open = false;
            snapshot->nav.trend_open = true;
        } else {
            snapshot->nav.screen = MESH_UI_SCREEN_NODES;
            snapshot->nav.node_detail_open = true;
            snapshot->nav.node_trend = (uint8_t)MESH_UI_HISTORY_BATTERY;
        }

        struct inkcell_action_bar bar;
        mesh_ui_actions_for(snapshot, &bar);
        bool named = false;
        bool tabs = false;
        for (size_t i = 0U; i < bar.count; ++i) {
            named = named || (bar.items[i].button == INKCELL_BUTTON_LEFT_RIGHT &&
                              bar.items[i].label == MESH_STR_ACTION_SPAN);
            tabs = tabs || bar.items[i].button == INKCELL_BUTTON_SHOULDERS;
        }
        if (!named) {
            failure = "a chart's bar should name the span press";
        } else if (!tabs) {
            failure = "the shoulders are still the tabs and still have to be named";
        }
    }
    free(snapshot);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* ---- columns -------------------------------------------------------------------------------- */

/*
 * A bin is no narrower than the readings are apart and no more of them than fit, rounded up the
 * ladder.
 *
 * Each of these is a radio somebody has. A minute's DeviceMetrics over an hour is sixty one-minute
 * columns; the same stream over six hours has to double up to fit; a radio that only sends
 * LocalStats every quarter of an hour gets quarter-hour columns rather than fourteen empty slots
 * for every full one.
 */
/*
 * The readings behind the picture: the same window, listed newest first.
 *
 * Newest first is the contract rather than a presentation choice - it is what keeps a row still
 * while readings arrive, and the scroll the nav holds is an index into this order.
 */
MESH_TEST_CASE(trend_readings_list_the_window_newest_first, unit) {
    struct inkcell_series series;
    inkcell_series_reset(&series, 0U);
    for (uint32_t i = 0U; i < 5U; ++i) {
        inkcell_series_push(&series, 1000U + i * 60U * 1000U, (int32_t)(10 + i));
    }

    MESH_TEST_FAIL_IF(inkcell_trend_readings(&series, INKCELL_TREND_SPAN_ALL) != 5U,
                      "ALL holds every reading");

    struct inkcell_trend_reading row;
    MESH_TEST_FAIL_IF(!inkcell_trend_reading_at(&series, INKCELL_TREND_SPAN_ALL, 0U, &row),
                      "row 0 should be there");
    MESH_TEST_FAIL_IF(row.value != 14, "row 0 is the newest reading");
    MESH_TEST_FAIL_IF(row.before_ms != 0U, "and nothing came before it");

    MESH_TEST_FAIL_IF(!inkcell_trend_reading_at(&series, INKCELL_TREND_SPAN_ALL, 2U, &row),
                      "row 2 should be there");
    MESH_TEST_FAIL_IF(row.value != 12, "the rows count back from the newest");
    MESH_TEST_FAIL_IF(row.before_ms != 2U * 60U * 1000U,
                      "measured from the newest reading rather than from a clock");

    MESH_TEST_FAIL_IF(inkcell_trend_reading_at(&series, INKCELL_TREND_SPAN_ALL, 5U, &row),
                      "past the end is not a row");

    /* The span cuts this list exactly as it cuts the plot: four minutes of readings, and a
       quarter of an hour holds all of them while a narrower window would not. */
    MESH_TEST_FAIL_IF(inkcell_trend_readings(&series, INKCELL_TREND_SPAN_15M) != 5U,
                      "a window wider than the readings holds them all");

    /* And the one place the list says more than the picture: a single reading is no window at
       all to inkcell_series_window(), and is a perfectly good row. */
    struct inkcell_series one;
    inkcell_series_reset(&one, 0U);
    inkcell_series_push(&one, 4000U, 7);
    MESH_TEST_FAIL_IF(inkcell_trend_readings(&one, INKCELL_TREND_SPAN_ALL) != 1U,
                      "one reading is one row");
    MESH_TEST_FAIL_IF(!inkcell_trend_reading_at(&one, INKCELL_TREND_SPAN_ALL, 0U, &row) ||
                          row.value != 7,
                      "and it is the reading that was taken");

    struct inkcell_series empty;
    inkcell_series_reset(&empty, 0U);
    MESH_TEST_FAIL_IF(inkcell_trend_readings(&empty, INKCELL_TREND_SPAN_ALL) != 0U,
                      "nothing kept is no rows");
    MESH_TEST_FAIL_IF(inkcell_trend_readings(NULL, INKCELL_TREND_SPAN_ALL) != 0U,
                      "and neither is no series");
    record_success(test_name);
}

/*
 * The airtime chart has no readings list, and that is a decision rather than an omission.
 *
 * Six hours at a reading a minute is 360 rows nobody will scroll, and the screen already bins
 * them into columns precisely because reading by reading is the wrong grain for that record. So
 * Y means nothing here, and the bar must not name it - which is the same rule the span press is
 * held to one case up, applied to the one difference the two charts really do have.
 */
MESH_TEST_CASE(trend_airtime_chart_has_no_readings_list, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(!open_airtime_chart(&store), "the airtime chart did not open");

    const char *failure = NULL;
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);

    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_Y, &action);
    if (store.nav.trend_table) {
        failure = "Y on the airtime chart should not list anything";
        goto cleanup;
    }

    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    if (snapshot == NULL) {
        failure = "snapshot allocation failed";
        goto cleanup;
    }
    snapshot->nav = store.nav;
    snapshot->history = store.history;
    struct inkcell_action_bar bar;
    mesh_ui_actions_for(snapshot, &bar);
    for (size_t i = 0U; i < bar.count; ++i) {
        if (bar.items[i].button == INKCELL_BUTTON_Y ||
            bar.items[i].button == INKCELL_BUTTON_UP_DOWN) {
            failure = "the bar named a press the airtime chart does not have";
            break;
        }
    }
    free(snapshot);

cleanup:
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(trend_bin_follows_the_cadence_and_the_room, unit) {
    const uint32_t minute = 60U * 1000U;
    MESH_TEST_FAIL_IF(inkcell_trend_bin_ms(60U * minute, minute, 80U) != minute,
                      "an hour of minutes should be one-minute columns");
    MESH_TEST_FAIL_IF(inkcell_trend_bin_ms(15U * minute, minute + 400U, 80U) != minute,
                      "a report a little late is still a one-minute cadence");
    MESH_TEST_FAIL_IF(inkcell_trend_bin_ms(6U * 60U * minute, minute, 80U) != 5U * minute,
                      "six hours should widen to fit the room");
    MESH_TEST_FAIL_IF(inkcell_trend_bin_ms(6U * 60U * minute, 16U * minute, 80U) != 15U * minute,
                      "a LocalStats-only radio should get columns as wide as its reports");
    MESH_TEST_FAIL_IF(inkcell_trend_bin_ms(0U, 0U, 80U) != 1000U,
                      "nothing to measure is the narrowest rung");
    record_success(test_name);
}

static void note_minutes(struct mesh_ui_history *history, uint32_t start, uint32_t count,
                         const int32_t *utilization, int32_t tx) {
    for (uint32_t i = 0U; i < count; ++i) {
        mesh_ui_history_note_metrics_airtime(history, start + i * 60000U + (i % 3U) * 150U,
                                             utilization[i], tx);
    }
}

/*
 * A bursty minute-by-minute reading becomes one column a minute, each in its own bin even though
 * the reports jitter, and the axis fits the tallest column.
 */
MESH_TEST_CASE(trend_airtime_bins_each_report_into_its_own_column, unit) {
    struct mesh_ui_history *history = calloc(1U, sizeof *history);
    MESH_TEST_FAIL_IF(history == NULL, "allocation failed");
    mesh_ui_history_reset(history);
    int32_t values[30];
    for (uint32_t i = 0U; i < 30U; ++i) {
        values[i] = (i % 4U) == 0U ? 21 : 0; /* 2.1% one minute in four, like a quiet mesh */
    }
    note_minutes(history, 1000U, 30U, values, 8);

    struct mesh_ui_trend_airtime binned;
    const bool framed =
        mesh_ui_trend_airtime(history, (uint8_t)INKCELL_TREND_SPAN_ALL, 80U, &binned);
    bool every_minute_present = true;
    bool every_value_kept = true;
    for (uint32_t i = 0U; framed && i < binned.utilization.count; ++i) {
        every_minute_present = every_minute_present && binned.utilization.present[i] &&
                               binned.tx.present[i] && (i == 0U || binned.tx.joins[i]);
        every_value_kept = every_value_kept && binned.utilization.values[i] == values[i];
    }
    free(history);

    MESH_TEST_FAIL_IF(!framed, "thirty readings should frame a chart");
    MESH_TEST_FAIL_IF(binned.bin_ms != 60000U, "a minute's reports should be minute columns");
    MESH_TEST_FAIL_IF(binned.utilization.count != 30U, "one column per report");
    MESH_TEST_FAIL_IF(!every_minute_present, "jitter should not leave a column empty");
    MESH_TEST_FAIL_IF(!every_value_kept, "a column of one reading is that reading");
    MESH_TEST_FAIL_IF(binned.frame.scale.max != 50 || binned.frame.scale.min != 0,
                      "the ceiling should contract to the rung above the tallest column");
    record_success(test_name);
}

/*
 * Wider bins average, and a line over them bridges one skipped report but not a silence.
 */
MESH_TEST_CASE(trend_airtime_averages_and_lifts_the_pen_at_a_silence, unit) {
    struct mesh_ui_history *history = calloc(1U, sizeof *history);
    MESH_TEST_FAIL_IF(history == NULL, "allocation failed");
    mesh_ui_history_reset(history);

    /* Two readings into one bin: a narrow room forces two-minute columns over this window. */
    int32_t pair[] = {10, 30, 10, 30, 10, 30, 10, 30, 10, 30, 10, 30};
    note_minutes(history, 60000U, 12U, pair, 4);
    struct mesh_ui_trend_airtime binned;
    MESH_TEST_FAIL_IF_CLEANUP(
        !mesh_ui_trend_airtime(history, (uint8_t)INKCELL_TREND_SPAN_ALL, 8U, &binned),
        free(history), "the readings should frame a chart");
    bool averaged = binned.bin_ms == 2U * 60000U;
    for (uint32_t i = 1U; averaged && i + 1U < binned.utilization.count; ++i) {
        averaged = binned.utilization.present[i] && binned.utilization.values[i] == 20;
    }

    /* A skipped report is bridged; ten silent minutes are not. */
    mesh_ui_history_reset(history);
    const uint32_t minute = 60000U;
    const uint32_t stamps[] = {1U, 2U, 3U, 5U, 6U, 16U, 17U};
    for (uint32_t i = 0U; i < sizeof stamps / sizeof stamps[0]; ++i) {
        mesh_ui_history_note_metrics_airtime(history, stamps[i] * minute, 10, 5);
    }
    MESH_TEST_FAIL_IF_CLEANUP(
        !mesh_ui_trend_airtime(history, (uint8_t)INKCELL_TREND_SPAN_ALL, 80U, &binned),
        free(history), "the readings should frame a chart");
    free(history);
    const struct inkcell_trend_bins *tx = &binned.tx;

    MESH_TEST_FAIL_IF(!averaged, "a bin of two readings should be their mean");
    MESH_TEST_FAIL_IF(tx->count != 17U, "seventeen minutes of window is seventeen columns");
    MESH_TEST_FAIL_IF(tx->present[3], "the skipped minute is an empty column");
    MESH_TEST_FAIL_IF(!tx->joins[4], "a line should bridge one skipped report");
    MESH_TEST_FAIL_IF(!tx->present[15] || tx->joins[15], "a line should lift over a silence");
    MESH_TEST_FAIL_IF(!tx->joins[16], "and continue once readings are punctual again");
    record_success(test_name);
}

/* The span cuts the bins as it cuts a line: the last quarter hour of three hours is a quarter
   hour of columns, and the ceiling fits what is left rather than the busy spell before it. */
MESH_TEST_CASE(trend_airtime_span_cuts_the_bins, unit) {
    struct mesh_ui_history *history = calloc(1U, sizeof *history);
    MESH_TEST_FAIL_IF(history == NULL, "allocation failed");
    mesh_ui_history_reset(history);
    for (uint32_t i = 0U; i < 180U; ++i) {
        mesh_ui_history_note_metrics_airtime(history, 1000U + i * 60000U, i < 120U ? 400 : 12, 3);
    }
    struct mesh_ui_trend_airtime binned;
    const bool framed =
        mesh_ui_trend_airtime(history, (uint8_t)INKCELL_TREND_SPAN_15M, 80U, &binned);
    free(history);
    MESH_TEST_FAIL_IF(!framed, "the readings should frame a chart");
    MESH_TEST_FAIL_IF(binned.frame.to - binned.frame.from != 15U * 60000U,
                      "the window should be the span");
    MESH_TEST_FAIL_IF(binned.utilization.count != 16U || binned.bin_ms != 60000U,
                      "a quarter hour of minutes is sixteen centred columns");
    MESH_TEST_FAIL_IF(binned.frame.scale.max != 20,
                      "the ceiling should fit the quarter hour, not the busy spell before it");
    record_success(test_name);
}

/*
 * A silence the bins are too wide to show still lifts the line.
 *
 * From review: five-minute bins put the readings either side of a four-minute outage in adjacent
 * bins, and joining by adjacency drew straight across it. The rule is relative to the readings'
 * own spacing, so a radio that reports every quarter hour is still one line.
 */
MESH_TEST_CASE(trend_airtime_lifts_the_pen_over_a_silence_inside_wide_bins, unit) {
    struct mesh_ui_history *history = calloc(1U, sizeof *history);
    MESH_TEST_FAIL_IF(history == NULL, "allocation failed");
    const uint32_t minute = 60000U;

    /* Once a minute for an hour, silent from minute 30 to 34. */
    mesh_ui_history_reset(history);
    for (uint32_t m = 0U; m <= 60U; ++m) {
        if (m <= 30U || m >= 34U) {
            mesh_ui_history_note_metrics_airtime(history, minute + m * minute, 10, 5);
        }
    }
    struct mesh_ui_trend_airtime binned;
    MESH_TEST_FAIL_IF_CLEANUP(
        !mesh_ui_trend_airtime(history, (uint8_t)INKCELL_TREND_SPAN_ALL, 13U, &binned),
        free(history), "the readings should frame a chart");
    const struct inkcell_trend_bins wide = binned.tx;
    const uint32_t wide_bin = binned.bin_ms;

    /* A quarter-hour radio: every reading well past the three-minute gap, and one line. */
    mesh_ui_history_reset(history);
    for (uint32_t r = 0U; r < 8U; ++r) {
        mesh_ui_history_note_airtime(history, minute + r * 16U * minute, 10, 5);
    }
    MESH_TEST_FAIL_IF_CLEANUP(
        !mesh_ui_trend_airtime(history, (uint8_t)INKCELL_TREND_SPAN_ALL, 80U, &binned),
        free(history), "the readings should frame a chart");
    free(history);
    bool quarter_hour_joined = true;
    for (uint32_t i = 1U; i < binned.tx.count; ++i) {
        quarter_hour_joined = quarter_hour_joined && (!binned.tx.present[i] || binned.tx.joins[i]);
    }

    /* Bins centred back from minute 60: the one centred on 35 is the first after the silence. */
    MESH_TEST_FAIL_IF(wide_bin != 5U * minute, "an hour in thirteen bins is five-minute columns");
    MESH_TEST_FAIL_IF(!wide.present[6] || !wide.present[7],
                      "both sides of the silence have columns");
    MESH_TEST_FAIL_IF(wide.joins[7], "the line should lift over a silence inside wide bins");
    MESH_TEST_FAIL_IF(!wide.joins[8], "and join again once reports are punctual");
    MESH_TEST_FAIL_IF(!quarter_hour_joined, "a quarter-hour radio should still be one line");
    record_success(test_name);
}
