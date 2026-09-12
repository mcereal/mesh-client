#define _POSIX_C_SOURCE 200809L

/*
 * The client's memory of what it has been told: the sample ring, the projection a sparkline is
 * drawn from, and the slots the history keeps them in.
 *
 * Its own suite rather than more cases in ui_layout because the subject is different in kind.
 * Everything in ui_layout answers a question about the frame being drawn now; everything here
 * is about readings the frame no longer has - which is exactly what makes it the half of the
 * sparkline that a picture cannot check. A trend drawn from a wrong projection looks like a
 * trend.
 */

#include "framework/mesh_test.h"

#include "mesh/ui/anim.h"
#include "mesh/ui/history.h"
#include "mesh/ui/layout.h"
#include "mesh/ui/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

MESH_TEST_CASE(series_keeps_the_newest_readings, unit) {
    struct mesh_ui_series series;
    mesh_ui_series_reset(&series, 1000U);
    MESH_TEST_FAIL_IF(mesh_ui_series_newest(&series) != NULL, "a reset series should be empty");

    for (uint32_t i = 0U; i < MESH_UI_SERIES_MAX + 5U; ++i) {
        mesh_ui_series_push(&series, i * 100U, (int32_t)i);
    }
    MESH_TEST_FAIL_IF(series.count != MESH_UI_SERIES_MAX, "the ring should cap at its size");

    /* The oldest five fell off the front, so the window is the newest MESH_UI_SERIES_MAX. */
    const struct mesh_ui_sample *oldest = mesh_ui_series_at(&series, 0U);
    const struct mesh_ui_sample *newest = mesh_ui_series_newest(&series);
    MESH_TEST_FAIL_IF(oldest == NULL || oldest->value != 5, "the oldest reading should be evicted");
    MESH_TEST_FAIL_IF(newest == NULL || newest->value != (int32_t)(MESH_UI_SERIES_MAX + 4U),
                      "the newest push should be the newest reading");
    MESH_TEST_FAIL_IF(mesh_ui_series_at(&series, MESH_UI_SERIES_MAX) != NULL,
                      "reading past the end should be refused");
    record_success(test_name);
}

/*
 * A clock that goes backwards empties the series rather than being clamped or ignored.
 *
 * The readings are still true; *when* they were taken is what is gone, and a series with no
 * usable time axis is not a series. See mesh_ui_series_push().
 */
MESH_TEST_CASE(series_drops_history_when_the_clock_goes_back, unit) {
    struct mesh_ui_series series;
    mesh_ui_series_reset(&series, 0U);
    mesh_ui_series_push(&series, 5000U, 10);
    mesh_ui_series_push(&series, 6000U, 20);
    mesh_ui_series_push(&series, 100U, 30);

    MESH_TEST_FAIL_IF(series.count != 1U, "a backwards clock should leave only the new reading");
    const struct mesh_ui_sample *newest = mesh_ui_series_newest(&series);
    MESH_TEST_FAIL_IF(newest == NULL || newest->value != 30 || newest->time != 100U,
                      "the reading that reset the series should still be kept");
    record_success(test_name);
}

/*
 * The x axis is time, not the sample number - which is the whole reason a series carries a
 * stamp at all.
 *
 * Three readings at 0, 9 and 10 seconds describe something that was steady and then moved, and
 * evenly spaced points would draw it as something that moved throughout. The middle sample
 * lands at nine tenths across, not at a half.
 */
MESH_TEST_CASE(series_projects_x_on_time_and_y_on_the_domain, unit) {
    struct mesh_ui_series series;
    mesh_ui_series_reset(&series, 0U);
    mesh_ui_series_push(&series, 0U, 0);
    mesh_ui_series_push(&series, 9000U, 50);
    mesh_ui_series_push(&series, 10000U, 100);

    struct mesh_ui_polyline points;
    const struct mesh_ui_scale scale = {0, 100};
    mesh_ui_series_project(&series, scale, &points);

    MESH_TEST_FAIL_IF(points.count != 3U, "every sample should be projected");
    MESH_TEST_FAIL_IF(points.items[0].x != 0 || points.items[2].x != MESH_UI_ANIM_ONE,
                      "the ends of the span should be the ends of the box");
    MESH_TEST_FAIL_IF(points.items[1].x != 900, "x should follow the clock, not the sample index");
    MESH_TEST_FAIL_IF(points.items[1].y != 500,
                      "y should be the reading on its own domain, not on the samples' range");
    MESH_TEST_FAIL_IF(!points.items[0].gap, "the first point continues nothing");
    MESH_TEST_FAIL_IF(points.items[1].gap || points.items[2].gap,
                      "readings inside the gap window should be one line");

    /*
     * And the domain is the *stated* one. These three readings span 0..100 of a 0..1000 scale,
     * so a line that rescaled itself to its data would put the middle sample at half height
     * again - which is the difference between a battery that fell five percent and a cliff.
     */
    const struct mesh_ui_scale wide = {0, 1000};
    mesh_ui_series_project(&series, wide, &points);
    MESH_TEST_FAIL_IF(points.items[1].y != 50, "the domain decides the height, not the samples");
    record_success(test_name);
}

/*
 * Two series on one picture are measured on one window.
 *
 * This is the projection's own rule arriving one component along, and it is the way a chart is
 * wrong quietly. A series projected on its *own* span fills whatever box it is handed, so two of
 * them - one still arriving, one that stopped an hour ago - come out the same width, and the
 * second is drawn as though it were current. On the airtime chart that is our transmit share
 * being drawn as though it had climbed to meet the channel's total.
 */
MESH_TEST_CASE(series_share_one_window_across_a_chart, unit) {
    struct mesh_ui_series busy;
    struct mesh_ui_series ours;
    mesh_ui_series_reset(&busy, 0U);
    mesh_ui_series_reset(&ours, 0U);
    /* The channel is still reporting; ours stopped at the halfway mark. */
    mesh_ui_series_push(&busy, 0U, 10);
    mesh_ui_series_push(&busy, 5000U, 20);
    mesh_ui_series_push(&busy, 10000U, 30);
    mesh_ui_series_push(&ours, 0U, 1);
    mesh_ui_series_push(&ours, 5000U, 2);

    const struct mesh_ui_series *const both[] = {&busy, &ours};
    uint32_t from = 42U;
    uint32_t to = 42U;
    MESH_TEST_FAIL_IF(!mesh_ui_series_window(both, 2U, &from, &to), "two series frame a window");
    MESH_TEST_FAIL_IF(from != 0U || to != 10000U,
                      "the window is the union of the series, not one of them");

    const struct mesh_ui_scale scale = {0, 100};
    struct mesh_ui_polyline points;
    mesh_ui_series_project_over(&ours, scale, from, to, &points);
    MESH_TEST_FAIL_IF(points.count != 2U, "every sample should still be projected");
    MESH_TEST_FAIL_IF(points.items[1].x != MESH_UI_ANIM_ONE / 2,
                      "a series that stopped halfway through should end halfway across");

    /* And the one that reaches the end of the window still reaches the end of the box, so the
       two are directly comparable rather than merely both present. */
    mesh_ui_series_project_over(&busy, scale, from, to, &points);
    MESH_TEST_FAIL_IF(points.items[2].x != MESH_UI_ANIM_ONE,
                      "the series that runs to the window's end should reach the right edge");

    /* A window with no width to it is refused rather than answered with a point. The client's
       clock is monotonic and one report stamps both series, so this is a clock too coarse to
       separate two pushes - and the caller's answer to it is to draw no span, not a zero one. */
    struct mesh_ui_series flat;
    mesh_ui_series_reset(&flat, 0U);
    mesh_ui_series_push(&flat, 7000U, 1);
    mesh_ui_series_push(&flat, 7000U, 2);
    const struct mesh_ui_series *const one[] = {&flat};
    MESH_TEST_FAIL_IF(mesh_ui_series_window(one, 1U, &from, &to),
                      "a window of zero width is not a window");

    /* Nothing to frame, in the three ways there are. An empty series is not an error - the pair
       on the Status card start empty and fill one report at a time. */
    struct mesh_ui_series empty;
    mesh_ui_series_reset(&empty, 0U);
    const struct mesh_ui_series *const nothing[] = {&empty, NULL};
    MESH_TEST_FAIL_IF(mesh_ui_series_window(nothing, 2U, &from, &to), "empty series framed one");
    MESH_TEST_FAIL_IF(mesh_ui_series_window(NULL, 2U, &from, &to), "no series framed one");
    MESH_TEST_FAIL_IF(mesh_ui_series_window(both, 0U, &from, &to), "no count framed one");
    record_success(test_name);
}

/*
 * A reading outside the window is held at the edge it fell off, and the fallback is the
 * projection's own.
 *
 * The holding matters because the arithmetic is unsigned: a sample older than the window would
 * otherwise come out most of a picture to the *right* of everything that happened after it,
 * which is a line drawn backwards rather than a line drawn wrongly.
 */
MESH_TEST_CASE(series_projected_over_a_window_stays_inside_it, unit) {
    struct mesh_ui_series series;
    mesh_ui_series_reset(&series, 0U);
    mesh_ui_series_push(&series, 1000U, 10);
    mesh_ui_series_push(&series, 2000U, 20);
    mesh_ui_series_push(&series, 3000U, 30);

    const struct mesh_ui_scale scale = {0, 100};
    struct mesh_ui_polyline points;
    /* A window that starts after the first reading and ends before the last. */
    mesh_ui_series_project_over(&series, scale, 2000U, 2500U, &points);
    MESH_TEST_FAIL_IF(points.items[0].x != 0,
                      "a reading before the window should hold at its left");
    MESH_TEST_FAIL_IF(points.items[1].x != 0, "the window's own start is its left edge");
    MESH_TEST_FAIL_IF(points.items[2].x != MESH_UI_ANIM_ONE,
                      "a reading after the window should hold at its right");

    /* A window with no width falls back to even spacing, which is mesh_ui_series_project()'s own
       answer for a span of zero: the order of the samples is then all that is known about them. */
    mesh_ui_series_project_over(&series, scale, 5000U, 5000U, &points);
    MESH_TEST_FAIL_IF(points.items[0].x != 0 || points.items[1].x != MESH_UI_ANIM_ONE / 2 ||
                          points.items[2].x != MESH_UI_ANIM_ONE,
                      "a window of no width should space the samples evenly");

    /* And the ordinary window is what mesh_ui_series_project() itself produces, because that is
       what it is written in terms of. Two projections that could differ is two answers to where
       a reading goes. */
    struct mesh_ui_polyline own;
    mesh_ui_series_project(&series, scale, &own);
    mesh_ui_series_project_over(&series, scale, 1000U, 3000U, &points);
    for (uint32_t i = 0U; i < own.count; ++i) {
        MESH_TEST_FAIL_IF(own.items[i].x != points.items[i].x ||
                              own.items[i].y != points.items[i].y ||
                              own.items[i].gap != points.items[i].gap,
                          "a series' own span should project exactly as the window does");
    }
    record_success(test_name);
}

/*
 * A line exists when two adjacent readings are one, which is not the same as there being two.
 *
 * The predicate and the projection have to agree, because the failure they can have between them
 * is silent in the worst way: a screen that offers a picture and a renderer that then declines to
 * draw one, leaving axes and a legend around nothing. They share the break test for that reason,
 * and this walks the three ways a series can be samples without being a line.
 */
MESH_TEST_CASE(series_offers_a_line_only_when_one_can_be_drawn, unit) {
    struct mesh_ui_series series;

    /* Nothing, and one reading: a level, and there is a component for that. */
    mesh_ui_series_reset(&series, 1000U);
    MESH_TEST_FAIL_IF(mesh_ui_series_has_segment(&series), "an empty series is not a line");
    mesh_ui_series_push(&series, 0U, 10);
    MESH_TEST_FAIL_IF(mesh_ui_series_has_segment(&series), "one reading is not a line");
    MESH_TEST_FAIL_IF(mesh_ui_series_has_segment(NULL), "no series is not a line");

    /* Two, with a silence between them. The pen lifts at the second, so there is no stroke. */
    mesh_ui_series_reset(&series, 1000U);
    mesh_ui_series_push(&series, 0U, 10);
    mesh_ui_series_push(&series, 5000U, 20);
    MESH_TEST_FAIL_IF(mesh_ui_series_has_segment(&series),
                      "two readings across a silence are two points, not a line");

    /* Two, with a break the clock cannot see - a reading refused rather than missing. */
    mesh_ui_series_reset(&series, 0U);
    mesh_ui_series_push(&series, 0U, 10);
    mesh_ui_series_break(&series);
    mesh_ui_series_push(&series, 100U, 20);
    MESH_TEST_FAIL_IF(mesh_ui_series_has_segment(&series),
                      "a break the source declared is still a break");

    /* And one unbroken pair anywhere in the ring is a line, wherever the breaks are around it. */
    mesh_ui_series_push(&series, 200U, 30);
    MESH_TEST_FAIL_IF(!mesh_ui_series_has_segment(&series),
                      "a reading that continues the one before it is a line");

    /*
     * The two answers are one answer. Whatever the shape of the series, "this has a segment" and
     * "the projection draws a stroke" must be the same claim - so the projection is walked for a
     * point that continues its predecessor and the two are compared.
     */
    static const uint32_t k_times[] = {0U, 400U, 5000U, 5400U, 5800U, 12000U};
    for (uint32_t take = 0U; take <= sizeof k_times / sizeof k_times[0]; ++take) {
        mesh_ui_series_reset(&series, 1000U);
        for (uint32_t i = 0U; i < take; ++i) {
            mesh_ui_series_push(&series, k_times[i], (int32_t)i * 10);
        }
        struct mesh_ui_polyline points;
        mesh_ui_series_project(&series, (struct mesh_ui_scale){0, 100}, &points);
        bool drawn = false;
        for (uint32_t i = 1U; i < points.count; ++i) {
            drawn = drawn || !points.items[i].gap;
        }
        MESH_TEST_FAIL_IF(drawn != mesh_ui_series_has_segment(&series),
                          "the predicate and the projection disagree about whether there is a "
                          "line");
    }
    record_success(test_name);
}

/*
 * Whether there is a trend to draw at all, asked once.
 *
 * Three places read it - the verb table that offers the chart, the action bar that names the
 * press and the clamp that closes the screen when it empties - and a fourth counting samples by
 * hand would be a fourth opinion about whether a screen exists.
 */
MESH_TEST_CASE(history_says_when_there_is_an_airtime_trend, unit) {
    struct mesh_ui_history history;
    mesh_ui_history_reset(&history);
    MESH_TEST_FAIL_IF(mesh_ui_history_has_airtime(&history), "an empty history has no trend");

    mesh_ui_history_note_airtime(&history, 1000U, 110, 30);
    MESH_TEST_FAIL_IF(mesh_ui_history_has_airtime(&history), "one reading is a level, not a trend");

    mesh_ui_history_note_airtime(&history, 2000U, 140, 40);
    MESH_TEST_FAIL_IF(!mesh_ui_history_has_airtime(&history),
                      "two readings a minute apart make a line");

    /*
     * And two readings a break apart are not a line either, which is the case a count cannot
     * see. A link down for a quarter of an hour and then back is two samples the ring holds and
     * a silence the series calls a break - so the projection lifts the pen at the second one and
     * a chart drawn from them has axes, a legend and nothing between them.
     *
     * The test is a *drawable segment* rather than a sample count for that reason: what the verb
     * promises is a picture, and the honest answer when there is none is not to offer it.
     */
    mesh_ui_history_reset(&history);
    mesh_ui_history_note_airtime(&history, 1000U, 110, 30);
    mesh_ui_history_note_airtime(&history, 1000U + MESH_UI_HISTORY_RADIO_GAP_MS + 1000U, 140, 40);
    MESH_TEST_FAIL_IF(mesh_ui_history_has_airtime(&history),
                      "two readings with a silence between them draw no line");

    /* One more punctual reading after it, and there is a segment again - the break is at the
       sample that follows the silence, not at every sample after it. */
    mesh_ui_history_note_airtime(&history, 1000U + MESH_UI_HISTORY_RADIO_GAP_MS + 61000U, 150, 45);
    MESH_TEST_FAIL_IF(!mesh_ui_history_has_airtime(&history),
                      "a reading that continues the one before it is a line");

    /* And a radio swap takes it with the roster, which is what closes the chart under a reader
       looking at a mesh that is no longer theirs. */
    mesh_ui_history_forget(&history);
    MESH_TEST_FAIL_IF(mesh_ui_history_has_airtime(&history),
                      "a forgotten history should offer nothing to draw");
    MESH_TEST_FAIL_IF(mesh_ui_history_has_airtime(NULL), "no history is no trend");
    record_success(test_name);
}

/*
 * A series has to survive the cadence its own readings arrive on.
 *
 * This is the test the two next door cannot make. Both of them space their pushes against
 * MESH_UI_HISTORY_RADIO_GAP_MS - a minute apart is inside it, the gap plus a second is outside
 * it - so they go on passing whatever that constant is, including the fifteen minutes that made
 * it exactly one LocalStats report. Written that way round the arithmetic is always consistent
 * and always about nothing: a gap no larger than the cadence breaks at every sample, so
 * mesh_ui_history_has_airtime() is false for the life of a session, the Mesh card is offered no
 * verb and the Status cursor walks from the Link card to the Radio one. Every capture scene
 * stamps its readings milliseconds apart on the harness clock, so nothing but a device showed it.
 *
 * So the pushes here are spaced by the *cadence*, which is what the radio does rather than what
 * the client believes: a report every MESH_UI_HISTORY_RADIO_REPORT_MS, plus the minute of slop
 * the firmware's once-a-minute tick adds to its own throttle. A gap that stops being a few
 * reports fails here rather than on somebody's Brick.
 */
MESH_TEST_CASE(history_draws_a_line_at_the_radios_own_cadence, unit) {
    /* The interval two reports actually land on: the throttle, tested a tick late. */
    const uint32_t cadence = MESH_UI_HISTORY_RADIO_REPORT_MS + 60U * 1000U;

    struct mesh_ui_history history;
    mesh_ui_history_reset(&history);
    for (uint32_t i = 0U; i < 4U; ++i) {
        mesh_ui_history_note_airtime(&history, 1000U + i * cadence, (int32_t)(60U + i * 10U),
                                     (int32_t)(20U + i * 5U));
    }
    MESH_TEST_FAIL_IF(!mesh_ui_history_has_airtime(&history),
                      "reports arriving on the radio's own schedule should draw a line");

    /* And the same claim stated directly, so the reason survives a rewrite of the loop above.
       Two cadences rather than one: a report is skipped as well as delayed - the firmware sends
       its stats only on the turns it is not broadcasting to the mesh - so a gap that merely
       outlasts one report still breaks on an ordinary pair of them. */
    MESH_TEST_FAIL_IF(MESH_UI_HISTORY_RADIO_GAP_MS <= 2U * MESH_UI_HISTORY_RADIO_REPORT_MS,
                      "the radio's gap must outlast a skipped report, not just a late one");
    MESH_TEST_FAIL_IF(MESH_UI_HISTORY_NODE_GAP_MS <= 2U * MESH_UI_HISTORY_NODE_REPORT_MS,
                      "a node's gap must outlast a skipped report, not just a late one");
    record_success(test_name);
}

/*
 * The airtime trend survives a restart, and the clock it comes back to is not the one it left.
 *
 * This is the case the whole persistence exists for and the one that is easy to get wrong in a
 * way no other test would see. A sample is stamped with CLOCK_MONOTONIC, which counts from
 * *boot*: a Brick that has been up for hours writes samples in the millions, and the next run -
 * a fresh boot, or simply an earlier point in a long uptime - pushes its first live reading at
 * a smaller number. mesh_ui_series_push() reads a time below the newest as the clock having
 * gone backwards and empties the series, so a restore that kept the saved stamps would undo
 * itself on the first report with nothing on the frame saying so.
 *
 * So the second session's clock here is deliberately *far below* the first's, which is what
 * makes this a regression test rather than a round trip.
 */
MESH_TEST_CASE(history_airtime_survives_a_restart_onto_a_new_clock, unit) {
    char path[] = "/tmp/mesh_ui_history_restartXXXXXX";
    const int fd = mkstemp(path);
    MESH_TEST_FAIL_IF(fd < 0, "could not make a cache path");
    close(fd);

    /* Session one, on a machine that has been up for a while. */
    struct mesh_ui_store first;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&first) != 0, "store init failed");
    const uint32_t old_clock = 9U * 60U * 60U * 1000U; /* nine hours of uptime */
    const uint32_t cadence = MESH_UI_HISTORY_RADIO_REPORT_MS + 60U * 1000U;
    for (uint32_t i = 0U; i < 4U; ++i) {
        mesh_ui_history_note_airtime(&first.history, old_clock + i * cadence,
                                     (int32_t)(50U + i * 10U), (int32_t)(10U + i * 5U));
    }
    MESH_TEST_FAIL_IF(!mesh_ui_history_has_airtime(&first.history), "the first session had a line");
    MESH_TEST_FAIL_IF(mesh_ui_store_save(&first, path) != 0, "save failed");

    /* Session two, on a clock that starts again from a cold boot. */
    struct mesh_ui_store second;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&second) != 0, "store init failed");
    MESH_TEST_FAIL_IF(mesh_ui_store_load(&second, path) != 0, "load failed");

    MESH_TEST_FAIL_IF(second.history.channel_utilization.count != 4U,
                      "every saved reading should come back");
    MESH_TEST_FAIL_IF(second.history.air_util_tx.count != 4U,
                      "the transmit share rides the same record");
    MESH_TEST_FAIL_IF(!mesh_ui_history_has_airtime(&second.history),
                      "a restored trend should be drawable before the radio says anything");

    /* The readings themselves, oldest first, in the order they were written. */
    for (uint32_t i = 0U; i < 4U; ++i) {
        const struct mesh_ui_sample *sample =
            mesh_ui_series_at(&second.history.channel_utilization, i);
        MESH_TEST_FAIL_IF(sample == NULL || sample->value != (int32_t)(50U + i * 10U),
                          "a restored reading should be the one that was saved");
    }

    /* And now the live clock, which is thirty seconds into this boot - far below every stamp
       the first session wrote. */
    const uint32_t new_clock = 30U * 1000U;
    mesh_ui_history_note_airtime(&second.history, new_clock, 90, 30);
    MESH_TEST_FAIL_IF(second.history.channel_utilization.count != 5U,
                      "a reading on a lower clock must not empty the restored series");
    MESH_TEST_FAIL_IF(!mesh_ui_series_starts_segment(&second.history.channel_utilization, 4U),
                      "the first reading after a restart starts a segment of its own");

    /* A second live reading continues it, so the new session draws a line of its own. */
    mesh_ui_history_note_airtime(&second.history, new_clock + 60U * 1000U, 95, 32);
    MESH_TEST_FAIL_IF(mesh_ui_series_starts_segment(&second.history.channel_utilization, 5U),
                      "a punctual reading after the seam continues the live segment");
    MESH_TEST_FAIL_IF(second.history.channel_utilization.count != 6U, "both live readings kept");

    remove(path);
    record_success(test_name);
}

/* A silence longer than the series' own gap breaks the line rather than sloping across it. */
MESH_TEST_CASE(series_breaks_the_line_at_a_gap, unit) {
    struct mesh_ui_series series;
    mesh_ui_series_reset(&series, 1000U);
    mesh_ui_series_push(&series, 0U, 10);
    mesh_ui_series_push(&series, 500U, 20);
    mesh_ui_series_push(&series, 9000U, 30); /* eight and a half seconds of nothing */
    mesh_ui_series_push(&series, 9500U, 40);

    struct mesh_ui_polyline points;
    mesh_ui_series_project(&series, (struct mesh_ui_scale){0, 100}, &points);
    MESH_TEST_FAIL_IF(points.items[1].gap, "a reading inside the window continues the line");
    MESH_TEST_FAIL_IF(!points.items[2].gap, "a reading after a silence should lift the pen");
    MESH_TEST_FAIL_IF(points.items[3].gap, "the line should resume after the break");
    record_success(test_name);
}

/*
 * A discontinuity the clock cannot see: a reading that was refused rather than missing.
 *
 * The elapsed-time test cannot catch it - the two real readings are punctual, well inside the
 * gap window - so the source has to say so, and mesh_ui_series_break() is how.
 */
MESH_TEST_CASE(series_breaks_where_the_source_says_so, unit) {
    struct mesh_ui_series series;
    mesh_ui_series_reset(&series, 10000U);
    mesh_ui_series_push(&series, 0U, 80);
    mesh_ui_series_break(&series);
    mesh_ui_series_push(&series, 1000U, 79);
    mesh_ui_series_push(&series, 2000U, 78);

    struct mesh_ui_polyline points;
    mesh_ui_series_project(&series, (struct mesh_ui_scale){0, 100}, &points);
    MESH_TEST_FAIL_IF(points.count != 3U, "a break should cost no reading");
    MESH_TEST_FAIL_IF(!points.items[1].gap,
                      "a reading after a stated break should start its own segment");
    MESH_TEST_FAIL_IF(points.items[2].gap, "the break should be spent on one reading, not held");
    record_success(test_name);
}

/* One reading is a level, and a widget handed one draws nothing. */
MESH_TEST_CASE(series_projection_of_one_reading_is_not_a_trend, unit) {
    struct mesh_ui_series series;
    mesh_ui_series_reset(&series, 0U);
    mesh_ui_series_push(&series, 1000U, 42);

    struct mesh_ui_polyline points;
    mesh_ui_series_project(&series, (struct mesh_ui_scale){0, 100}, &points);
    MESH_TEST_FAIL_IF(points.count != 1U, "the sample should still be projected");

    /* And an empty one is not a crash: a screen asks before the radio has said anything. */
    mesh_ui_series_reset(&series, 0U);
    mesh_ui_series_project(&series, (struct mesh_ui_scale){0, 100}, &points);
    MESH_TEST_FAIL_IF(points.count != 0U, "an empty series projects to nothing");
    record_success(test_name);
}

/*
 * Samples the clock could not separate fall back to even spacing, because their order is then
 * all that is known about them - the one case where the sample number is the axis.
 */
MESH_TEST_CASE(series_with_no_span_falls_back_to_even_spacing, unit) {
    struct mesh_ui_series series;
    mesh_ui_series_reset(&series, 0U);
    mesh_ui_series_push(&series, 7U, 0);
    mesh_ui_series_push(&series, 7U, 50);
    mesh_ui_series_push(&series, 7U, 100);

    struct mesh_ui_polyline points;
    mesh_ui_series_project(&series, (struct mesh_ui_scale){0, 100}, &points);
    MESH_TEST_FAIL_IF(points.items[0].x != 0 || points.items[1].x != MESH_UI_ANIM_ONE / 2 ||
                          points.items[2].x != MESH_UI_ANIM_ONE,
                      "samples the clock cannot separate should be spread evenly");
    record_success(test_name);
}

MESH_TEST_CASE(history_keeps_a_battery_per_node, unit) {
    struct mesh_ui_history history;
    mesh_ui_history_reset(&history);
    MESH_TEST_FAIL_IF(mesh_ui_history_series(&history, 0x1234U, MESH_UI_HISTORY_BATTERY) != NULL,
                      "a node nothing has been kept for has no trend");

    mesh_ui_history_note_battery(&history, 1000U, 0x1234U, 90U);
    mesh_ui_history_note_battery(&history, 2000U, 0x1234U, 88U);
    mesh_ui_history_note_battery(&history, 1500U, 0x5678U, 40U);

    const struct mesh_ui_series *series =
        mesh_ui_history_series(&history, 0x1234U, MESH_UI_HISTORY_BATTERY);
    MESH_TEST_FAIL_IF(series == NULL || series->count != 2U, "both readings should be kept");
    MESH_TEST_FAIL_IF(mesh_ui_series_newest(series)->value != 88,
                      "the newest reading should be the newest push");
    MESH_TEST_FAIL_IF(mesh_ui_history_series(&history, 0x5678U, MESH_UI_HISTORY_BATTERY) == NULL,
                      "a second node should get a slot of its own");

    /*
     * 101 is the firmware's "running off external power", which every phone app draws as a plug
     * rather than as a level. It is refused rather than drawn as a reading above full, and the
     * trend keeps what it had.
     */
    mesh_ui_history_note_battery(&history, 3000U, 0x1234U, 101U);
    MESH_TEST_FAIL_IF(mesh_ui_history_series(&history, 0x1234U, MESH_UI_HISTORY_BATTERY)->count !=
                          2U,
                      "a node on mains reports no battery level to plot");
    record_success(test_name);
}

/*
 * And an hour on external power breaks the line rather than vanishing from it.
 *
 * Refusing the sentinel is not enough on its own: a node on mains is still reporting punctually,
 * so the two real readings either side of it are inside the gap window and would be joined -
 * one continuous slope over a period where no battery level existed. This is the case the
 * elapsed-time rule cannot see.
 */
MESH_TEST_CASE(history_breaks_a_battery_trend_across_external_power, unit) {
    struct mesh_ui_history history;
    mesh_ui_history_reset(&history);

    mesh_ui_history_note_battery(&history, 1000U, 0x1234U, 80U);
    mesh_ui_history_note_battery(&history, 2000U, 0x1234U, 101U); /* plugged in */
    mesh_ui_history_note_battery(&history, 3000U, 0x1234U, 79U);

    const struct mesh_ui_series *series =
        mesh_ui_history_series(&history, 0x1234U, MESH_UI_HISTORY_BATTERY);
    MESH_TEST_FAIL_IF(series == NULL || series->count != 2U,
                      "only the two real levels should be readings");

    struct mesh_ui_polyline points;
    mesh_ui_series_project(series, (struct mesh_ui_scale){0, 100}, &points);
    MESH_TEST_FAIL_IF(!points.items[1].gap,
                      "the reading after the plug should not continue the one before it");

    /* A node nothing has been watching has no trend to discontinue, and recording that it is
       plugged in would spend a slot a node with readings could have used. */
    mesh_ui_history_note_battery(&history, 4000U, 0x9999U, 101U);
    MESH_TEST_FAIL_IF(mesh_ui_history_series(&history, 0x9999U, MESH_UI_HISTORY_BATTERY) != NULL,
                      "external power alone should not claim a slot");
    record_success(test_name);
}

/* More nodes than slots: the least recently heard from is what a new one costs. */
MESH_TEST_CASE(history_evicts_the_least_recently_heard_node, unit) {
    struct mesh_ui_history history;
    mesh_ui_history_reset(&history);

    for (uint32_t i = 0U; i < MESH_UI_HISTORY_NODES; ++i) {
        mesh_ui_history_note_battery(&history, 1000U + i, 0x100U + i, 50U);
    }
    /* The oldest slot is spoken for again, so it is no longer the one to go. */
    mesh_ui_history_note_battery(&history, 9000U, 0x100U, 49U);
    mesh_ui_history_note_battery(&history, 9100U, 0xBEEFU, 30U);

    MESH_TEST_FAIL_IF(mesh_ui_history_series(&history, 0xBEEFU, MESH_UI_HISTORY_BATTERY) == NULL,
                      "the arriving node should have taken a slot");
    MESH_TEST_FAIL_IF(mesh_ui_history_series(&history, 0x100U, MESH_UI_HISTORY_BATTERY) == NULL,
                      "a node heard from again should not be the one evicted");
    MESH_TEST_FAIL_IF(mesh_ui_history_series(&history, 0x101U, MESH_UI_HISTORY_BATTERY) != NULL,
                      "the least recently heard node should be the one that went");
    record_success(test_name);
}

/*
 * The store is where a reading becomes a sample, and the radio's own report is the trigger.
 *
 * Its stamp cannot be: `time` is our clock when it arrived and is 0 on a device with no wall
 * clock, so a series keyed on it would hold one sample forever. The report having changed is
 * the test, and a publish that changes something else entirely adds nothing.
 */
MESH_TEST_CASE(store_records_airtime_as_the_radio_reports_it, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init should succeed");

    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.stats.valid = true;
    settings.stats.channel_utilization = 10.0f;
    settings.stats.air_util_tx = 1.0f;

    mesh_ui_store_tick(&store, 1000U);
    mesh_ui_store_set_settings(&store, &settings);

    settings.stats.channel_utilization = 30.0f;
    settings.stats.num_packets_rx = 4U;
    mesh_ui_store_tick(&store, 2000U);
    mesh_ui_store_set_settings(&store, &settings);

    MESH_TEST_FAIL_IF(store.history.channel_utilization.count != 2U,
                      "each report the radio sends should be one sample");
    MESH_TEST_FAIL_IF(mesh_ui_series_newest(&store.history.channel_utilization)->value != 300,
                      "the reading should be kept in permille, as the meter reads it");
    MESH_TEST_FAIL_IF(mesh_ui_series_newest(&store.history.air_util_tx)->time !=
                          mesh_ui_series_newest(&store.history.channel_utilization)->time,
                      "one report is one moment, whichever of its figures is being read");

    /* A publish that changes something other than the report adds no reading. */
    settings.reboot_notices = 3U;
    mesh_ui_store_tick(&store, 3000U);
    mesh_ui_store_set_settings(&store, &settings);
    MESH_TEST_FAIL_IF(store.history.channel_utilization.count != 2U,
                      "a settings change that is not a report should not be a sample");

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    MESH_TEST_FAIL_IF(!mesh_ui_store_consume_updates(&store, &snapshot),
                      "the settings write should have marked the store dirty");
    MESH_TEST_FAIL_IF(snapshot.history.channel_utilization.count != 2U,
                      "the history should travel to the backends in the snapshot");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * A node's battery, the same way - and a different radio drops the lot.
 *
 * Node numbers belong to the mesh rather than to the radio, but a different radio is a
 * different mesh: a trend stitched across the swap would draw one node's battery falling into
 * another node's.
 */
/*
 * The two readings a node's air carries, each kept on its own and under one stamp.
 *
 * The independence is the whole of what this checks. EnvironmentMetrics is an optional-field
 * message, so a node with a thermometer and no hygrometer is ordinary - and a push that filled
 * the missing half with a zero would draw a flat line at freezing rather than no line at all.
 */
MESH_TEST_CASE(history_keeps_temperature_and_humidity_apart, unit) {
    struct mesh_ui_history history;
    mesh_ui_history_reset(&history);

    mesh_ui_history_note_environment(&history, 1000U, 0x1234U, true, 215, true, 470);
    mesh_ui_history_note_environment(&history, 2000U, 0x1234U, true, 208, false, 0);

    const struct mesh_ui_series *temp =
        mesh_ui_history_series(&history, 0x1234U, MESH_UI_HISTORY_TEMPERATURE);
    MESH_TEST_FAIL_IF(temp == NULL || temp->count != 2U,
                      "both temperature readings should be kept");
    MESH_TEST_FAIL_IF(mesh_ui_series_at(temp, 1U)->value != 208,
                      "the series holds tenths of a degree as pushed");
    const struct mesh_ui_series *wet =
        mesh_ui_history_series(&history, 0x1234U, MESH_UI_HISTORY_HUMIDITY);
    MESH_TEST_FAIL_IF(wet == NULL || wet->count != 1U,
                      "a report with no humidity is not a humidity reading");

    /* And a below-zero reading is a reading, which is the one way this differs from every
       percentage in the client: the guard that reads "not above zero" as "nothing was said"
       would erase every winter night on the mesh. */
    mesh_ui_history_note_environment(&history, 3000U, 0x1234U, true, -85, false, 0);
    MESH_TEST_FAIL_IF(mesh_ui_series_newest(temp)->value != -85, "a frost is a reading");

    /* The humidity catches up the moment the node reports one again, on its own count rather
       than on the three temperatures that went past it. */
    mesh_ui_history_note_environment(&history, 4000U, 0x1234U, true, 190, true, 455);
    MESH_TEST_FAIL_IF(wet->count != 2U, "humidity should hold only the reports that carried one");
    MESH_TEST_FAIL_IF(temp->count != 4U, "and the temperature should hold all four");

    /* A report carrying neither takes no slot: claiming one to remember that a node reports
       nothing is how the node somebody is watching gets evicted. */
    mesh_ui_history_note_environment(&history, 4000U, 0x9999U, false, 0, false, 0);
    MESH_TEST_FAIL_IF(mesh_ui_history_series(&history, 0x9999U, MESH_UI_HISTORY_TEMPERATURE) !=
                          NULL,
                      "an empty environment report should claim no slot");
    record_success(test_name);
}

/* One slot per node, carrying every reading - so a node evicted out of one takes all three of
   its trends with it rather than leaving a temperature under the arriving node's name. */
MESH_TEST_CASE(history_evicts_a_node_with_all_of_its_readings, unit) {
    struct mesh_ui_history history;
    mesh_ui_history_reset(&history);

    /* Two reports each, because one reading is a level rather than a trend and
       mesh_ui_history_series() answers NULL until there is a segment to draw between them. */
    for (uint32_t i = 0U; i < MESH_UI_HISTORY_NODES; ++i) {
        mesh_ui_history_note_environment(&history, 1000U + i, 0x100U + i, true, 200, true, 500);
        mesh_ui_history_note_environment(&history, 2000U + i, 0x100U + i, true, 210, true, 490);
    }
    /* The first slot is the least recently heard from, so a thirteenth node takes it. */
    mesh_ui_history_note_environment(&history, 9000U, 0xBEEFU, true, 300, false, 0);
    mesh_ui_history_note_environment(&history, 9100U, 0xBEEFU, true, 310, false, 0);

    MESH_TEST_FAIL_IF(mesh_ui_history_series(&history, 0x100U, MESH_UI_HISTORY_TEMPERATURE) != NULL,
                      "the evicted node should keep no temperature");
    MESH_TEST_FAIL_IF(mesh_ui_history_series(&history, 0x100U, MESH_UI_HISTORY_HUMIDITY) != NULL,
                      "nor a humidity the arriving node never reported");
    MESH_TEST_FAIL_IF(mesh_ui_history_series(&history, 0xBEEFU, MESH_UI_HISTORY_HUMIDITY) != NULL,
                      "a reading the arriving node did not report is not its predecessor's");
    MESH_TEST_FAIL_IF(mesh_ui_history_series(&history, 0xBEEFU, MESH_UI_HISTORY_TEMPERATURE) ==
                          NULL,
                      "the arriving node keeps what it did report");
    record_success(test_name);
}

/* A reading off the air becomes an integer exactly once, and a temperature's guard cannot be the
   percentage's: zero is the middle of this domain rather than the bottom of it. */
MESH_TEST_CASE(history_temperature_survives_the_wire, unit) {
    MESH_TEST_FAIL_IF(mesh_ui_temperature_decidegrees(21.4f) != 214, "tenths, rounded");
    MESH_TEST_FAIL_IF(mesh_ui_temperature_decidegrees(-8.46f) != -85,
                      "a negative rounds away from zero rather than toward it");
    MESH_TEST_FAIL_IF(mesh_ui_temperature_decidegrees(0.0f) != 0, "zero is a temperature");
    MESH_TEST_FAIL_IF(mesh_ui_temperature_decidegrees(999.0f) != MESH_UI_TEMPERATURE_CEILING,
                      "a fault clamps to the end of the scale");
    MESH_TEST_FAIL_IF(mesh_ui_temperature_decidegrees(-999.0f) != MESH_UI_TEMPERATURE_FLOOR,
                      "and so does one the other way");
    const float nan_reading = 0.0f / 0.0f;
    MESH_TEST_FAIL_IF(mesh_ui_temperature_decidegrees(nan_reading) != MESH_UI_TEMPERATURE_FLOOR,
                      "a NaN is not a reading and must not sail through the bounds");
    record_success(test_name);
}

/* The store's own push: a node's air is keyed on the environment struct having changed, and not
   on the battery report next to it - two Telemetry variants arriving on two schedules. */
MESH_TEST_CASE(store_records_node_environment_on_its_own_schedule, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init should succeed");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.roster_owner = 0xAAAAU;
    handshake.node_count = 1U;
    handshake.nodes[0].node_id = 0x4242U;
    handshake.nodes[0].environment.valid = true;
    handshake.nodes[0].environment.has_temperature = true;
    handshake.nodes[0].environment.temperature = 21.5f;

    mesh_ui_store_tick(&store, 1000U);
    mesh_ui_store_set_handshake(&store, &handshake);

    /* A battery report moving underneath is not a new temperature. */
    handshake.nodes[0].metrics.valid = true;
    handshake.nodes[0].metrics.has_battery = true;
    handshake.nodes[0].metrics.battery_level = 80U;
    mesh_ui_store_tick(&store, 2000U);
    mesh_ui_store_set_handshake(&store, &handshake);
    const struct mesh_ui_series *temp =
        mesh_ui_history_series(&store.history, 0x4242U, MESH_UI_HISTORY_TEMPERATURE);
    MESH_TEST_FAIL_IF(temp == NULL || temp->count != 1U,
                      "a device-metrics report is not an environment reading");

    handshake.nodes[0].environment.temperature = 22.1f;
    mesh_ui_store_tick(&store, 3000U);
    mesh_ui_store_set_handshake(&store, &handshake);
    MESH_TEST_FAIL_IF(temp->count != 2U, "a fresh environment report should be a reading");
    MESH_TEST_FAIL_IF(mesh_ui_series_newest(temp)->value != 221, "in tenths, as the row draws it");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

MESH_TEST_CASE(store_records_node_batteries_and_forgets_on_a_radio_swap, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init should succeed");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.roster_owner = 0xAAAAU;
    handshake.node_count = 1U;
    handshake.nodes[0].node_id = 0x4242U;
    handshake.nodes[0].metrics.valid = true;
    handshake.nodes[0].metrics.has_battery = true;
    handshake.nodes[0].metrics.battery_level = 80U;

    mesh_ui_store_tick(&store, 1000U);
    mesh_ui_store_set_handshake(&store, &handshake);

    /* A republish of the same roster is not a second reading. */
    handshake.nodes[0].last_heard = 42U;
    mesh_ui_store_tick(&store, 2000U);
    mesh_ui_store_set_handshake(&store, &handshake);
    MESH_TEST_FAIL_IF(
        mesh_ui_history_series(&store.history, 0x4242U, MESH_UI_HISTORY_BATTERY)->count != 1U,
        "a roster republish with no new telemetry is not a reading");

    handshake.nodes[0].metrics.battery_level = 76U;
    handshake.nodes[0].metrics.uptime_seconds = 900U;
    mesh_ui_store_tick(&store, 3000U);
    mesh_ui_store_set_handshake(&store, &handshake);
    MESH_TEST_FAIL_IF(
        mesh_ui_history_series(&store.history, 0x4242U, MESH_UI_HISTORY_BATTERY)->count != 2U,
        "a fresh telemetry report should be a reading");

    handshake.roster_owner = 0xBBBBU;
    mesh_ui_store_tick(&store, 4000U);
    mesh_ui_store_set_handshake(&store, &handshake);
    const struct mesh_ui_series *after =
        mesh_ui_history_series(&store.history, 0x4242U, MESH_UI_HISTORY_BATTERY);
    MESH_TEST_FAIL_IF(after == NULL || after->count != 1U,
                      "a radio swap should leave only what this radio has said");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}
