#define _POSIX_C_SOURCE 200809L

/*
 * The press-to-panel probe: what it counts, what it refuses to count, and what a percentile
 * taken from a histogram is allowed to claim.
 *
 * Every case here is the probe lying rather than the probe failing, which is the only way a
 * measurement tool goes wrong that anybody notices too late. A number that is merely absent
 * gets investigated; a number that is confidently a third of the truth gets written into a
 * roadmap and built on.
 */

#include "framework/mesh_test.h"

#include "mesh/ui/latency.h"

#include <stdlib.h>
#include <string.h>

/* Every case switches the probe on and starts from nothing: it is file-static state shared by
   the whole binary, and the suites run in one process. */
static void begin(void) {
    mesh_ui_latency_enable();
    mesh_ui_latency_reset();
}

/* One press, answered by one frame, `wait_us` after the kernel stamped it. The stamp is taken
   from the probe's own clock so the arithmetic is the same one the client does. */
static void press_answered(uint64_t age_us) {
    const uint64_t now = mesh_ui_latency_now_us();
    mesh_ui_latency_event(now - age_us);
    mesh_ui_latency_press();
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_frame_drawn(4096U);
    mesh_ui_latency_frame_end(4096U);
}

MESH_TEST_CASE(latency_reset_leaves_nothing_behind, unit) {
    mesh_ui_latency_enable();
    mesh_ui_latency_reset();
    /* There is no way back to "off" once a run has asked for it - the knob is read once - so
       what this checks is that a reset leaves nothing behind, which is the state a client that
       never enabled it is in. */
    uint32_t frames = 1U;
    uint32_t presses = 1U;
    mesh_ui_latency_counts(&frames, NULL, &presses, NULL, NULL);
    MESH_TEST_FAIL_IF(frames != 0U || presses != 0U, "a reset should leave no samples behind");
    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS)->count != 0U,
                      "and no histogram either");
    record_success(test_name);
}

/*
 * The measurement starts at the kernel's stamp, not at the frame.
 *
 * This is the whole of what an integrated number adds to the standalone benchmark in
 * docs/maps-roadmap.md: a loop busy decoding a tile does not wake for the event at all, and the
 * wait it does not wake for is invisible to anything that starts timing once the event has been
 * read.
 */
MESH_TEST_CASE(latency_measures_a_press_from_the_kernels_stamp, unit) {
    begin();
    press_answered(8000U);

    const struct mesh_ui_latency_histogram *const press =
        mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS);
    MESH_TEST_FAIL_IF(press->count != 1U, "the press should have been recorded once");
    MESH_TEST_FAIL_IF(press->min_us < 8000U,
                      "a press stamped 8 ms ago cannot have been answered in less");
    MESH_TEST_FAIL_IF(press->min_us > 200000U, "and it did not take a fifth of a second either");
    record_success(test_name);
}

/*
 * A repeat has no kernel stamp, so it is not counted at all.
 *
 * src/ui/input.c generates key repeat from its own timerfd - the d-pad is an absolute axis and
 * never repeats itself - so a held direction reaches the store with no evdev event behind it.
 * Counted from "now" it would report a queueing delay of zero on exactly the presses a held pan
 * is made of, which is the fill loop's worst case reported as its best.
 */
MESH_TEST_CASE(latency_does_not_count_a_repeat_as_a_press, unit) {
    begin();
    mesh_ui_latency_press(); /* no mesh_ui_latency_event() before it */
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_frame_drawn(4096U);
    mesh_ui_latency_frame_end(4096U);

    uint32_t presses = 99U;
    uint32_t frames = 0U;
    mesh_ui_latency_counts(&frames, NULL, &presses, NULL, NULL);
    MESH_TEST_FAIL_IF(presses != 0U, "a repeat is not a press this probe can time");
    MESH_TEST_FAIL_IF(frames != 1U, "the frame it drew still happened");
    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS)->count != 0U,
                      "and nothing should have been charged to it");
    record_success(test_name);
}

/*
 * Several presses answered by one frame are charged to the oldest of them.
 *
 * The store coalesces: three presses in one epoll batch produce one snapshot and so one
 * present(). Timed from the newest, the frame looks fast and the reader who pressed first - the
 * one who actually waited - is the one not counted.
 */
MESH_TEST_CASE(latency_charges_a_coalesced_frame_to_the_oldest_press, unit) {
    begin();
    const uint64_t now = mesh_ui_latency_now_us();
    mesh_ui_latency_event(now - 30000U);
    mesh_ui_latency_press();
    mesh_ui_latency_event(now - 1000U);
    mesh_ui_latency_press();
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_frame_drawn(4096U);
    mesh_ui_latency_frame_end(4096U);

    uint32_t presses = 0U;
    uint32_t coalesced = 0U;
    mesh_ui_latency_counts(NULL, NULL, &presses, &coalesced, NULL);
    MESH_TEST_FAIL_IF(presses != 2U, "both presses arrived");
    MESH_TEST_FAIL_IF(coalesced != 1U, "and one of them shared the frame the other got");
    const struct mesh_ui_latency_histogram *const press =
        mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS);
    MESH_TEST_FAIL_IF(press->count != 1U, "one frame is one latency");
    MESH_TEST_FAIL_IF(press->max_us < 30000U, "measured from the press that waited longest");
    record_success(test_name);
}

/*
 * A press nothing answered is dropped rather than charged to the next frame.
 *
 * Down at the end of a list changes nothing, so no snapshot is published and no frame is drawn
 * for it. Left pending, it would be handed to whatever came next - an animation tick, a packet
 * arriving a minute later - and the probe would report a latency of a minute for a press that
 * was answered correctly by doing nothing.
 */
MESH_TEST_CASE(latency_drops_a_press_no_frame_answered, unit) {
    begin();
    press_answered(MESH_UI_LATENCY_PRESS_TIMEOUT_US + 500000U);

    uint32_t unanswered = 0U;
    mesh_ui_latency_counts(NULL, NULL, NULL, NULL, &unanswered);
    MESH_TEST_FAIL_IF(unanswered != 1U, "the press should be recorded as unanswered");
    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS)->count != 0U,
                      "and must not appear in the percentiles");
    record_success(test_name);
}

/* A stamp from the future is a device whose events are still on the wall clock - a node opened
   before the EVIOCSCLOCKID, or a kernel that refused it. Subtracting it gives a huge unsigned
   number, which is the one reading a histogram cannot survive. */
MESH_TEST_CASE(latency_refuses_a_stamp_that_is_not_on_its_own_clock, unit) {
    begin();
    mesh_ui_latency_event(mesh_ui_latency_now_us() + 60000000U);
    mesh_ui_latency_press();
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_frame_drawn(4096U);
    mesh_ui_latency_frame_end(4096U);

    uint32_t presses = 99U;
    mesh_ui_latency_counts(NULL, NULL, &presses, NULL, NULL);
    MESH_TEST_FAIL_IF(presses != 0U, "a wall-clock stamp is not a press this probe can time");
    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS)->count != 0U,
                      "and nothing should have been charged to it");
    record_success(test_name);
}

/* The two halves of a tile are separate readings, because they answer different questions: the
   read is the card's and the decode is the CPU's. */
MESH_TEST_CASE(latency_keeps_the_read_and_the_decode_apart, unit) {
    begin();
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_tile(800U, 1240U);
    mesh_ui_latency_frame_drawn(4096U);
    mesh_ui_latency_frame_end(4096U);

    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_READ)->max_us != 800U,
                      "the read should be recorded as itself");
    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_DECODE)->max_us != 1240U,
                      "and so should the decode");
    uint32_t frames = 0U;
    uint32_t tile_frames = 0U;
    mesh_ui_latency_counts(&frames, &tile_frames, NULL, NULL, NULL);
    MESH_TEST_FAIL_IF(frames != 1U || tile_frames != 1U,
                      "the frame that read a tile should be counted as one");
    record_success(test_name);
}

/* A frame with no tile in it is still a frame, and must not be counted as one that read one -
   the fill loop reads at most one per frame and most frames read none. */
MESH_TEST_CASE(latency_counts_a_frame_with_no_tile_in_it, unit) {
    begin();
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_frame_drawn(4096U);
    mesh_ui_latency_frame_end(4096U);

    uint32_t frames = 0U;
    uint32_t tile_frames = 99U;
    mesh_ui_latency_counts(&frames, &tile_frames, NULL, NULL, NULL);
    MESH_TEST_FAIL_IF(frames != 1U, "the frame happened");
    MESH_TEST_FAIL_IF(tile_frames != 0U, "and it read no tile");
    record_success(test_name);
}

/*
 * A percentile is the upper edge of the bucket it lands in, and is never outside the readings.
 *
 * The edge is what a histogram can honestly say - the sample itself is not kept - but an edge
 * is an artefact of the bucketing, so a p99 above the largest reading or a p0 below the
 * smallest would be the probe inventing a measurement. Clamping to min and max is what keeps
 * the resolution loss from becoming a claim.
 */
MESH_TEST_CASE(latency_percentiles_stay_inside_the_readings, unit) {
    begin();
    mesh_ui_latency_frame_begin();
    for (unsigned int i = 0U; i < 100U; ++i) {
        mesh_ui_latency_tile(1000U + i * 10U, 1U);
    }
    mesh_ui_latency_frame_drawn(4096U);
    mesh_ui_latency_frame_end(4096U);

    const struct mesh_ui_latency_histogram *const read =
        mesh_ui_latency_metric(MESH_UI_LATENCY_READ);
    MESH_TEST_FAIL_IF(read->count != 100U, "every reading should be in there");
    MESH_TEST_FAIL_IF(read->min_us != 1000U || read->max_us != 1990U,
                      "min and max are kept exactly rather than bucketed");
    const uint32_t p50 = mesh_ui_latency_percentile(read, 50U);
    const uint32_t p99 = mesh_ui_latency_percentile(read, 99U);
    MESH_TEST_FAIL_IF(p50 < read->min_us || p50 > read->max_us,
                      "p50 should be inside the readings");
    MESH_TEST_FAIL_IF(p99 < p50 || p99 > read->max_us, "and p99 above it and still inside");
    MESH_TEST_FAIL_IF(p50 > 1550U, "the median of 1.00-1.99 ms is not up at the top of it");
    record_success(test_name);
}

/* A reading past the last bucket is counted and kept as the maximum rather than dropped: a
   frame that took a second is the most interesting frame of the run. */
MESH_TEST_CASE(latency_keeps_a_reading_past_the_last_bucket, unit) {
    begin();
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_tile(5000000U, 1U);
    mesh_ui_latency_frame_drawn(4096U);
    mesh_ui_latency_frame_end(4096U);

    const struct mesh_ui_latency_histogram *const read =
        mesh_ui_latency_metric(MESH_UI_LATENCY_READ);
    MESH_TEST_FAIL_IF(read->count != 1U, "an overflowing reading is still a reading");
    MESH_TEST_FAIL_IF(read->max_us != 5000000U, "and the maximum is exact, not a bucket edge");
    MESH_TEST_FAIL_IF(mesh_ui_latency_percentile(read, 100U) != 5000000U,
                      "p100 of one reading is that reading");
    record_success(test_name);
}

/*
 * A frame that handed nothing to the panel is not a flip of no time.
 *
 * The damage compare finds nothing on any frame where the snapshot changed somewhere the panel
 * does not show, and such a frame never calls FBIOPAN_DISPLAY at all. Recorded as a flip, it
 * would be a zero in the one histogram that exists to say how much of a press is spent waiting
 * for the panel - and on a screen that is mostly still, the zeroes are the majority.
 */
MESH_TEST_CASE(latency_does_not_call_an_unwritten_frame_a_flip, unit) {
    begin();
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_frame_drawn(0U);
    mesh_ui_latency_frame_end(0U);

    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_FLIP)->count != 0U,
                      "a frame that wrote nothing did not flip");
    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_DRAW)->count != 1U,
                      "but it was still drawn, and the drawing is what it cost");
    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_FRAME)->count != 1U,
                      "and it is still a frame");
    MESH_TEST_FAIL_IF(mesh_ui_latency_written() != 0U, "and it put no bytes on the panel");
    record_success(test_name);
}

/* The draw and the flip are the two halves of a frame, and neither is the whole of it. */
MESH_TEST_CASE(latency_splits_a_frame_into_the_draw_and_the_flip, unit) {
    begin();
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_frame_drawn(65536U);
    mesh_ui_latency_frame_end(65536U);

    const uint32_t frame = mesh_ui_latency_metric(MESH_UI_LATENCY_FRAME)->max_us;
    const uint32_t draw = mesh_ui_latency_metric(MESH_UI_LATENCY_DRAW)->max_us;
    const uint32_t flip = mesh_ui_latency_metric(MESH_UI_LATENCY_FLIP)->max_us;
    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_FLIP)->count != 1U,
                      "a frame that wrote bytes flipped");
    MESH_TEST_FAIL_IF(draw > frame || flip > frame,
                      "neither half may be larger than the frame it is half of");
    MESH_TEST_FAIL_IF(mesh_ui_latency_written() != 65536U,
                      "the bytes handed to the panel should be counted");
    record_success(test_name);
}

/* An empty histogram answers 0 rather than walking off the end of its buckets. */
MESH_TEST_CASE(latency_percentile_of_nothing_is_nothing, unit) {
    begin();
    MESH_TEST_FAIL_IF(mesh_ui_latency_percentile(mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS),
                                                 50U) != 0U,
                      "no readings, no percentile");
    MESH_TEST_FAIL_IF(mesh_ui_latency_percentile(NULL, 50U) != 0U, "and no histogram either");
    record_success(test_name);
}
