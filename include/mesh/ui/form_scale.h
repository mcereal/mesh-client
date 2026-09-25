#ifndef MESH_UI_FORM_SCALE_H
#define MESH_UI_FORM_SCALE_H

/*
 * How a form row walks its values: a number through a list of presets, and a choice through a
 * set.
 *
 * A number row is edited by stepping, not typing - Left and Right go to the next preset below or
 * above - and a row whose presets *measure* something can also be drawn as a track the reader
 * aims at. A choice row wraps around a range of values, some of which a bitmask may rule out.
 * Both walks are here, apart from any field that uses them: a row hands in its presets or its
 * mask and gets its answer.
 *
 * Nothing here allocates, and the lists are the caller's, usually `static const`.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The values a number row steps through.
 *
 * `values` is strictly increasing *as uint32_t*. That is the only order this walks in, and it
 * keeps a list of all-negative values in order too: two's-complement negatives cast to uint32_t
 * all land in the top half of the range with their order intact (-100 is 0xFFFFFF9C, below -80's
 * 0xFFFFFFB0), so a threshold list in negative decibels steps correctly without anything here
 * knowing it is signed. A list that mixes signs does not, and says so by stepping wrong.
 */
struct mesh_ui_form_presets {
    const uint32_t *values;
    size_t count;
    /*
     * Whether the presets *measure* something or *name* something. The difference is not in the
     * numbers and cannot be derived from them - {0, 1, ... 7} is a count in one row and a pin
     * number in another, and a length drawn across the second says a pin is two thirds of the
     * way to being a pin. Only a scale is offered as a track.
     */
    bool scale;
    /*
     * Whether the first preset is a *word* standing outside the scale - a 0 that reads "whatever
     * the other end picks", or "as much as there is". Neither is a quantity, and drawn at the
     * bottom of a track "as much as there is" sits hard left, which is not merely unhelpful but
     * backwards. The track spans the presets after it, and a value of 0 is off the track.
     */
    bool zero_aside;
};

/* The next preset above (delta > 0) or below (delta < 0) `value`, or `value` itself at either
   end, with no presets, or with a delta of 0. A value between two presets steps to the nearer
   one in the direction asked. */
uint32_t mesh_ui_form_presets_step(const struct mesh_ui_form_presets *presets, uint32_t value,
                                   int delta);

/*
 * Where a value sits on a track, for a row that draws one.
 *
 * `position` is on the same 0..1000 a meter's fill is on (inkcell's INKCELL_ANIM_ONE), and
 * `stops` is how many choices there are to mark. `unplaced` is true for a value the track has no
 * room for; `position` is then 0 and `stops` still counts the marks, so a caller can lay the
 * control out without testing first.
 */
struct mesh_ui_form_track {
    int32_t position;
    uint32_t stops;
    bool unplaced;
};

/*
 * Fills `out` for presets that are a scale of at least two stops, and returns false for any
 * others - including presets that *name* something, because a length drawn across those is a
 * claim about magnitude the numbers do not make.
 *
 * The stops are evenly spaced and a value between two of them is interpolated, so a list that
 * climbs geometrically is a track the reader can aim at rather than eight choices crowded into
 * its first sixth. `unplaced` comes back true for a value below the bottom stop: a leading word
 * stood aside, or a list that starts above zero because the other end refuses anything smaller,
 * while something nobody configured still reports 0. Either way the value is not at the bottom
 * of the scale; it is not on the scale at all.
 *
 * One function answers position, count and placement together: a control measured twice is a
 * control that disagrees with itself.
 */
bool mesh_ui_form_presets_track(const struct mesh_ui_form_presets *presets, uint32_t value,
                                struct mesh_ui_form_track *out);

/*
 * The two halves of a row whose values are a set: is this one in it, and what is the next one.
 *
 * `choices` is a bitmask over 0..count-1 and 0 means unconstrained, so a caller with nothing to
 * say passes 0 and gets the plain wrap-around. Values from `count` up are never in the set,
 * whatever the mask says, which is what keeps a stale mask from offering a value the row no
 * longer has.
 *
 * step() walks in `delta`'s direction until it finds a value in the set, and answers `current`
 * when there is no other - a row with one legal value is a row Left and Right do nothing to,
 * which is the truth rather than a press that silently lands where it started. A `current`
 * outside the range starts the walk from 0: the other end may be holding a value this build
 * does not know, and a press on that row has to land on one that exists.
 */
bool mesh_ui_form_choice_allowed(uint32_t choices, uint32_t count, uint32_t value);
uint32_t mesh_ui_form_choice_step(uint32_t choices, uint32_t count, uint32_t current, int delta);

#ifdef __cplusplus
}
#endif

#endif
