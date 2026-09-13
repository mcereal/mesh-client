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

static struct mesh_ui_latency_counts counted(void) {
    struct mesh_ui_latency_counts counts;
    memset(&counts, 0, sizeof counts);
    mesh_ui_latency_counts(&counts);
    return counts;
}

/* One press that changed the frame, answered by one frame, `age_us` after the kernel stamped
   it. The stamp is taken from the probe's own clock so the arithmetic is the one the client
   does. */
static void press_answered(uint64_t age_us) {
    mesh_ui_latency_event(mesh_ui_latency_now_us() - age_us);
    mesh_ui_latency_press();
    mesh_ui_latency_press_handled(true);
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
    const struct mesh_ui_latency_counts counts = counted();
    MESH_TEST_FAIL_IF(counts.frames != 0U || counts.presses != 0U,
                      "a reset should leave no samples behind");
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
 * A press that changed nothing is not charged the next frame that happens to arrive.
 *
 * Down at the end of a list publishes no snapshot, so no frame is drawn for it - but a map
 * filling tiles or an animation settling is asking for one every 33 ms anyway. Held pending,
 * the press would be answered by that frame, and the histogram would fill up with the frame
 * rate of whatever else was moving. The store's own "does this repaint" is what tells them
 * apart, and mesh_ui_controller_handle_key() passes it straight through.
 */
MESH_TEST_CASE(latency_does_not_charge_an_inert_press_to_the_next_frame, unit) {
    begin();
    mesh_ui_latency_event(mesh_ui_latency_now_us() - 5000U);
    mesh_ui_latency_press();
    mesh_ui_latency_press_handled(false);

    /* Something else draws, a moment later. */
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_frame_drawn(4096U);
    mesh_ui_latency_frame_end(4096U);

    const struct mesh_ui_latency_counts counts = counted();
    MESH_TEST_FAIL_IF(counts.presses != 0U, "a press that repaints nothing is not timed");
    MESH_TEST_FAIL_IF(counts.inert != 1U, "but it is counted, so the run says how many there were");
    MESH_TEST_FAIL_IF(counts.frames != 1U, "the frame that did happen still happened");
    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS)->count != 0U,
                      "and it must not have been charged to the press");
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
    mesh_ui_latency_press_handled(true);
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_frame_drawn(4096U);
    mesh_ui_latency_frame_end(4096U);

    const struct mesh_ui_latency_counts counts = counted();
    MESH_TEST_FAIL_IF(counts.presses != 0U, "a repeat is not a press this probe can time");
    MESH_TEST_FAIL_IF(counts.inert != 0U, "nor is it an inert one - it is not a press at all");
    MESH_TEST_FAIL_IF(counts.frames != 1U, "the frame it drew still happened");
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
    mesh_ui_latency_press_handled(true);
    mesh_ui_latency_event(now - 1000U);
    mesh_ui_latency_press();
    mesh_ui_latency_press_handled(true);
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_frame_drawn(4096U);
    mesh_ui_latency_frame_end(4096U);

    const struct mesh_ui_latency_counts counts = counted();
    MESH_TEST_FAIL_IF(counts.presses != 2U, "both presses arrived");
    MESH_TEST_FAIL_IF(counts.coalesced != 1U, "and one of them shared the frame the other got");
    const struct mesh_ui_latency_histogram *const press =
        mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS);
    MESH_TEST_FAIL_IF(press->count != 1U, "one frame is one latency");
    MESH_TEST_FAIL_IF(press->max_us < 30000U, "measured from the press that waited longest");
    record_success(test_name);
}

/*
 * A press whose frame never came within the timeout is dropped rather than charged to a later
 * one. The store said it would repaint, so something went wrong - a frame that took a second is
 * not a press latency anybody should read as one.
 */
MESH_TEST_CASE(latency_drops_a_press_no_frame_answered, unit) {
    begin();
    press_answered(MESH_UI_LATENCY_PRESS_TIMEOUT_US + 500000U);

    const struct mesh_ui_latency_counts counts = counted();
    MESH_TEST_FAIL_IF(counts.unanswered != 1U, "the press should be recorded as unanswered");
    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS)->count != 0U,
                      "and must not appear in the percentiles");
    record_success(test_name);
}

/*
 * ...and so is one still waiting when the run ends.
 *
 * The last press of a run is the likely one: the client is stopped a moment later, so no frame
 * was ever drawn for it. Left in the pending slot it appears in neither column - not in the
 * percentiles, and not in the count of what nothing answered - which is a press that quietly
 * never happened.
 */
MESH_TEST_CASE(latency_expires_a_pending_press_when_the_run_ends, unit) {
    begin();
    mesh_ui_latency_event(mesh_ui_latency_now_us() - 2000U);
    mesh_ui_latency_press();
    mesh_ui_latency_press_handled(true);

    MESH_TEST_FAIL_IF(counted().unanswered != 0U, "nothing has ended yet");
    mesh_ui_latency_report("test");
    MESH_TEST_FAIL_IF(counted().unanswered != 1U, "the report should have expired it");
    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS)->count != 0U,
                      "without inventing a latency for it");

    /* And a second report does not count it twice. */
    mesh_ui_latency_report("test");
    MESH_TEST_FAIL_IF(counted().unanswered != 1U, "expiring is not something that repeats");
    record_success(test_name);
}

/* A stamp from the future is a device whose events are still on the wall clock - a node opened
   before the EVIOCSCLOCKID, or a kernel that refused it. Subtracting it gives a huge unsigned
   number, which is the one reading a histogram cannot survive. */
MESH_TEST_CASE(latency_refuses_a_stamp_that_is_not_on_its_own_clock, unit) {
    begin();
    mesh_ui_latency_event(mesh_ui_latency_now_us() + 60000000U);
    mesh_ui_latency_press();
    mesh_ui_latency_press_handled(true);
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_frame_drawn(4096U);
    mesh_ui_latency_frame_end(4096U);

    MESH_TEST_FAIL_IF(counted().presses != 0U,
                      "a wall-clock stamp is not a press this probe can time");
    MESH_TEST_FAIL_IF(mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS)->count != 0U,
                      "and nothing should have been charged to it");
    record_success(test_name);
}

/* One press is one candidate: a second key-down before the first was confirmed replaces it
   rather than queueing behind it, so a caller that forgets to confirm cannot leave a stamp
   lying about to be charged to some later press's frame. */
MESH_TEST_CASE(latency_keeps_one_candidate_press_at_a_time, unit) {
    begin();
    const uint64_t now = mesh_ui_latency_now_us();
    mesh_ui_latency_event(now - 90000U);
    mesh_ui_latency_press(); /* never confirmed */
    mesh_ui_latency_event(now - 3000U);
    mesh_ui_latency_press();
    mesh_ui_latency_press_handled(true);
    mesh_ui_latency_frame_begin();
    mesh_ui_latency_frame_drawn(4096U);
    mesh_ui_latency_frame_end(4096U);

    const struct mesh_ui_latency_histogram *const press =
        mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS);
    MESH_TEST_FAIL_IF(press->count != 1U, "only the confirmed press is timed");
    MESH_TEST_FAIL_IF(press->max_us > 60000U, "and it is the recent one, not the abandoned one");
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
    const struct mesh_ui_latency_counts counts = counted();
    MESH_TEST_FAIL_IF(counts.frames != 1U || counts.tile_frames != 1U,
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

    const struct mesh_ui_latency_counts counts = counted();
    MESH_TEST_FAIL_IF(counts.frames != 1U, "the frame happened");
    MESH_TEST_FAIL_IF(counts.tile_frames != 0U, "and it read no tile");
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
    MESH_TEST_FAIL_IF(counted().written != 0U, "and it put no bytes on the panel");
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
    MESH_TEST_FAIL_IF(counted().written != 65536U,
                      "the bytes handed to the panel should be counted");
    record_success(test_name);
}

/* An empty histogram answers 0 rather than walking off the end of its buckets. */
MESH_TEST_CASE(latency_percentile_of_nothing_is_nothing, unit) {
    begin();
    MESH_TEST_FAIL_IF(
        mesh_ui_latency_percentile(mesh_ui_latency_metric(MESH_UI_LATENCY_PRESS), 50U) != 0U,
        "no readings, no percentile");
    MESH_TEST_FAIL_IF(mesh_ui_latency_percentile(NULL, 50U) != 0U, "and no histogram either");
    record_success(test_name);
}
