/*
 * The airtime chart: this client's half of what inkcell/ui/trend.h frames.
 *
 * The span, the domain, the projection and the bin ladder are inkcell's - they are arithmetic
 * about a window and a ceiling, and the fb backend must not be the only thing that can ask.
 * What is here is the one chart whose *source* is a fact about a radio: how often LocalStats
 * arrives, what a gap in it means, and that utilization and tx are two lines over one window.
 *
 * Nothing here has a pixel in it, for the same reason nothing in inkcell/ui/trend.h does.
 */

#include "mesh/ui/trend.h"

#include "mesh/ui/anim.h"
#include "mesh/ui/history.h"

#include <stddef.h>
#include <string.h>

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

static void bins_finish(struct inkcell_trend_bins *bins, const int64_t *sums,
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
    /* The span's cut, as inkcell_trend_frame() makes it: anchored on the newest reading and only
       ever narrowing. */
    const uint32_t ms = inkcell_trend_span_ms(span);
    if (ms > 0U && (to - from) > ms) {
        from = to - ms;
    }

    uint32_t cap = max_bins > INKCELL_TREND_BINS_MAX ? INKCELL_TREND_BINS_MAX : max_bins;
    cap = cap < 2U ? 2U : cap;
    const uint32_t cadence = airtime_cadence(history, from, to);
    const uint32_t bin = inkcell_trend_bin_ms(to - from, cadence, cap);
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

    int64_t util_sums[INKCELL_TREND_BINS_MAX];
    int64_t tx_sums[INKCELL_TREND_BINS_MAX];
    uint32_t counts[INKCELL_TREND_BINS_MAX];
    bool breaks[INKCELL_TREND_BINS_MAX];
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
    out->frame.scale = inkcell_trend_domain((struct inkcell_scale){0, 0}, high);
    return true;
}
