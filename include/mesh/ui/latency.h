#ifndef MESH_UI_LATENCY_H
#define MESH_UI_LATENCY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How long a press takes to reach the panel, measured inside the client that is drawing it.
 *
 * This is the one number the standalone tile benchmark could not give: a frame of the *real*
 * client, with a tile read and decoded in it, while the BLE link is being serviced. The benchmark
 * is a second process on an idle launcher, so what it measures is the card and the decoder; what it
 * cannot measure is the thing that actually decides whether a map is usable here, which is the one
 * epoll loop having to do both.
 *
 * Four readings, and the first is the one the roadmap asks for:
 *
 *   press   the kernel's own timestamp on the evdev event, to the end of the present() that
 *           answered it. It starts at the kernel rather than at the read, because a loop busy
 *           decoding does not wake for the event at all - and that wait is the whole of what
 *           an integrated number adds to a standalone one.
 *   frame   the whole of present(): the two below, back to back.
 *   draw    the render and the damage compare - the client's own work, and the only part of a
 *           frame that a change to the drawing code can move.
 *   flip    handing it to the panel: FBIOPAN_DISPLAY and the msync after it. Recorded only for
 *           a frame that had something to hand over, because a frame whose damage compare found
 *           nothing does not pan at all and would otherwise report a flip of zero.
 *   read    the frame's one tile off the card.
 *   decode  that tile's PNG.
 *
 * The last two are split from the middle two on purpose. "The map is slow" and "the panel is
 * 60 Hz" are the same number at the end of a press and two entirely different things to do
 * about it, and a single figure for present() cannot tell them apart.
 *
 * Off unless it is switched on (MESHCLIENT_LATENCY_TRACE, or --trace-latency, which is the one
 * that reaches a Brick - NextUI's launch loop hands a pak no environment of its own). Every
 * entry point below is a no-op until then, so the probe costs a predictable-branch test per
 * frame in a build that is not measuring.
 *
 * What it keeps is a histogram rather than a ring of samples, which is the difference between
 * percentiles over the whole run and percentiles over the last few seconds of it: a fill frame
 * is twenty times as common as a press, so a ring sized for one is a window on the other. The
 * price is resolution - MESH_UI_LATENCY_FINE_US below - and the report says so rather than
 * printing a quarter-millisecond bucket edge as though it were a measurement.
 */

/* Bucket widths. Fine below the knee, coarse above it, and anything past the last bucket is
   counted and kept as the true maximum but has no percentile of its own. 50 us resolves a
   decode (about 1.2 ms on this hardware); 1 ms is plenty for a frame that has gone wrong. */
#define MESH_UI_LATENCY_FINE_US 50U
#define MESH_UI_LATENCY_FINE_BUCKETS 256U /* 0 .. 12.8 ms */
#define MESH_UI_LATENCY_COARSE_US 1000U
#define MESH_UI_LATENCY_COARSE_BUCKETS 128U /* 12.8 ms .. 140.8 ms */
#define MESH_UI_LATENCY_BUCKETS (MESH_UI_LATENCY_FINE_BUCKETS + MESH_UI_LATENCY_COARSE_BUCKETS + 1U)

/*
 * A press that no frame answered within this is not a slow frame, it is a press that changed
 * nothing on the panel - a direction at the end of a list, a button a screen does not use. Left
 * pending it would be charged to whatever frame came next, which on an animating screen is an
 * arbitrary number with nothing to do with the press.
 */
#define MESH_UI_LATENCY_PRESS_TIMEOUT_US 1000000U

struct mesh_ui_latency_histogram {
    uint32_t buckets[MESH_UI_LATENCY_BUCKETS];
    uint32_t count;
    uint32_t min_us;
    uint32_t max_us;
    uint64_t total_us;
};

/* Which reading a histogram holds. Ordered as the report prints them. */
enum mesh_ui_latency_metric {
    MESH_UI_LATENCY_PRESS = 0,
    MESH_UI_LATENCY_FRAME,
    MESH_UI_LATENCY_DRAW,
    MESH_UI_LATENCY_FLIP,
    MESH_UI_LATENCY_READ,
    MESH_UI_LATENCY_DECODE,
    MESH_UI_LATENCY_METRIC_COUNT,
};

/* CLOCK_MONOTONIC in microseconds - the same clock evdev is put on, so a kernel timestamp and a
   reading taken here are comparable. 0 if the clock read fails, which the probe treats as "no
   reading" rather than as an instant. */
uint64_t mesh_ui_latency_now_us(void);

/* Switch it on. Idempotent; --trace-latency calls this, and the environment knob is read the
   first time anything asks whether it is enabled. */
void mesh_ui_latency_enable(void);
bool mesh_ui_latency_enabled(void);

/* Forget every sample. Tests, and a second measurement in one process. */
void mesh_ui_latency_reset(void);

/* One evdev event, carrying the kernel's timestamp for it. Called for every event read, before
   the mapping decides whether it is a key at all. */
void mesh_ui_latency_event(uint64_t event_us);

/*
 * ...and that event was a button going down on a key this client maps.
 *
 * It is held as a *candidate* rather than counted, because whether a press costs anything is
 * not knowable here: one that changes nothing - Down at the end of a list, a button a screen
 * does not use - publishes no snapshot and draws no frame. mesh_ui_latency_press_handled()
 * below is the store's answer, and only a confirmed press is ever charged to a frame.
 *
 * A repeat is deliberately not a press, and there are two kinds. Ours comes from input.c's own
 * timerfd and reaches the probe with no event behind it at all, so it is refused here. The
 * kernel's autorepeat on a face button does carry a stamp, and is refused by the caller: a held
 * button is one press, and counting its repeats would weigh whatever screen that button drives
 * by how long somebody leant on it.
 */
void mesh_ui_latency_press(void);

/*
 * Whether that press changed the frame, which is mesh_ui_store_handle_key()'s own return value.
 *
 * With it, the candidate becomes the press a frame is charged with - the *oldest* one not yet
 * answered, because several presses coalesce into one snapshot and so into one frame, and the
 * reader who pressed first is the one who waited. Without it the candidate is dropped and
 * counted as inert, so the next frame to arrive - an animation, or the map fill loop asking for
 * another turn - is not charged to a press that nobody ever waited on.
 */
void mesh_ui_latency_press_handled(bool repaints);

void mesh_ui_latency_frame_begin(void);

/* The drawing is done and the damage compare has said how many bytes of it are new. Everything
   after this call is the panel's. */
void mesh_ui_latency_frame_drawn(size_t written);

/* ...and it is on the panel. `written` is what the damage compare found, repeated so a frame
   that handed over nothing is not counted as a flip of no time at all. */
void mesh_ui_latency_frame_end(size_t written);

/* The frame's one tile: what the read off the card cost and what the decode cost. Either may be
   0 when that half did not run. */
void mesh_ui_latency_tile(uint64_t read_us, uint64_t decode_us);

/* Percentiles, one log line per metric, plus a line of counts. `why` names the moment ("exit"),
   and a report with nothing in it says so rather than printing four empty rows. */
void mesh_ui_latency_report(const char *why);

/* Read back, for tests and for anything that wants the numbers rather than the log line. */
const struct mesh_ui_latency_histogram *mesh_ui_latency_metric(enum mesh_ui_latency_metric which);

/* The `percent`th percentile in microseconds, as the upper edge of the bucket it lands in - so
   it is an upper bound rather than a sample, and never below min or above max. 0 when the
   histogram is empty. */
uint32_t mesh_ui_latency_percentile(const struct mesh_ui_latency_histogram *histogram,
                                    unsigned int percent);

/* What the run did, as opposed to how long it took. A struct rather than a row of out
   parameters: these are read together, by a report and by a test, and there are now seven. */
struct mesh_ui_latency_counts {
    uint32_t frames;
    uint32_t tile_frames; /* ...of which this many read a tile. Never more than one each. */
    uint32_t presses;     /* key-downs the store said would repaint */
    uint32_t inert;       /* ...and key-downs it said would not, which are timed by nothing */
    uint32_t coalesced;   /* presses that shared a frame with an older one */
    uint32_t unanswered;  /* presses the frame never came for */
    /* What the damage compare handed the panel, which is the other half of the partial-redraw
       question: a frame's cost is what it drew *and* what it copied. */
    uint64_t written;
};

void mesh_ui_latency_counts(struct mesh_ui_latency_counts *out);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_LATENCY_H */
