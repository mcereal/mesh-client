/*
 * The frame a chart draws inside: see include/mesh/ui/trend.h for why it is one file rather than
 * two copies of the same arithmetic in two renderers.
 */

#include "mesh/ui/trend.h"

#include "mesh/ui/anim.h"

#include <stddef.h>
#include <string.h>

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

/*
 * The window one series' readings are listed over: mesh_ui_trend_frame()'s cut, without the
 * domain - a list has no vertical to contract.
 *
 * A single reading is a window of no width, which mesh_ui_series_window() declines and this
 * accepts. That is the one place the list and the plot part company, and it parts in the
 * direction that costs nothing: a picture with one point is a frame around nothing, and a table
 * with one row is a reading somebody took.
 */
static bool trend_reading_window(const struct mesh_ui_series *series, uint8_t span,
                                 uint32_t *out_from, uint32_t *out_to) {
    if (series == NULL || series->count == 0U) {
        return false;
    }
    const struct mesh_ui_series *one[1] = {series};
    uint32_t from = 0U;
    uint32_t to = 0U;
    if (!mesh_ui_series_window(one, 1U, &from, &to)) {
        const struct mesh_ui_sample *only = mesh_ui_series_newest(series);
        if (only == NULL) {
            return false;
        }
        from = only->time;
        to = only->time;
    }
    const uint32_t ms = mesh_ui_trend_span_ms(span);
    if (ms > 0U && (to - from) > ms) {
        from = to - ms;
    }
    *out_from = from;
    *out_to = to;
    return true;
}

uint32_t mesh_ui_trend_readings(const struct mesh_ui_series *series, uint8_t span) {
    uint32_t from = 0U;
    uint32_t to = 0U;
    if (!trend_reading_window(series, span, &from, &to)) {
        return 0U;
    }
    uint32_t count = 0U;
    for (uint32_t i = 0U; i < series->count; ++i) {
        const struct mesh_ui_sample *sample = mesh_ui_series_at(series, i);
        if (sample != NULL && sample->time >= from && sample->time <= to) {
            ++count;
        }
    }
    return count;
}

bool mesh_ui_trend_reading_at(const struct mesh_ui_series *series, uint8_t span, uint32_t index,
                              struct mesh_ui_trend_reading *out) {
    uint32_t from = 0U;
    uint32_t to = 0U;
    if (out == NULL || !trend_reading_window(series, span, &from, &to)) {
        return false;
    }
    /* Walked from the newest end, because that is how the rows are numbered - see the header. */
    uint32_t seen = 0U;
    for (uint32_t i = series->count; i > 0U; --i) {
        const struct mesh_ui_sample *sample = mesh_ui_series_at(series, i - 1U);
        if (sample == NULL || sample->time < from || sample->time > to) {
            continue;
        }
        if (seen++ != index) {
            continue;
        }
        out->time = sample->time;
        out->before_ms = to - sample->time;
        out->value = sample->value;
        return true;
    }
    return false;
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

/* ---- columns -------------------------------------------------------------------------------- */

/* The durations a bin may be, shortest first. Down to a second only because the capture harness
   and the tests stamp readings that close together; a radio never reports that fast. */
static const uint32_t k_bin_ladder[] = {
    1000U,   2000U,   5000U,    10000U,   15000U,   30000U,    60000U,    120000U,   300000U,
    600000U, 900000U, 1800000U, 3600000U, 7200000U, 10800000U, 21600000U, 43200000U, 86400000U,
};

uint32_t mesh_ui_trend_bin_ms(uint32_t window_ms, uint32_t cadence_ms, uint32_t max_bins) {
    const uint32_t bins = max_bins < 2U ? 2U : max_bins;
    /* `bins - 1` rather than `bins`: centred bins take half of one at each end, so a window of
       exactly (bins - 1) bin widths is the most that fits. */
    const uint32_t by_width = (window_ms + (bins - 2U)) / (bins - 1U);
    const uint32_t by_cadence = cadence_ms - cadence_ms / 8U;
    const uint32_t floor = by_width > by_cadence ? by_width : by_cadence;
    const size_t rungs = sizeof k_bin_ladder / sizeof k_bin_ladder[0];
    for (size_t i = 0U; i < rungs; ++i) {
        if (k_bin_ladder[i] >= floor) {
            return k_bin_ladder[i];
        }
    }
    return k_bin_ladder[rungs - 1U];
}

/* The median spacing of the readings inside [from, to], or 0 with fewer than two. An insertion
   sort over at most the ring's worth of intervals, which is a few hundred and runs once a frame
   the chart is open. */
static uint32_t airtime_cadence(const struct mesh_ui_history *history, uint32_t from, uint32_t to) {
    uint32_t intervals[MESH_UI_HISTORY_AIRTIME_MAX];
    uint32_t count = 0U;
    uint32_t previous = 0U;
    bool have_previous = false;
    const uint32_t total = mesh_ui_history_airtime_count(history);
    for (uint32_t i = 0U; i < total; ++i) {
        const struct mesh_ui_airtime_sample *sample = mesh_ui_history_airtime_at(history, i);
        if (sample->time < from || sample->time > to) {
            continue;
        }
        if (have_previous && sample->time > previous && !sample->gap) {
            const uint32_t interval = sample->time - previous;
            uint32_t j = count++;
            while (j > 0U && intervals[j - 1U] > interval) {
                intervals[j] = intervals[j - 1U];
                --j;
            }
            intervals[j] = interval;
        }
        previous = sample->time;
        have_previous = true;
    }
    return count > 0U ? intervals[count / 2U] : 0U;
}

static void bins_finish(struct mesh_ui_trend_bins *bins, const int64_t *sums,
                        const uint32_t *counts, const bool *breaks) {
    uint32_t last_present = 0U;
    bool any = false;
    for (uint32_t i = 0U; i < bins->count; ++i) {
        bins->present[i] = counts[i] > 0U;
        if (!bins->present[i]) {
            continue;
        }
        const int64_t n = (int64_t)counts[i];
        bins->values[i] = (int32_t)((sums[i] + n / 2) / n);
        bins->joins[i] = any && (i - last_present) <= 2U && !breaks[i];
        last_present = i;
        any = true;
    }
}

bool mesh_ui_trend_airtime(const struct mesh_ui_history *history, uint8_t span, uint32_t max_bins,
                           struct mesh_ui_trend_airtime *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (!mesh_ui_history_has_airtime(history)) {
        return false;
    }

    uint32_t to = mesh_ui_history_airtime_newest(history)->time;
    uint32_t from = mesh_ui_history_airtime_at(history, 0U)->time;
    /* The span's cut, as mesh_ui_trend_frame() makes it: anchored on the newest reading and only
       ever narrowing. */
    const uint32_t ms = mesh_ui_trend_span_ms(span);
    if (ms > 0U && (to - from) > ms) {
        from = to - ms;
    }

    uint32_t cap = max_bins > MESH_UI_TREND_BINS_MAX ? MESH_UI_TREND_BINS_MAX : max_bins;
    cap = cap < 2U ? 2U : cap;
    const uint32_t cadence = airtime_cadence(history, from, to);
    const uint32_t bin = mesh_ui_trend_bin_ms(to - from, cadence, cap);
    /*
     * How long a silence has to be before the line lifts over it: three of the readings' own
     * spacing, and never less than the radio's gap.
     *
     * Measured between readings rather than between bins, because a bin wide enough for six hours
     * is wide enough to swallow an outage - two readings either side of ten silent minutes land in
     * adjacent five-minute bins, and bins_finish() alone would join them. Relative to the cadence
     * rather than MESH_UI_HISTORY_RADIO_GAP_MS alone, because a radio that only sends LocalStats
     * reports every fifteen minutes, and a three-minute rule would break it at every reading.
     */
    const uint32_t silence =
        cadence > MESH_UI_HISTORY_RADIO_GAP_MS / 3U ? cadence * 3U : MESH_UI_HISTORY_RADIO_GAP_MS;
    /* At most `cap` by the ladder's own arithmetic, except off the top of the ladder - where the
       oldest bins are what give. */
    uint32_t count = (to - from + bin / 2U) / bin + 1U;
    count = count > cap ? cap : count;

    int64_t util_sums[MESH_UI_TREND_BINS_MAX];
    int64_t tx_sums[MESH_UI_TREND_BINS_MAX];
    uint32_t counts[MESH_UI_TREND_BINS_MAX];
    bool breaks[MESH_UI_TREND_BINS_MAX];
    memset(util_sums, 0, sizeof util_sums);
    memset(tx_sums, 0, sizeof tx_sums);
    memset(counts, 0, sizeof counts);
    memset(breaks, 0, sizeof breaks);

    const uint32_t total = mesh_ui_history_airtime_count(history);
    uint32_t previous = 0U;
    bool have_previous = false;
    for (uint32_t i = 0U; i < total; ++i) {
        const struct mesh_ui_airtime_sample *sample = mesh_ui_history_airtime_at(history, i);
        if (sample->time > to) {
            continue;
        }
        /* Bins back from the newest, rounded to the nearest: bin 0 is centred on `to`. */
        const uint32_t back = (to - sample->time + bin / 2U) / bin;
        if (sample->time < from || back >= count) {
            continue;
        }
        const uint32_t slot = count - 1U - back;
        const bool silent = have_previous && sample->time - previous > silence;
        previous = sample->time;
        have_previous = true;
        /* A break on the first reading into a bin - marked by the source, or a silence before
           it - is a break before the bin. One inside a bin has nowhere to be drawn. */
        if (counts[slot] == 0U && (sample->gap || silent)) {
            breaks[slot] = true;
        }
        util_sums[slot] += sample->utilization;
        tx_sums[slot] += sample->tx;
        ++counts[slot];
    }

    out->bin_ms = bin;
    out->utilization.count = count;
    out->tx.count = count;
    bins_finish(&out->utilization, util_sums, counts, breaks);
    bins_finish(&out->tx, tx_sums, counts, breaks);

    int32_t high = 0;
    for (uint32_t i = 0U; i < count; ++i) {
        if (!out->utilization.present[i]) {
            continue;
        }
        if (out->utilization.values[i] > high) {
            high = out->utilization.values[i];
        }
        if (out->tx.values[i] > high) {
            high = out->tx.values[i];
        }
    }
    out->frame.from = from;
    out->frame.to = to;
    /* The airtime domain is the identity one: readings already permille. */
    out->frame.scale = mesh_ui_trend_domain((struct mesh_ui_scale){0, 0}, high);
    return true;
}
