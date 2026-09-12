#ifndef MESH_UI_TREND_H
#define MESH_UI_TREND_H

/*
 * What a chart is looking at: how far back, and how far up.
 *
 * The two chart screens - the radio's airtime and one of a node's readings - were each working
 * this out for themselves, in code that was the same arithmetic with different words around it.
 * Both asked mesh_ui_series_window() for a window, both projected against a domain, both formatted
 * a span. That is the *frame* rather than the picture, it is the same question on both screens,
 * and this is the one answer. The renderers keep what actually differs: what the lines are, what
 * they are called and what the ends of the vertical are worded as.
 *
 * Two things it decides, and both of them are decisions the panel had got wrong.
 *
 *   - **How far back.** A chart used to draw every reading the ring held, which is a span the
 *     reader could read off the caption and not change. On the radio's own report - minutes
 *     apart - that is an hour or two, and the last ten minutes of a mesh that has just gone busy
 *     are a fifth of the plot. `enum mesh_ui_trend_span` is the reader's answer to that: the
 *     newest reading anchors the right-hand edge and the span says how much of what came before
 *     it is on the picture.
 *   - **How far up.** This is the harder one, and the rule it lands on is deliberately not the
 *     spreadsheet's. See mesh_ui_trend_domain().
 *
 * Nothing here has a pixel in it, for the reason mesh_ui_series_project() does not: what a
 * backend needs from a chart is arithmetic, and the fb backend must not be the only thing that
 * can ask. Nothing here holds state either - a span is on the nav, where every other thing a
 * press moves is.
 */

#include "mesh/i18n/strings.h"
#include "mesh/ui/layout.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How far back a chart looks.
 *
 * Four, because FB_SEGMENTED_MAX is four and a segmented button is what draws it: above four the
 * words stop fitting the strip, and the set that cannot be read at a glance is not a set the
 * d-pad should be stepping either.
 *
 * The three fixed ones bracket the intervals the readings actually arrive on. LocalStats is a few
 * minutes, so a quarter of an hour is "what just happened" and an hour is "this session"; a node's
 * telemetry is half an hour by default, so six is the one that shows an afternoon of it. ALL is
 * last rather than first because it is the widest, and a strip whose spans do not run in order is
 * a control the reader has to read rather than aim at.
 *
 * ALL is also the default, and that matters: it is what the screen did before there was a picker,
 * so a reader who never touches Left or Right sees what they always saw.
 */
enum mesh_ui_trend_span {
    MESH_UI_TREND_SPAN_15M = 0,
    MESH_UI_TREND_SPAN_1H,
    MESH_UI_TREND_SPAN_6H,
    MESH_UI_TREND_SPAN_ALL,
    MESH_UI_TREND_SPAN_COUNT
};

/* How long the span is, in milliseconds - 0 for ALL, which is "however long the readings are". */
uint32_t mesh_ui_trend_span_ms(uint8_t span);

/* What the strip calls it. A word rather than a formatted duration: these four are fixed, and
   mesh_ui_format_duration() answers about a measurement rather than about a choice. */
enum mesh_str_id mesh_ui_trend_span_label(uint8_t span);

/*
 * The next span along, `delta` steps from this one, wrapping at both ends.
 *
 * Wrapping rather than clamping, because the strip is four segments the d-pad walks and a press
 * that does nothing at the end of a set of four is a press the action bar is still naming. It is
 * the enum row's rule (`(value + 1) % count`), and it is here rather than in nav.c so that the
 * chart's Left and Right cannot disagree about the order the segments draw in.
 */
uint8_t mesh_ui_trend_span_step(uint8_t span, int delta);

/*
 * The vertical, with its ceiling contracted to the highest rung the readings clear.
 *
 * This is the one place this client bends the rule stated in layout.h - that a trend's y axis is
 * the reading's own domain and never the range its samples span - and it bends it in exactly one
 * direction, for a reason that was measured in a field: a mesh at 1.1% busy drawn on a domain of
 * 0-100% is a flat line along the bottom of an empty rectangle. There is nothing wrong with the
 * number and nothing to see in the picture.
 *
 * What auto-scaling gets wrong is not that it moves the ceiling. It is that it moves *both* ends
 * to the data, so the shape is normalised away: a battery that fell two percent overnight fills
 * the plot corner to corner and reads as a cliff, and two visits to one screen cannot be compared
 * because neither axis stayed still. So:
 *
 *   - **The floor never moves.** It is the domain's own, so a reading near the bottom is drawn
 *     near the bottom and a fall of two percent is two percent of something.
 *   - **The ceiling moves only to a rung**, and the rungs are fractions of the domain rather than
 *     of the data: a hundredth, a fiftieth, a twentieth, a tenth, a quarter, a half, all of it.
 *     Which is the 1-2-5 ladder every axis has used since graph paper, expressed in the one thing
 *     that is fixed here. Two visits an hour apart are on the same rung unless the readings
 *     genuinely crossed one, and a rung is a number the reader can hold on to.
 *   - **The rung is written on the axis.** The top label is the ceiling, so a chart that has
 *     contracted says so in the one place a reader looks to find out what "high" is. That is what
 *     makes this honest where auto-scaling is not: the lie is never the scale, it is a scale the
 *     picture does not state.
 *
 * `high` is the largest reading the picture will draw, which is narrower than "the largest" in
 * two ways: it is inside the window, because a ceiling picked from readings that scrolled off is
 * a plot with empty air at the top of it; and it is on a line, because a reading nothing draws
 * must not move an axis. Both are mesh_ui_trend_frame()'s to establish.
 *
 * The domain comes back unchanged in the three cases where contracting it would be a guess: a
 * descending domain (min > max, which reads backwards and whose "ceiling" is its floor), a `high`
 * at or above the domain's own ceiling, and a span too narrow for the ladder's own arithmetic. A
 * zeroed domain is the identity one - the caller's readings are already permille - and it
 * contracts against 1000 and comes back as real ends, which is the same domain said out loud.
 */
struct mesh_ui_scale mesh_ui_trend_domain(struct mesh_ui_scale domain, int32_t high);

/*
 * Everything a chart needs to place its lines: the window along the bottom, and the domain up the
 * side.
 *
 * One call rather than four, and it is what makes the two chart screens one screen drawn twice.
 * The order matters and is the whole of why this is a function: the window is cut *first* and the
 * ceiling is picked from what is left inside it, so a span of fifteen minutes over an hour of
 * readings is scaled to the quarter hour the reader asked to see rather than to the busy spell
 * that has just left the picture.
 *
 * False when there is nothing to frame - no series, none of them with two readings, or every
 * stamp alike - which is mesh_ui_series_window()'s answer and is passed straight through. A caller
 * that gets false has a picture with no axis to lay along it and should say so rather than draw a
 * frame around nothing.
 */
struct mesh_ui_trend {
    uint32_t from; /* the client's monotonic clock at the left-hand edge */
    uint32_t to;   /* and at the right-hand edge, which is the newest reading */
    struct mesh_ui_scale scale;
};

bool mesh_ui_trend_frame(const struct mesh_ui_series *const *series, uint32_t count,
                         struct mesh_ui_scale domain, uint8_t span, struct mesh_ui_trend *out);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_TREND_H */
