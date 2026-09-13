#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/latency.h"

#include "mesh/utils/env.h"
#include "mesh/utils/log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * The press-to-panel probe. See include/mesh/ui/latency.h for what it is for and why it keeps
 * histograms rather than samples.
 *
 * File-static rather than an instance handed around, for the same reason the framebuffer's
 * clock reading is: there is one client, one loop and one panel, and the two ends of the
 * measurement are in two subsystems that have no business holding a pointer to each other. The
 * input reader stamps a press and the backend closes the frame; neither knows the other exists.
 */

static bool s_enabled;
static bool s_env_read;

static struct mesh_ui_latency_histogram s_metrics[MESH_UI_LATENCY_METRIC_COUNT];

/*
 * The three stages a press goes through, which are three different claims:
 *   s_event_us      an evdev event was read, and this is when the kernel says it happened
 *   s_candidate_us  ...and it was a button going down on a key this client maps
 *   s_pending_us    ...and the store said it would change the frame, so a frame owes it an answer
 */
static uint64_t s_event_us;
static uint64_t s_candidate_us;
static uint64_t s_pending_press_us;

static uint64_t s_frame_start_us;
static uint64_t s_frame_drawn_us;
static uint64_t s_frame_tile_us;
static uint64_t s_written;

static uint32_t s_frames;
static uint32_t s_tile_frames;
static uint32_t s_presses;
static uint32_t s_inert;
static uint32_t s_coalesced;
static uint32_t s_unanswered;

uint64_t mesh_ui_latency_now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000000U + (uint64_t)ts.tv_nsec / 1000U;
}

void mesh_ui_latency_enable(void) {
    s_enabled = true;
    s_env_read = true;
}

bool mesh_ui_latency_enabled(void) {
    if (!s_env_read) {
        s_env_read = true;
        s_enabled = mesh_env_bool("MESHCLIENT_LATENCY_TRACE", "latency trace", false);
    }
    return s_enabled;
}

void mesh_ui_latency_reset(void) {
    memset(s_metrics, 0, sizeof s_metrics);
    s_event_us = 0U;
    s_candidate_us = 0U;
    s_pending_press_us = 0U;
    s_frame_start_us = 0U;
    s_frame_drawn_us = 0U;
    s_frame_tile_us = 0U;
    s_written = 0U;
    s_frames = 0U;
    s_tile_frames = 0U;
    s_presses = 0U;
    s_inert = 0U;
    s_coalesced = 0U;
    s_unanswered = 0U;
}

/*
 * Which bucket a reading falls in.
 *
 * The fine range is the one every healthy reading lands in; the coarse range exists so that a
 * frame which went wrong still has a percentile rather than piling into an overflow that makes
 * p99 unreadable. The last bucket is the overflow and is deliberately not given an upper edge:
 * what is true about it is the maximum, which is kept exactly.
 */
static size_t mesh_ui_latency_bucket(uint32_t value_us) {
    const uint32_t fine_span = MESH_UI_LATENCY_FINE_US * MESH_UI_LATENCY_FINE_BUCKETS;
    if (value_us < fine_span) {
        return value_us / MESH_UI_LATENCY_FINE_US;
    }
    const uint32_t over = value_us - fine_span;
    const uint32_t coarse = over / MESH_UI_LATENCY_COARSE_US;
    if (coarse < MESH_UI_LATENCY_COARSE_BUCKETS) {
        return MESH_UI_LATENCY_FINE_BUCKETS + coarse;
    }
    return MESH_UI_LATENCY_BUCKETS - 1U;
}

/* The upper edge of a bucket, which is what a percentile answers with. The overflow bucket has
   no edge of its own, so it answers with the maximum seen. */
static uint32_t mesh_ui_latency_bucket_edge(const struct mesh_ui_latency_histogram *histogram,
                                            size_t bucket) {
    if (bucket < MESH_UI_LATENCY_FINE_BUCKETS) {
        return (uint32_t)(bucket + 1U) * MESH_UI_LATENCY_FINE_US;
    }
    if (bucket < MESH_UI_LATENCY_FINE_BUCKETS + MESH_UI_LATENCY_COARSE_BUCKETS) {
        const uint32_t fine_span = MESH_UI_LATENCY_FINE_US * MESH_UI_LATENCY_FINE_BUCKETS;
        return fine_span +
               (uint32_t)(bucket - MESH_UI_LATENCY_FINE_BUCKETS + 1U) * MESH_UI_LATENCY_COARSE_US;
    }
    return histogram->max_us;
}

static void mesh_ui_latency_record(enum mesh_ui_latency_metric which, uint64_t value_us) {
    if (which >= MESH_UI_LATENCY_METRIC_COUNT) {
        return;
    }
    struct mesh_ui_latency_histogram *const histogram = &s_metrics[which];
    const uint32_t value = value_us > UINT32_MAX ? UINT32_MAX : (uint32_t)value_us;
    histogram->buckets[mesh_ui_latency_bucket(value)] += 1U;
    if (histogram->count == 0U || value < histogram->min_us) {
        histogram->min_us = value;
    }
    if (value > histogram->max_us) {
        histogram->max_us = value;
    }
    histogram->total_us += value;
    histogram->count += 1U;
}

void mesh_ui_latency_event(uint64_t event_us) {
    if (!mesh_ui_latency_enabled()) {
        return;
    }
    s_event_us = event_us;
}

void mesh_ui_latency_press(void) {
    if (!mesh_ui_latency_enabled()) {
        return;
    }
    const uint64_t event_us = s_event_us;
    s_event_us = 0U;
    /* A press with no event behind it is a repeat from our own timer, which has no kernel
       stamp and so no latency this probe can honestly state. It is not counted at all rather
       than counted from now, which would report the queueing delay as zero. */
    if (event_us == 0U) {
        return;
    }
    /* A stamp from the future is a device evdev would not put on CLOCK_MONOTONIC - a node
       opened before the ioctl, or a host that refused it. */
    if (event_us > mesh_ui_latency_now_us()) {
        return;
    }
    s_candidate_us = event_us;
}

void mesh_ui_latency_press_handled(bool repaints) {
    if (!mesh_ui_latency_enabled()) {
        return;
    }
    const uint64_t candidate = s_candidate_us;
    s_candidate_us = 0U;
    if (candidate == 0U) {
        return;
    }
    if (!repaints) {
        /* Nothing was published, so no frame is coming for this. Counted, because a run made
           mostly of presses that did nothing is a run whose percentiles describe very little,
           and the count is the only thing that says so. */
        s_inert += 1U;
        return;
    }
    s_presses += 1U;
    if (s_pending_press_us != 0U) {
        s_coalesced += 1U;
    } else {
        s_pending_press_us = candidate;
    }
}

void mesh_ui_latency_frame_begin(void) {
    if (!mesh_ui_latency_enabled()) {
        return;
    }
    s_frame_start_us = mesh_ui_latency_now_us();
    s_frame_drawn_us = 0U;
    s_frame_tile_us = 0U;
}

void mesh_ui_latency_frame_drawn(size_t written) {
    if (!mesh_ui_latency_enabled() || s_frame_start_us == 0U) {
        return;
    }
    s_frame_drawn_us = mesh_ui_latency_now_us();
    mesh_ui_latency_record(MESH_UI_LATENCY_DRAW, s_frame_drawn_us - s_frame_start_us);
    s_written += (uint64_t)written;
}

void mesh_ui_latency_tile(uint64_t read_us, uint64_t decode_us) {
    if (!mesh_ui_latency_enabled()) {
        return;
    }
    mesh_ui_latency_record(MESH_UI_LATENCY_READ, read_us);
    mesh_ui_latency_record(MESH_UI_LATENCY_DECODE, decode_us);
    s_frame_tile_us += read_us + decode_us;
}

void mesh_ui_latency_frame_end(size_t written) {
    if (!mesh_ui_latency_enabled() || s_frame_start_us == 0U) {
        return;
    }
    const uint64_t now = mesh_ui_latency_now_us();
    mesh_ui_latency_record(MESH_UI_LATENCY_FRAME, now - s_frame_start_us);
    if (written > 0U && s_frame_drawn_us != 0U) {
        mesh_ui_latency_record(MESH_UI_LATENCY_FLIP, now - s_frame_drawn_us);
    }
    s_frames += 1U;
    if (s_frame_tile_us != 0U) {
        s_tile_frames += 1U;
    }
    s_frame_start_us = 0U;
    s_frame_drawn_us = 0U;

    if (s_pending_press_us == 0U) {
        return;
    }
    const uint64_t waited = now - s_pending_press_us;
    s_pending_press_us = 0U;
    if (waited > MESH_UI_LATENCY_PRESS_TIMEOUT_US) {
        /* Nothing answered that press; this frame is somebody else's. */
        s_unanswered += 1U;
        return;
    }
    mesh_ui_latency_record(MESH_UI_LATENCY_PRESS, waited);
}

const struct mesh_ui_latency_histogram *mesh_ui_latency_metric(enum mesh_ui_latency_metric which) {
    return which < MESH_UI_LATENCY_METRIC_COUNT ? &s_metrics[which] : NULL;
}

uint32_t mesh_ui_latency_percentile(const struct mesh_ui_latency_histogram *histogram,
                                    unsigned int percent) {
    if (histogram == NULL || histogram->count == 0U) {
        return 0U;
    }
    if (percent > 100U) {
        percent = 100U;
    }
    /* The rank the percentile names, rounded up: p50 of two samples is the second of them, and
       p100 is the last - so the walk below always terminates inside a bucket that has counts
       in it. */
    const uint64_t rank = ((uint64_t)histogram->count * percent + 99U) / 100U;
    uint64_t seen = 0U;
    for (size_t bucket = 0; bucket < MESH_UI_LATENCY_BUCKETS; ++bucket) {
        seen += histogram->buckets[bucket];
        if (seen >= rank && rank != 0U) {
            const uint32_t edge = mesh_ui_latency_bucket_edge(histogram, bucket);
            /* An edge below the smallest reading, or above the largest, describes the bucket
               rather than the data. */
            if (edge < histogram->min_us) {
                return histogram->min_us;
            }
            return edge > histogram->max_us ? histogram->max_us : edge;
        }
    }
    return histogram->max_us;
}

void mesh_ui_latency_counts(struct mesh_ui_latency_counts *out) {
    if (out == NULL) {
        return;
    }
    out->frames = s_frames;
    out->tile_frames = s_tile_frames;
    out->presses = s_presses;
    out->inert = s_inert;
    out->coalesced = s_coalesced;
    out->unanswered = s_unanswered;
    out->written = s_written;
}

static const char *const k_metric_names[MESH_UI_LATENCY_METRIC_COUNT] = {
    [MESH_UI_LATENCY_PRESS] = "press", [MESH_UI_LATENCY_FRAME] = "frame",
    [MESH_UI_LATENCY_DRAW] = "draw",   [MESH_UI_LATENCY_FLIP] = "flip",
    [MESH_UI_LATENCY_READ] = "read",   [MESH_UI_LATENCY_DECODE] = "decode",
};

/* Microseconds as milliseconds to two places, without floating point - the client has none on
   its drawing path and there is no reason for the report to introduce it. */
static void mesh_ui_latency_ms(char *out, size_t size, uint32_t value_us) {
    snprintf(out, size, "%u.%02u", value_us / 1000U, (value_us % 1000U) / 10U);
}

void mesh_ui_latency_report(const char *why) {
    if (!mesh_ui_latency_enabled()) {
        return;
    }
    const char *const moment = why != NULL ? why : "report";
    /* A press still waiting when the run ends never got its frame - the client was stopped
       before it drew one. Expired here, or the last press of every run goes missing from both
       columns: not in the percentiles, and not in the count of the ones nothing answered. */
    if (s_pending_press_us != 0U) {
        s_pending_press_us = 0U;
        s_unanswered += 1U;
    }
    mesh_log_info("latency",
                  "%s: %u frames, %u with a tile, %u presses (%u inert, %u coalesced, "
                  "%u unanswered), %llu KiB to the panel",
                  moment, s_frames, s_tile_frames, s_presses, s_inert, s_coalesced, s_unanswered,
                  (unsigned long long)(s_written / 1024U));

    for (size_t i = 0; i < MESH_UI_LATENCY_METRIC_COUNT; ++i) {
        const struct mesh_ui_latency_histogram *const histogram = &s_metrics[i];
        if (histogram->count == 0U) {
            mesh_log_info("latency", "%-6s nothing measured", k_metric_names[i]);
            continue;
        }
        char min[16];
        char p50[16];
        char p90[16];
        char p99[16];
        char max[16];
        char mean[16];
        mesh_ui_latency_ms(min, sizeof min, histogram->min_us);
        mesh_ui_latency_ms(p50, sizeof p50, mesh_ui_latency_percentile(histogram, 50U));
        mesh_ui_latency_ms(p90, sizeof p90, mesh_ui_latency_percentile(histogram, 90U));
        mesh_ui_latency_ms(p99, sizeof p99, mesh_ui_latency_percentile(histogram, 99U));
        mesh_ui_latency_ms(max, sizeof max, histogram->max_us);
        mesh_ui_latency_ms(mean, sizeof mean,
                           (uint32_t)(histogram->total_us / (uint64_t)histogram->count));
        mesh_log_info("latency", "%-6s n=%u min=%s p50=%s p90=%s p99=%s max=%s mean=%s ms",
                      k_metric_names[i], histogram->count, min, p50, p90, p99, max, mean);
    }
    mesh_log_info("latency", "percentiles are bucket edges: %u us below %u ms, %u ms above",
                  MESH_UI_LATENCY_FINE_US,
                  (MESH_UI_LATENCY_FINE_US * MESH_UI_LATENCY_FINE_BUCKETS) / 1000U,
                  MESH_UI_LATENCY_COARSE_US / 1000U);
}
