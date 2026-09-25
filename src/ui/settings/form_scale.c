/*
 * A number row's presets and a choice row's set - see mesh/ui/form_scale.h.
 */

#include "mesh/ui/form_scale.h"

#include "inkcell/ui/anim.h"

uint32_t mesh_ui_form_presets_step(const struct mesh_ui_form_presets *presets, uint32_t value,
                                   int delta) {
    if (presets == NULL || presets->values == NULL || delta == 0) {
        return value;
    }
    if (delta > 0) {
        for (size_t i = 0; i < presets->count; ++i) {
            if (presets->values[i] > value) {
                return presets->values[i];
            }
        }
        return value;
    }
    for (size_t i = presets->count; i > 0U; --i) {
        if (presets->values[i - 1U] < value) {
            return presets->values[i - 1U];
        }
    }
    return value;
}

/*
 * In *stop* space rather than in value space, and that is the arithmetic worth explaining. A
 * list of intervals climbs geometrically - 15s, 30s, a minute, two, five, ten, fifteen, half an
 * hour, an hour - so a handle placed at value/3600 would put eight of the ten choices inside the
 * first sixth of the track and leave the last two with the rest of it. What the reader is
 * choosing between is the *choices*, so the stops are evenly spaced and the position is the
 * index among them.
 *
 * A value the list does not contain still gets a position, interpolated between the two stops it
 * falls between, and that is one thing a track can do that a set of alternatives cannot: a set
 * has no room between its members, so an unknown value there has to fall back to words. An axis
 * has room. 42 seconds, written by some other program with a different list, lands where 42
 * seconds actually is - a true statement about a number this list would not itself have offered.
 *
 * The unsigned comparisons are the ones presets_step() walks with, and they are correct over an
 * all-negative list for the reason the header gives: two's complement keeps their order, and an
 * unsigned difference of two of them is the true distance between them.
 */
bool mesh_ui_form_presets_track(const struct mesh_ui_form_presets *presets, uint32_t value,
                                struct mesh_ui_form_track *out) {
    if (presets == NULL || !presets->scale || presets->values == NULL) {
        return false;
    }
    const size_t aside = presets->zero_aside ? 1U : 0U;
    if (presets->count < aside + 2U) {
        return false;
    }
    const uint32_t *stops = presets->values + aside;
    const size_t last = presets->count - aside - 1U;

    struct mesh_ui_form_track track = {.stops = (uint32_t)(last + 1U)};
    if (value < stops[0]) {
        /*
         * Below the bottom stop is off the track, on every scale rather than only on the ones
         * that stand a zero aside.
         *
         * A list can start above zero because the thing on the other end refuses anything below
         * it, while something that has never been configured reports 0. Placing that at the first
         * stop would draw "off" exactly as the shortest setting, which is the same false claim a
         * leading "as much as there is" makes when it is drawn at the empty end of its own bar.
         */
        track.unplaced = true;
    } else if (value >= stops[last]) {
        track.position = INKCELL_ANIM_ONE;
    } else if (value > stops[0]) {
        size_t i = 0U;
        while (i < last && stops[i + 1U] <= value) {
            ++i;
        }
        /* The stop itself, plus however far past it the value has got towards the next one. The
           span cannot be zero - a preset list is strictly increasing - but a list edited into
           holding a repeat would divide by it, and the stop is the honest answer for that. */
        const uint32_t span = stops[i + 1U] - stops[i];
        const uint64_t within =
            span > 0U ? ((uint64_t)(value - stops[i]) * INKCELL_ANIM_ONE) / span : 0U;
        track.position = (int32_t)(((uint64_t)i * INKCELL_ANIM_ONE + within) / last);
    }
    if (out != NULL) {
        *out = track;
    }
    return true;
}

bool mesh_ui_form_choice_allowed(uint32_t choices, uint32_t count, uint32_t value) {
    if (value >= count) {
        return false;
    }
    /* No mask is every value, not no value - see the header. And a value at or past the width
       of the word has no bit to test, so it is outside any set that has one. */
    if (choices == 0U) {
        return true;
    }
    return value < 32U && (choices & (1U << value)) != 0U;
}

uint32_t mesh_ui_form_choice_step(uint32_t choices, uint32_t count, uint32_t current, int delta) {
    if (count == 0U) {
        return current;
    }
    /* Start from somewhere inside the range even when `current` is not: the other end may be
       holding a value this build does not have, and a press on that row has to land on one that
       exists rather than walk off the end of the word. */
    uint32_t value = current < count ? current : 0U;
    const uint32_t forward = delta < 0 ? count - 1U : 1U;
    for (uint32_t step = 0; step < count; ++step) {
        value = (value + forward) % count;
        if (mesh_ui_form_choice_allowed(choices, count, value)) {
            return value;
        }
    }
    /* A full lap with nothing legal on it. One value in the set, or none. */
    return current;
}
