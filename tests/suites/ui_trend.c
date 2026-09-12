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

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/ui/actions.h"
#include "mesh/ui/anim.h"
#include "mesh/ui/history.h"
#include "mesh/ui/layout.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "mesh/ui/trend.h"

#include <stdlib.h>
#include <string.h>

/* The airtime domain: both readings are already permille, which is the identity scale. */
static const struct mesh_ui_scale k_permille = {0, 0};

static void push_every(struct mesh_ui_series *series, uint32_t from, uint32_t step, uint32_t count,
                       int32_t value) {
    for (uint32_t i = 0U; i < count; ++i) {
        mesh_ui_series_push(series, from + i * step, value);
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
    const struct mesh_ui_scale zoomed = mesh_ui_trend_domain(k_permille, 11);
    MESH_TEST_FAIL_IF(zoomed.min != 0, "the floor may not move");
    MESH_TEST_FAIL_IF(zoomed.max != 20, "1.1% should be drawn on the 2% rung");
    MESH_TEST_FAIL_IF(mesh_ui_scale_permille(zoomed, 11) < 500,
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
    const struct mesh_ui_scale a = mesh_ui_trend_domain(k_permille, 11);
    const struct mesh_ui_scale b = mesh_ui_trend_domain(k_permille, 19);
    MESH_TEST_FAIL_IF(a.max != b.max, "two readings inside one rung should share a ceiling");

    /* A battery is the case the rule was written for: full-ish readings keep the whole domain, so
       an overnight fall of two percent is drawn as two percent of it. */
    const struct mesh_ui_scale battery = mesh_ui_trend_domain((struct mesh_ui_scale){0, 100}, 90);
    MESH_TEST_FAIL_IF(battery.max != 100, "a nearly full battery should keep its own domain");
    MESH_TEST_FAIL_IF(mesh_ui_scale_permille(battery, 88) > 900,
                      "a two-percent fall should be two percent of the plot");
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
    const struct mesh_ui_scale descending = {20, -20};
    MESH_TEST_FAIL_IF(mesh_ui_trend_domain(descending, 0).max != descending.max ||
                          mesh_ui_trend_domain(descending, 0).min != descending.min,
                      "a descending domain should be left alone");

    const struct mesh_ui_scale full = mesh_ui_trend_domain((struct mesh_ui_scale){0, 100}, 100);
    MESH_TEST_FAIL_IF(full.max != 100, "a reading at the ceiling should keep the ceiling");

    const struct mesh_ui_scale narrow = {7, 7};
    /* The identity domain, which is the one min == max case that is not a mistake: it means the
       readings are already permille, so it contracts against 1000 and comes back stated. */
    MESH_TEST_FAIL_IF(mesh_ui_trend_domain(narrow, 3).min != 7,
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
    struct mesh_ui_series series;
    mesh_ui_series_reset(&series, 60U * 1000U);
    /* Three readings a few seconds apart: one line, all of it low. */
    push_every(&series, 1000U, 5U * 1000U, 3U, 11);
    /* Then a silence past the gap, and a single high reading with nothing after it. */
    mesh_ui_series_push(&series, 1000U + 10U * 60U * 1000U, 300);

    MESH_TEST_FAIL_IF(!mesh_ui_series_starts_segment(&series, 3U),
                      "a reading after a long silence starts a segment");
    MESH_TEST_FAIL_IF(mesh_ui_series_starts_segment(&series, 1U),
                      "a reading a few seconds after the last one continues it");

    const struct mesh_ui_series *const list[] = {&series};
    struct mesh_ui_trend frame;
    MESH_TEST_FAIL_IF(!mesh_ui_trend_frame(list, 1U, k_permille, MESH_UI_TREND_SPAN_ALL, &frame),
                      "four readings should frame");
    MESH_TEST_FAIL_IF(frame.scale.max > 20,
                      "an isolated reading held the ceiling open over the line that is drawn");

    /* And the same reading, once something continues it, is on a line and does count. */
    mesh_ui_series_push(&series, 1000U + 10U * 60U * 1000U + 5U * 1000U, 290);
    MESH_TEST_FAIL_IF(!mesh_ui_trend_frame(list, 1U, k_permille, MESH_UI_TREND_SPAN_ALL, &frame),
                      "five readings should frame");
    MESH_TEST_FAIL_IF(frame.scale.max < 300, "a reading on a line has to fit under the ceiling");
    record_success(test_name);
}

/* The strip's four spans, in the order it draws them, walked by the d-pad and wrapping at both
   ends - a press that did nothing at the end of a set of four would be a keycap the action bar is
   still naming. */
MESH_TEST_CASE(trend_span_steps_and_wraps, unit) {
    MESH_TEST_FAIL_IF(mesh_ui_trend_span_step(MESH_UI_TREND_SPAN_15M, 1) != MESH_UI_TREND_SPAN_1H,
                      "Right should widen the span");
    MESH_TEST_FAIL_IF(mesh_ui_trend_span_step(MESH_UI_TREND_SPAN_15M, -1) != MESH_UI_TREND_SPAN_ALL,
                      "Left off the first segment should wrap to the last");
    MESH_TEST_FAIL_IF(mesh_ui_trend_span_step(MESH_UI_TREND_SPAN_ALL, 1) != MESH_UI_TREND_SPAN_15M,
                      "Right off the last segment should wrap to the first");
    /* A byte off the nav that names no span is the widest one, which is the answer that cannot
       mislead: it shows every reading there is. */
    MESH_TEST_FAIL_IF(mesh_ui_trend_span_ms(99U) != 0U, "an unknown span should be ALL");
    MESH_TEST_FAIL_IF(mesh_ui_trend_span_ms(MESH_UI_TREND_SPAN_1H) != 60U * 60U * 1000U,
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
    struct mesh_ui_series series;
    mesh_ui_series_reset(&series, 0U);
    /* An hour of readings, one every five minutes. */
    push_every(&series, 1000U, 5U * 60U * 1000U, 13U, 40);

    const struct mesh_ui_series *const list[] = {&series};
    struct mesh_ui_trend frame;

    MESH_TEST_FAIL_IF(!mesh_ui_trend_frame(list, 1U, k_permille, MESH_UI_TREND_SPAN_ALL, &frame),
                      "an hour of readings should frame");
    MESH_TEST_FAIL_IF(frame.to - frame.from != 60U * 60U * 1000U,
                      "ALL should be the readings' own span");

    MESH_TEST_FAIL_IF(!mesh_ui_trend_frame(list, 1U, k_permille, MESH_UI_TREND_SPAN_15M, &frame),
                      "a narrowed span should still frame");
    MESH_TEST_FAIL_IF(frame.to - frame.from != 15U * 60U * 1000U,
                      "a quarter hour should be a quarter hour");

    MESH_TEST_FAIL_IF(!mesh_ui_trend_frame(list, 1U, k_permille, MESH_UI_TREND_SPAN_6H, &frame),
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
    struct mesh_ui_series series;
    mesh_ui_series_reset(&series, 0U);
    /* Half an hour of a busy mesh, then a quarter hour of a quiet one. */
    push_every(&series, 1000U, 5U * 60U * 1000U, 6U, 300);
    push_every(&series, 1000U + 30U * 60U * 1000U, 5U * 60U * 1000U, 4U, 11);

    const struct mesh_ui_series *const list[] = {&series};
    struct mesh_ui_trend all;
    struct mesh_ui_trend recent;
    MESH_TEST_FAIL_IF(!mesh_ui_trend_frame(list, 1U, k_permille, MESH_UI_TREND_SPAN_ALL, &all),
                      "the whole record should frame");
    MESH_TEST_FAIL_IF(!mesh_ui_trend_frame(list, 1U, k_permille, MESH_UI_TREND_SPAN_15M, &recent),
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
 * mesh_ui_series_project_over() holds a sample outside the window at the edge it fell off, which
 * is right for a window taken from the series themselves - nothing is ever outside one. Over a
 * window the reader narrowed it draws every older reading at one x: a vertical stroke up the side
 * of the plot, in the data's own colour, that is not a reading of anything.
 */
MESH_TEST_CASE(trend_projection_drops_readings_outside_the_window, unit) {
    struct mesh_ui_series series;
    mesh_ui_series_reset(&series, 0U);
    push_every(&series, 1000U, 5U * 60U * 1000U, 13U, 40);

    const struct mesh_ui_series *const list[] = {&series};
    struct mesh_ui_trend frame;
    MESH_TEST_FAIL_IF(!mesh_ui_trend_frame(list, 1U, k_permille, MESH_UI_TREND_SPAN_15M, &frame),
                      "a narrowed span should frame");

    struct mesh_ui_polyline clipped;
    struct mesh_ui_polyline clamped;
    mesh_ui_series_project_within(&series, k_permille, frame.from, frame.to, &clipped);
    mesh_ui_series_project_over(&series, k_permille, frame.from, frame.to, &clamped);

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
    struct mesh_ui_series series;
    mesh_ui_series_reset(&series, 0U);
    /* Both at one tick of the client's clock, which is also the case mesh_ui_series_window()
       refuses: two readings the clock could not separate are not an axis. */
    mesh_ui_series_push(&series, 1000U, 40);
    mesh_ui_series_push(&series, 1000U, 45);

    struct mesh_ui_polyline points;
    mesh_ui_series_project_within(&series, k_permille, 500000U, 600000U, &points);
    MESH_TEST_FAIL_IF(points.count != 0U, "a window past every reading should hold no points");

    const struct mesh_ui_series *const list[] = {&series};
    struct mesh_ui_trend frame;
    MESH_TEST_FAIL_IF(mesh_ui_trend_frame(list, 1U, k_permille, MESH_UI_TREND_SPAN_ALL, &frame),
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
    MESH_TEST_FAIL_IF(nav.trend_span != (uint8_t)MESH_UI_TREND_SPAN_ALL,
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

    while (store->nav.screen != MESH_UI_SCREEN_STATUS) {
        const enum mesh_ui_screen before = store->nav.screen;
        (void)mesh_ui_store_handle_key(store, MESH_UI_KEY_RIGHT, &action);
        if (store->nav.screen == before) {
            return false;
        }
    }
    (void)mesh_ui_store_handle_key(store, MESH_UI_KEY_DOWN, &action);
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(store, MESH_UI_KEY_A, &action);
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
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    if (store.nav.screen != MESH_UI_SCREEN_STATUS) {
        failure = "Left on a chart changed tab instead of walking the span";
        goto cleanup;
    }
    if (store.nav.trend_span == before) {
        failure = "Left on a chart should walk the span picker";
        goto cleanup;
    }
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    if (store.nav.trend_span != before) {
        failure = "Right should walk back to where Left came from";
        goto cleanup;
    }
    if (action.type != MESH_UI_ACTION_NONE) {
        failure = "picking a span asks the radio for nothing";
        goto cleanup;
    }
    /* And the shoulders, which are what pay for the d-pad being spent here. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_R1, &action);
    if (store.nav.screen == MESH_UI_SCREEN_STATUS) {
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
            snapshot->nav.screen = MESH_UI_SCREEN_STATUS;
            snapshot->nav.trend_open = true;
        } else {
            snapshot->nav.screen = MESH_UI_SCREEN_NODES;
            snapshot->nav.node_detail_open = true;
            snapshot->nav.node_trend = (uint8_t)MESH_UI_HISTORY_BATTERY;
        }

        struct mesh_ui_action_bar bar;
        mesh_ui_actions_for(snapshot, &bar);
        bool named = false;
        bool tabs = false;
        for (size_t i = 0U; i < bar.count; ++i) {
            named = named || (bar.items[i].button == MESH_UI_BUTTON_LEFT_RIGHT &&
                              bar.items[i].label == MESH_STR_ACTION_SPAN);
            tabs = tabs || bar.items[i].button == MESH_UI_BUTTON_SHOULDERS;
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
