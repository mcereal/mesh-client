/*
 * The frame a chart draws inside: see include/mesh/ui/trend.h for why it is one file rather than
 * two copies of the same arithmetic in two renderers.
 */

#include "mesh/ui/trend.h"

#include "mesh/ui/anim.h"

#include <stddef.h>

/* ---- how far back --------------------------------------------------------------------------- */

/*
 * The spans, in the order the strip draws them.
 *
 * A table rather than a switch for the reason every other table in this layer is one: three
 * things read it - the strip's labels, the window's cut and the step that walks it - and a switch
 * written out three times is three opinions about how many spans there are.
 */
static const struct {
    uint32_t ms; /* 0: however long the readings are */
    enum mesh_str_id label;
} k_spans[MESH_UI_TREND_SPAN_COUNT] = {
    [MESH_UI_TREND_SPAN_15M] = {15U * 60U * 1000U, MESH_STR_TREND_SPAN_15M},
    [MESH_UI_TREND_SPAN_1H] = {60U * 60U * 1000U, MESH_STR_TREND_SPAN_1H},
    [MESH_UI_TREND_SPAN_6H] = {6U * 60U * 60U * 1000U, MESH_STR_TREND_SPAN_6H},
    [MESH_UI_TREND_SPAN_ALL] = {0U, MESH_STR_TREND_SPAN_ALL},
};

/* A span outside the set is ALL rather than refused: it is a byte off the nav, the nav is clamped
   against a set that can change, and the widest span is the one that cannot mislead - it shows
   every reading there is. */
static uint8_t span_or_all(uint8_t span) {
    return span < (uint8_t)MESH_UI_TREND_SPAN_COUNT ? span : (uint8_t)MESH_UI_TREND_SPAN_ALL;
}

uint32_t mesh_ui_trend_span_ms(uint8_t span) { return k_spans[span_or_all(span)].ms; }

enum mesh_str_id mesh_ui_trend_span_label(uint8_t span) { return k_spans[span_or_all(span)].label; }

uint8_t mesh_ui_trend_span_step(uint8_t span, int delta) {
    const int count = (int)MESH_UI_TREND_SPAN_COUNT;
    int next = (int)span_or_all(span) + delta;
    /* Wrapped both ways with the remainder made positive first, because C's % keeps the sign of
       the left operand and a Left press off the first segment would otherwise land outside the
       set - the one place this could hand the strip an `active` it declines to draw. */
    next = ((next % count) + count) % count;
    return (uint8_t)next;
}

/* ---- how far up ----------------------------------------------------------------------------- */

/*
 * The rungs the ceiling may contract to, as divisors of the domain's own span, widest last.
 *
 * The 1-2-5 ladder, which is what an axis has always been ruled in: a hundredth, a fiftieth, a
 * twentieth, a tenth, a quarter, a half, all of it. On the airtime domain those are 1%, 2%, 5%,
 * 10%, 25%, 50% and 100% of the air, which are the numbers somebody reading a mesh would have
 * picked by hand - and on a battery they are 1, 2, 5, 10, 25, 50 and 100 percent, which are the
 * same numbers because the domain is the same width.
 *
 * Walked from the narrowest, so what is chosen is the smallest ceiling the readings clear.
 */
static const int32_t k_rungs[] = {100, 50, 20, 10, 4, 2, 1};

struct mesh_ui_scale mesh_ui_trend_domain(struct mesh_ui_scale domain, int32_t high) {
    /* The identity domain said out loud: the caller's readings are already permille, so the ends
       it is not stating are 0 and 1000. Contracting needs real ends to divide. */
    const int32_t min = domain.min;
    const int32_t max = domain.min == domain.max ? MESH_UI_ANIM_ONE : domain.max;
    if (max <= min) {
        /* Descending, or a domain with no width. Neither has a ceiling to contract: the first
           reads backwards and its top *is* its floor, and the second is a caller's mistake this
           is not the place to correct. */
        return domain;
    }
    const int64_t span = (int64_t)max - (int64_t)min;
    for (size_t i = 0U; i < sizeof k_rungs / sizeof k_rungs[0]; ++i) {
        const int64_t ceiling = (int64_t)min + span / k_rungs[i];
        if (ceiling <= (int64_t)min) {
            continue; /* a domain too narrow for this rung: integer division has closed it up */
        }
        if ((int64_t)high > ceiling) {
            continue; /* the readings do not fit under it */
        }
        if (k_rungs[i] == 1) {
            break; /* the whole domain, which is the one the caller already has */
        }
        return (struct mesh_ui_scale){min, (int32_t)ceiling};
    }
    return domain;
}

/* ---- the frame ------------------------------------------------------------------------------ */

/*
 * The largest reading inside the window that the picture will actually draw.
 *
 * Two qualifications on "largest", and each of them is a way an axis gets held open over a plot
 * with nothing in it.
 *
 * *Inside the window*, because the ceiling is picked for what is drawn: a busy spell that has
 * just scrolled off the left-hand edge would otherwise go on holding the axis open, which is the
 * complaint this whole file answers arriving through the span picker instead of through the
 * domain.
 *
 * *That the picture will draw*, because a stroke needs two ends. A reading that starts a segment
 * and has nothing continuing it - the first report after a silence longer than the series'
 * `gap_ms`, or after a mesh_ui_series_break() - is emitted by the projection and drawn by
 * nothing. A node that comes back from an outage with one reading of 80C and then goes quiet
 * again would take the axis to 80 and flatten the afternoon of real readings underneath it: the
 * same flattening, from the same cause, that contracting the ceiling exists to undo.
 *
 * The two ends of a drawn segment both count, which is what the pair of tests says: a reading
 * that continues the one before it is on a line, and so is one the next reading continues.
 */
static bool series_high(const struct mesh_ui_series *const *series, uint32_t count, uint32_t from,
                        uint32_t to, int32_t *out) {
    bool any = false;
    int32_t high = 0;
    for (uint32_t i = 0U; i < count; ++i) {
        const struct mesh_ui_series *one = series[i];
        if (one == NULL) {
            continue;
        }
        uint32_t placed = 0U; /* how many of this series the window has kept so far */
        for (uint32_t j = 0U; j < one->count; ++j) {
            const struct mesh_ui_sample *sample = mesh_ui_series_at(one, j);
            if (sample == NULL || sample->time < from || sample->time > to) {
                continue;
            }
            const uint32_t here = placed++;
            /*
             * The first reading the window kept always starts a segment, whatever the ring says
             * about it - that is mesh_ui_series_project_within()'s own rule, and asking the raw
             * break test here instead would count a reading as drawn because of a predecessor the
             * clip has already thrown away.
             */
            const bool starts = here == 0U || mesh_ui_series_starts_segment(one, j);
            bool drawn = !starts;
            if (!drawn) {
                const struct mesh_ui_sample *next = mesh_ui_series_at(one, j + 1U);
                drawn =
                    next != NULL && next->time <= to && !mesh_ui_series_starts_segment(one, j + 1U);
            }
            if (!drawn) {
                continue;
            }
            if (!any || sample->value > high) {
                high = sample->value;
            }
            any = true;
        }
    }
    if (any && out != NULL) {
        *out = high;
    }
    return any;
}

bool mesh_ui_trend_frame(const struct mesh_ui_series *const *series, uint32_t count,
                         struct mesh_ui_scale domain, uint8_t span, struct mesh_ui_trend *out) {
    uint32_t from = 0U;
    uint32_t to = 0U;
    if (!mesh_ui_series_window(series, count, &from, &to)) {
        return false;
    }

    /*
     * The cut, anchored at the newest reading rather than at the clock.
     *
     * Anchoring at "now" is the obvious reading of "the last fifteen minutes" and it is the wrong
     * one here: a link that dropped twenty minutes ago would answer every span but ALL with an
     * empty plot, and the reader pressing Left and Right along a row of empty pictures has been
     * told nothing about a radio that was reporting perfectly well until it went away. What a
     * chart is looking at is *readings*, so the span is how much of them - and how long ago the
     * last one was is a question the Status card underneath already answers.
     *
     * It only ever narrows. A span wider than the readings leaves the window at the readings'
     * own, so the caption under the axis names what is drawn rather than what was asked for.
     */
    const uint32_t ms = mesh_ui_trend_span_ms(span);
    if (ms > 0U && (to - from) > ms) {
        from = to - ms;
    }

    int32_t high = 0;
    struct mesh_ui_scale scale = domain;
    if (series_high(series, count, from, to, &high)) {
        scale = mesh_ui_trend_domain(domain, high);
    }

    if (out != NULL) {
        out->from = from;
        out->to = to;
        out->scale = scale;
    }
    return true;
}
