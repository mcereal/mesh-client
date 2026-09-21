#define _POSIX_C_SOURCE 200809L

/*
 * The per-node trend log on the card: what a node's readings have been doing, across a restart.
 *
 * The claim the whole thing exists to make is the one the first case here holds: a client
 * relaunched opens a node's detail on the trend it had rather than on nothing. Everything else
 * is what that costs - a seam where nothing was watching, a chain that survives compaction, and
 * a log that belongs to one radio.
 */

#include "framework/mesh_test.h"
#include "support/fs_fixture.h"

#include "mesh/ui/history.h"
#include "mesh/ui/store_trends.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- fixtures ------------------------------------------------------------------------------- */

static bool trends_open(struct mesh_ui_trends *trends, char *dir, size_t dir_len) {
    snprintf(dir, dir_len, "/tmp/mesh_trends_XXXXXX");
    if (mkdtemp(dir) == NULL) {
        return false;
    }
    /* mkdtemp has already made it; the log is pointed at it and finds it there. */
    return mesh_ui_trends_init(trends, dir) == 0;
}

/* A history with nothing in it and a clock of its own, as a fresh run has. */
static void trends_history(struct mesh_ui_history *history) { mesh_ui_history_reset(history); }

/* How many samples one node's series of `reading` holds. */
static uint32_t trends_count(const struct mesh_ui_history *history, uint32_t node_id,
                             enum mesh_ui_history_reading reading) {
    const struct inkcell_series *series = mesh_ui_history_series(history, node_id, reading);
    return series != NULL ? series->count : 0U;
}

static bool trends_values(const struct mesh_ui_history *history, uint32_t node_id,
                          enum mesh_ui_history_reading reading, const int32_t *expected,
                          uint32_t count) {
    const struct inkcell_series *series = mesh_ui_history_series(history, node_id, reading);
    if (series == NULL || series->count != count) {
        return false;
    }
    for (uint32_t i = 0U; i < count; ++i) {
        const struct inkcell_sample *sample = inkcell_series_at(series, i);
        if (sample == NULL || sample->value != expected[i]) {
            return false;
        }
    }
    return true;
}

/* ---- the case the log exists for ------------------------------------------------------------ */

/*
 * A node's trend survives a restart, and the clock it comes back to is not the one it left.
 *
 * The same shape as history_airtime_survives_a_restart_onto_a_new_clock and for the same reason:
 * a sample is stamped with a clock that counts from boot, so the second session here deliberately
 * runs on a clock far *below* the first's. A restore that kept the saved stamps would be undone
 * by the first live reading, which inkcell_series_push() would read as the clock having gone
 * backwards.
 */
MESH_TEST_CASE(ui_trends_survive_a_restart, unit) {
    const char *failure = NULL;
    struct mesh_ui_trends first;
    char dir[64];
    MESH_TEST_FAIL_IF(!trends_open(&first, dir, sizeof dir), "could not open a trend log");

    /* Session one, on a device that has been up for a while. */
    struct mesh_ui_history one;
    trends_history(&one);
    const uint32_t old_clock = 9U * 60U * 60U * 1000U;
    for (uint32_t i = 0U; i < 4U; ++i) {
        const uint32_t at = old_clock + i * MESH_UI_HISTORY_NODE_REPORT_MS;
        mesh_ui_history_note_battery(&one, at, 0x1234U, (uint8_t)(90U - i * 5U));
        mesh_ui_history_note_environment(&one, at, 0x1234U, true, (int32_t)(200 + (int32_t)i),
                                         false, 0);
        if (mesh_ui_trends_append(&first, &one) <= 0) {
            failure = "a reading the history took should have reached the card";
            goto cleanup;
        }
    }
    /* Nothing new to say on a publish that brought no reading. */
    if (mesh_ui_trends_append(&first, &one) != 0) {
        failure = "a publish with no new reading should write nothing";
        goto cleanup;
    }

    /* Session two: a fresh log state and a fresh history, on a cold boot's clock. */
    struct mesh_ui_trends second;
    if (mesh_ui_trends_init(&second, dir) != 0) {
        failure = "the second run could not open the same directory";
        goto cleanup;
    }
    struct mesh_ui_history two;
    trends_history(&two);
    const uint32_t new_clock = 30U * 1000U;

    if (mesh_ui_trends_restore(&second, 0x1234U, &two, new_clock) != 8) {
        failure = "every saved reading should come back";
        goto cleanup;
    }
    const int32_t levels[] = {90, 85, 80, 75};
    const int32_t degrees[] = {200, 201, 202, 203};
    if (!trends_values(&two, 0x1234U, MESH_UI_HISTORY_BATTERY, levels, 4U)) {
        failure = "the restored battery trend should be the one that was saved";
        goto cleanup;
    }
    if (!trends_values(&two, 0x1234U, MESH_UI_HISTORY_TEMPERATURE, degrees, 4U)) {
        failure = "the restored temperature trend should be the one that was saved";
        goto cleanup;
    }
    /* Four readings half an hour apart is a line, which is the whole point: the detail screen
       offers a chart off a drawable segment rather than off a sample. */
    if (!inkcell_series_has_segment(
            mesh_ui_history_series(&two, 0x1234U, MESH_UI_HISTORY_BATTERY))) {
        failure = "a restored trend should be drawable before the radio says anything";
        goto cleanup;
    }

    /* And now the live clock, thirty seconds into this boot - far below every stamp the first
       session wrote. */
    mesh_ui_history_note_battery(&two, new_clock + 1000U, 0x1234U, 70U);
    if (trends_count(&two, 0x1234U, MESH_UI_HISTORY_BATTERY) != 5U) {
        failure = "a reading on a lower clock must not empty the restored series";
        goto cleanup;
    }
    if (!inkcell_series_starts_segment(
            mesh_ui_history_series(&two, 0x1234U, MESH_UI_HISTORY_BATTERY), 4U)) {
        failure = "the first reading after a restart starts a segment of its own";
        goto cleanup;
    }
    /* A second live reading continues it, so this session draws a line of its own. */
    mesh_ui_history_note_battery(&two, new_clock + 1000U + MESH_UI_HISTORY_NODE_REPORT_MS, 0x1234U,
                                 68U);
    if (inkcell_series_starts_segment(
            mesh_ui_history_series(&two, 0x1234U, MESH_UI_HISTORY_BATTERY), 5U)) {
        failure = "a punctual reading after the seam continues the live segment";
        goto cleanup;
    }

cleanup:
    (void)mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Restoring a node this run has already heard from does not draw a seam through the session.
 *
 * The other half of the restart case and the one that is easy to get backwards. The log holds
 * this run's readings too, so the card's copy has to be laid down ending exactly where the live
 * one does - a restore that placed it a gap behind the clock would break the line at the moment
 * the user pressed into the screen, which is the one moment nothing happened.
 */
MESH_TEST_CASE(ui_trends_restore_continues_a_live_session, unit) {
    const char *failure = NULL;
    struct mesh_ui_trends trends;
    char dir[64];
    MESH_TEST_FAIL_IF(!trends_open(&trends, dir, sizeof dir), "could not open a trend log");

    struct mesh_ui_history history;
    trends_history(&history);
    const uint32_t clock = 60U * 1000U;
    for (uint32_t i = 0U; i < 3U; ++i) {
        mesh_ui_history_note_battery(&history, clock + i * MESH_UI_HISTORY_NODE_REPORT_MS, 0x77U,
                                     (uint8_t)(50U + i));
        (void)mesh_ui_trends_append(&trends, &history);
    }

    if (mesh_ui_trends_restore(&trends, 0x77U, &history, clock + 3U * 60U * 1000U) != 3) {
        failure = "the readings this run took should come back off the card";
        goto cleanup;
    }
    const int32_t levels[] = {50, 51, 52};
    if (!trends_values(&history, 0x77U, MESH_UI_HISTORY_BATTERY, levels, 3U)) {
        failure = "a restore must replace the live trend rather than double it";
        goto cleanup;
    }
    /* No seam anywhere in it: nothing was interrupted. */
    for (uint32_t i = 1U; i < 3U; ++i) {
        if (inkcell_series_starts_segment(
                mesh_ui_history_series(&history, 0x77U, MESH_UI_HISTORY_BATTERY), i)) {
            failure = "a restore over an uninterrupted session must not break the line";
            goto cleanup;
        }
    }

    /* And the next publish writes nothing, because the restore said how far the file runs. */
    if (mesh_ui_trends_append(&trends, &history) != 0) {
        failure = "a restored reading must not be written back to the card";
        goto cleanup;
    }
    /* A reading taken after the restore still reaches it. */
    mesh_ui_history_note_battery(&history, clock + 3U * MESH_UI_HISTORY_NODE_REPORT_MS, 0x77U, 53U);
    if (mesh_ui_trends_append(&trends, &history) != 1) {
        failure = "a reading taken after a restore should still be written";
        goto cleanup;
    }

cleanup:
    (void)mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A break the source armed is part of what was watched, and comes back with the reading.
 *
 * A node that spends a while on external power reports punctually and reports something that is
 * not a level, so the silence is invisible to the clock - inkcell_series_break() is how the push
 * says so, and the format has to carry that or the restored trend would slope across an hour of
 * charge nobody measured.
 */
MESH_TEST_CASE(ui_trends_keep_a_break_the_clock_cannot_see, unit) {
    const char *failure = NULL;
    struct mesh_ui_trends trends;
    char dir[64];
    MESH_TEST_FAIL_IF(!trends_open(&trends, dir, sizeof dir), "could not open a trend log");

    struct mesh_ui_history history;
    trends_history(&history);
    const uint32_t clock = 60U * 1000U;
    mesh_ui_history_note_battery(&history, clock, 0x99U, 80U);
    /* 101 is the firmware's "running off external power": refused as a reading, and the series
       is told that whatever comes next does not continue what came before. */
    mesh_ui_history_note_battery(&history, clock + MESH_UI_HISTORY_NODE_REPORT_MS, 0x99U, 101U);
    mesh_ui_history_note_battery(&history, clock + 2U * MESH_UI_HISTORY_NODE_REPORT_MS, 0x99U, 95U);
    (void)mesh_ui_trends_append(&trends, &history);

    struct mesh_ui_history next;
    trends_history(&next);
    struct mesh_ui_trends reopened;
    if (mesh_ui_trends_init(&reopened, dir) != 0) {
        failure = "the second run could not open the same directory";
        goto cleanup;
    }
    if (mesh_ui_trends_restore(&reopened, 0x99U, &next, 5000U) != 2) {
        failure = "both readings either side of the charge should come back";
        goto cleanup;
    }
    if (!inkcell_series_starts_segment(
            mesh_ui_history_series(&next, 0x99U, MESH_UI_HISTORY_BATTERY), 1U)) {
        failure = "the reading after external power must not continue the one before it";
        goto cleanup;
    }

cleanup:
    (void)mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A run that appends to a file it did not open puts the seam in the file.
 *
 * The append path's half of the restart, and the one a restore never reaches: a node heard in
 * this run before anybody opened its detail screen has its reading written straight onto an
 * earlier run's chain. How long the client was not running is unmeasurable, so that first record
 * carries the gap and says it continues nothing - and the question behind it is asked of the
 * open descriptor's size rather than of the path, so an empty file left by an open that got no
 * further reads as a file with nothing to continue.
 */
MESH_TEST_CASE(ui_trends_append_writes_the_restart_seam, unit) {
    const char *failure = NULL;
    struct mesh_ui_trends first;
    char dir[64];
    MESH_TEST_FAIL_IF(!trends_open(&first, dir, sizeof dir), "could not open a trend log");

    struct mesh_ui_history one;
    trends_history(&one);
    const uint32_t old_clock = 4U * 60U * 60U * 1000U;
    for (uint32_t i = 0U; i < 2U; ++i) {
        mesh_ui_history_note_battery(&one, old_clock + i * MESH_UI_HISTORY_NODE_REPORT_MS, 0x321U,
                                     (uint8_t)(40U + i));
        (void)mesh_ui_trends_append(&first, &one);
    }

    /* A second run that hears from the node and writes, with nothing having opened its detail. */
    struct mesh_ui_trends second;
    struct mesh_ui_history two;
    if (mesh_ui_trends_init(&second, dir) != 0) {
        failure = "the second run could not open the same directory";
        goto cleanup;
    }
    trends_history(&two);
    mesh_ui_history_note_battery(&two, 20U * 1000U, 0x321U, 30U);
    if (mesh_ui_trends_append(&second, &two) != 1) {
        failure = "the second run's reading should have been written";
        goto cleanup;
    }

    /* A third run reads all three back and finds the seam where the client was not running. */
    struct mesh_ui_trends third;
    struct mesh_ui_history three;
    if (mesh_ui_trends_init(&third, dir) != 0) {
        failure = "the third run could not open the same directory";
        goto cleanup;
    }
    trends_history(&three);
    if (mesh_ui_trends_restore(&third, 0x321U, &three, 5000U) != 3) {
        failure = "both runs' readings should come back";
        goto cleanup;
    }
    const int32_t levels[] = {40, 41, 30};
    if (!trends_values(&three, 0x321U, MESH_UI_HISTORY_BATTERY, levels, 3U)) {
        failure = "the readings should come back in the order they were written";
        goto cleanup;
    }
    const struct inkcell_series *series =
        mesh_ui_history_series(&three, 0x321U, MESH_UI_HISTORY_BATTERY);
    if (inkcell_series_starts_segment(series, 1U)) {
        failure = "two readings inside one run should still be one line";
        goto cleanup;
    }
    if (!inkcell_series_starts_segment(series, 2U)) {
        failure = "the first reading of a later run must not continue the run before it";
        goto cleanup;
    }

cleanup:
    (void)mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A reading below zero comes back below zero, and the pair that arrived together comes back
 * together.
 *
 * Three of the five readings this keeps are signed - an RSSI is always negative, an SNR usually
 * is, and a temperature is for half the year - so a field written as unsigned anywhere in the
 * chain is a trend that reads as the loudest signal the scale has. The second half is the one
 * the format is arranged around: SNR and RSSI are two measurements of one packet and the node
 * detail draws them against one axis, so they have to come back on one timeline rather than on
 * two that agree by accident.
 */
MESH_TEST_CASE(ui_trends_keep_a_signed_reading_and_its_pair, unit) {
    const char *failure = NULL;
    struct mesh_ui_trends trends;
    char dir[64];
    MESH_TEST_FAIL_IF(!trends_open(&trends, dir, sizeof dir), "could not open a trend log");

    struct mesh_ui_history history;
    trends_history(&history);
    const uint32_t clock = 60U * 1000U;
    for (uint32_t i = 0U; i < 3U; ++i) {
        mesh_ui_history_note_signal(&history, clock + i * MESH_UI_HISTORY_NODE_REPORT_MS, 0xABCU,
                                    -4 - (int32_t)i, true, -101 - (int32_t)i);
    }
    (void)mesh_ui_trends_append(&trends, &history);

    struct mesh_ui_history next;
    trends_history(&next);
    struct mesh_ui_trends reopened;
    if (mesh_ui_trends_init(&reopened, dir) != 0) {
        failure = "the second run could not open the same directory";
        goto cleanup;
    }
    if (mesh_ui_trends_restore(&reopened, 0xABCU, &next, 5000U) != 6) {
        failure = "both readings of all three packets should come back";
        goto cleanup;
    }
    const int32_t snrs[] = {-4, -5, -6};
    const int32_t rssis[] = {-101, -102, -103};
    if (!trends_values(&next, 0xABCU, MESH_UI_HISTORY_SNR, snrs, 3U)) {
        failure = "a negative ratio should come back negative";
        goto cleanup;
    }
    if (!trends_values(&next, 0xABCU, MESH_UI_HISTORY_RSSI, rssis, 3U)) {
        failure = "a negative strength should come back negative";
        goto cleanup;
    }
    /* One packet is one moment: the two series line up sample for sample. */
    for (uint32_t i = 0U; i < 3U; ++i) {
        const struct inkcell_sample *snr =
            inkcell_series_at(mesh_ui_history_series(&next, 0xABCU, MESH_UI_HISTORY_SNR), i);
        const struct inkcell_sample *rssi =
            inkcell_series_at(mesh_ui_history_series(&next, 0xABCU, MESH_UI_HISTORY_RSSI), i);
        if (snr == NULL || rssi == NULL || snr->time != rssi->time) {
            failure = "two readings off one packet should come back on one timeline";
            goto cleanup;
        }
    }

cleanup:
    (void)mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A file is capped by rewriting it, and what the rewrite keeps is still one chain.
 *
 * The delta of the first record kept refers to one that has just been dropped, so compaction has
 * to zero it - a chain that began with an inherited delta would push the whole restored trend
 * further back than it ever was, and with enough compactions past the window entirely.
 */
MESH_TEST_CASE(ui_trends_compaction_keeps_the_chain, unit) {
    const char *failure = NULL;
    struct mesh_ui_trends trends;
    char dir[64];
    char path[128];
    MESH_TEST_FAIL_IF(!trends_open(&trends, dir, sizeof dir), "could not open a trend log");
    snprintf(path, sizeof path, "%s/n00000042.trend", dir);

    struct mesh_ui_history history;
    trends_history(&history);
    /* Far more readings than the cap, at a cadence the series itself would not break at. */
    const uint32_t clock = 60U * 1000U;
    for (uint32_t i = 0U; i < 4000U; ++i) {
        mesh_ui_history_note_battery(&history, clock + i * 60U * 1000U, 0x42U,
                                     (uint8_t)(50U + (i % 50U)));
        (void)mesh_ui_trends_append(&trends, &history);
    }

    struct stat info;
    if (stat(path, &info) != 0) {
        failure = "the node's file should be on the card";
        goto cleanup;
    }
    if (info.st_size > (off_t)MESH_UI_TRENDS_FILE_MAX_BYTES) {
        failure = "a file over the cap should have been rewritten";
        goto cleanup;
    }

    struct mesh_ui_history next;
    trends_history(&next);
    struct mesh_ui_trends reopened;
    if (mesh_ui_trends_init(&reopened, dir) != 0) {
        failure = "the second run could not open the same directory";
        goto cleanup;
    }
    if (mesh_ui_trends_restore(&reopened, 0x42U, &next, 5000U) <= 0) {
        failure = "a compacted file should still restore";
        goto cleanup;
    }
    const struct inkcell_series *series =
        mesh_ui_history_series(&next, 0x42U, MESH_UI_HISTORY_BATTERY);
    if (series == NULL || series->count != INKCELL_SERIES_MAX) {
        failure = "a compacted file should still fill the series";
        goto cleanup;
    }
    /* The newest reading written is the newest reading back, which is what a cap on the *oldest*
       records means. */
    const struct inkcell_sample *newest = inkcell_series_newest(series);
    if (newest == NULL || newest->value != (int32_t)(50U + ((4000U - 1U) % 50U))) {
        failure = "the newest reading should survive a compaction";
        goto cleanup;
    }
    /* And one chain: a minute apart throughout, so nothing in it breaks. */
    for (uint32_t i = 1U; i < series->count; ++i) {
        if (inkcell_series_starts_segment(series, i)) {
            failure = "a compacted chain should still be one line";
            goto cleanup;
        }
    }

cleanup:
    (void)mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A reading this build has no name for still holds its place in the chain.
 *
 * The forward-compatibility case, and it costs more here than it would in the cache: every delta
 * is measured from the record above, so a record dropped on the way in takes its elapsed interval
 * with it and pulls everything after it earlier. The file below spends a whole node gap on a
 * reading this build does not keep - drop it and the two batteries either side are a minute
 * apart and drawn as one line, which is a two-hour silence claimed as evidence.
 */
MESH_TEST_CASE(ui_trends_keep_an_unknown_reading_in_the_chain, unit) {
    const char *failure = NULL;
    struct mesh_ui_trends trends;
    char dir[64];
    char path[128];
    MESH_TEST_FAIL_IF(!trends_open(&trends, dir, sizeof dir), "could not open a trend log");
    snprintf(path, sizeof path, "%s/n00000abc.trend", dir);

    /* Written by hand, because what is being tested is a record no build here can produce: a
       reading id past the end of this enum, as a newer client would have written. */
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        failure = "could not write a log by hand";
        goto cleanup;
    }
    fprintf(file, "trend=%u,0,50,0\n", (unsigned)MESH_UI_HISTORY_BATTERY);
    fprintf(file, "trend=%u,%u,5,0\n", (unsigned)MESH_UI_HISTORY_READING_COUNT + 7U,
            (unsigned)MESH_UI_HISTORY_NODE_GAP_MS);
    fprintf(file, "trend=%u,60000,49,0\n", (unsigned)MESH_UI_HISTORY_BATTERY);
    fclose(file);

    struct mesh_ui_history history;
    trends_history(&history);
    /* Two restored, not three: the third record is kept by the reader and has no series here. */
    if (mesh_ui_trends_restore(&trends, 0xABCU, &history, 5000U) != 2) {
        failure = "only the readings this build knows should be restored";
        goto cleanup;
    }
    const int32_t levels[] = {50, 49};
    if (!trends_values(&history, 0xABCU, MESH_UI_HISTORY_BATTERY, levels, 2U)) {
        failure = "the readings this build knows should come back as they were";
        goto cleanup;
    }
    if (!inkcell_series_starts_segment(
            mesh_ui_history_series(&history, 0xABCU, MESH_UI_HISTORY_BATTERY), 1U)) {
        failure = "the interval an unknown record spent must stay in the chain";
        goto cleanup;
    }

cleanup:
    (void)mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A clock that wraps starts a new chain rather than stopping the log.
 *
 * The history's timeline is a uint32 of milliseconds shifted up by MESH_UI_HISTORY_EPOCH_MS, so
 * 41 days of continuous running reaches the end of it. inkcell_series_push() handles that by
 * emptying what it holds - the readings are still true and when they were taken is not - but a
 * writer whose high-water mark stayed up near the top of the range would read every reading
 * after the wrap as one it had already written, and go quiet for the rest of the run.
 */
MESH_TEST_CASE(ui_trends_survive_a_clock_that_wraps, unit) {
    const char *failure = NULL;
    struct mesh_ui_trends trends;
    char dir[64];
    MESH_TEST_FAIL_IF(!trends_open(&trends, dir, sizeof dir), "could not open a trend log");

    struct mesh_ui_history history;
    trends_history(&history);
    /* A caller's clock chosen so that the history's own stamp lands just under the top. */
    const uint32_t before_wrap = 0xFFFFFFFFU - MESH_UI_HISTORY_EPOCH_MS - 120000U;
    mesh_ui_history_note_battery(&history, before_wrap, 0xD00DU, 60U);
    if (mesh_ui_trends_append(&trends, &history) != 1) {
        failure = "the reading before the wrap should have been written";
        goto cleanup;
    }

    /* And one just past it: the stamp wraps, so the series is emptied and starts again low. */
    const uint32_t after_wrap = before_wrap + 240000U;
    mesh_ui_history_note_battery(&history, after_wrap, 0xD00DU, 59U);
    if (trends_count(&history, 0xD00DU, MESH_UI_HISTORY_BATTERY) != 1U) {
        failure = "a wrapped clock should have emptied the series it holds";
        goto cleanup;
    }
    if (mesh_ui_trends_append(&trends, &history) != 1) {
        failure = "a reading after the wrap must still reach the card";
        goto cleanup;
    }
    /* And the one after that, so the log is not merely unstuck for a single reading. */
    mesh_ui_history_note_battery(&history, after_wrap + 60000U, 0xD00DU, 58U);
    if (mesh_ui_trends_append(&trends, &history) != 1) {
        failure = "the log should keep writing once the chain has started again";
        goto cleanup;
    }

cleanup:
    (void)mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A different radio is a different mesh, so the logs go with it.
 *
 * Node numbers are the mesh's rather than the radio's, and two meshes can hand out the same one -
 * so a trend kept across a swap would draw one node's battery as another's. The store's history
 * is dropped for exactly this; the card has to be told the same thing, and the first radio a run
 * sees is not a swap.
 */
MESH_TEST_CASE(ui_trends_forgotten_with_the_radio, unit) {
    const char *failure = NULL;
    struct mesh_ui_trends trends;
    char dir[64];
    char path[128];
    MESH_TEST_FAIL_IF(!trends_open(&trends, dir, sizeof dir), "could not open a trend log");
    snprintf(path, sizeof path, "%s/n00000501.trend", dir);

    if (mesh_ui_trends_note_radio(&trends, 0xAAAAU)) {
        failure = "the first radio a run sees is not a swap";
        goto cleanup;
    }
    struct mesh_ui_history history;
    trends_history(&history);
    mesh_ui_history_note_battery(&history, 60U * 1000U, 0x501U, 44U);
    (void)mesh_ui_trends_append(&trends, &history);
    if (access(path, F_OK) != 0) {
        failure = "the node's file should be on the card";
        goto cleanup;
    }

    if (mesh_ui_trends_note_radio(&trends, 0xAAAAU)) {
        failure = "the same radio again is not a swap";
        goto cleanup;
    }
    if (!mesh_ui_trends_note_radio(&trends, 0xBBBBU)) {
        failure = "a different radio should drop what was kept for the last one";
        goto cleanup;
    }
    if (access(path, F_OK) == 0) {
        failure = "a swapped-out radio's trends should be gone from the card";
        goto cleanup;
    }

    /* And the chain is forgotten with them, so the first append after a swap is a fresh file
       rather than a delta measured from a reading that is no longer there. */
    struct mesh_ui_history after;
    trends_history(&after);
    mesh_ui_history_note_battery(&after, 60U * 1000U, 0x501U, 12U);
    if (mesh_ui_trends_append(&trends, &after) != 1) {
        failure = "the new radio's first reading should be written";
        goto cleanup;
    }

cleanup:
    (void)mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A client with nowhere to write is still a client.
 *
 * Every entry point takes a disabled log and reports success for it, exactly as the archive
 * does: a Brick with a full or read-only card keeps its live trend, and what it loses is only
 * the part of it that would have outlived this run.
 */
MESH_TEST_CASE(ui_trends_tolerate_a_disabled_log, unit) {
    struct mesh_ui_trends trends;
    MESH_TEST_FAIL_IF(mesh_ui_trends_init(&trends, "") == 0,
                      "an empty directory should not open a trend log");

    struct mesh_ui_history history;
    trends_history(&history);
    mesh_ui_history_note_battery(&history, 1000U, 0x11U, 50U);

    MESH_TEST_FAIL_IF(mesh_ui_trends_append(&trends, &history) != 0,
                      "a disabled log should write nothing and say so");
    MESH_TEST_FAIL_IF(mesh_ui_trends_restore(&trends, 0x11U, &history, 2000U) != 0,
                      "a disabled log should restore nothing and say so");
    MESH_TEST_FAIL_IF(mesh_ui_trends_forget(&trends) != 0, "a disabled log has nothing to drop");
    /* And the live trend is untouched, which is the point of all of the above. */
    MESH_TEST_FAIL_IF(trends_count(&history, 0x11U, MESH_UI_HISTORY_BATTERY) != 1U,
                      "a disabled log must not cost the live trend anything");
    record_success(test_name);
}

/*
 * A node the history has let go still has its trend on the card.
 *
 * MESH_UI_HISTORY_NODES is a screen budget, and the log is not bound by it: a node evicted from
 * the table to make room for a busier one keeps its file, and opening its detail reads it back.
 * That is the one thing persistence buys beyond surviving a restart.
 */
MESH_TEST_CASE(ui_trends_outlive_an_eviction, unit) {
    const char *failure = NULL;
    struct mesh_ui_trends trends;
    char dir[64];
    MESH_TEST_FAIL_IF(!trends_open(&trends, dir, sizeof dir), "could not open a trend log");

    struct mesh_ui_history history;
    trends_history(&history);
    const uint32_t clock = 60U * 1000U;
    for (uint32_t i = 0U; i < 2U; ++i) {
        mesh_ui_history_note_battery(&history, clock + i * MESH_UI_HISTORY_NODE_REPORT_MS, 0x1U,
                                     (uint8_t)(60U + i));
        (void)mesh_ui_trends_append(&trends, &history);
    }

    /* Fill the table past its budget, every one of them heard more recently than the first. */
    for (uint32_t i = 0U; i < MESH_UI_HISTORY_NODES + 2U; ++i) {
        mesh_ui_history_note_battery(&history, clock + 10U * MESH_UI_HISTORY_NODE_REPORT_MS + i,
                                     0x100U + i, 70U);
        (void)mesh_ui_trends_append(&trends, &history);
    }
    if (trends_count(&history, 0x1U, MESH_UI_HISTORY_BATTERY) != 0U) {
        failure = "the least recently heard node should have lost its slot";
        goto cleanup;
    }

    if (mesh_ui_trends_restore(&trends, 0x1U, &history, clock + 11U * 60U * 1000U) != 2) {
        failure = "an evicted node's trend should still be on the card";
        goto cleanup;
    }
    const int32_t levels[] = {60, 61};
    if (!trends_values(&history, 0x1U, MESH_UI_HISTORY_BATTERY, levels, 2U)) {
        failure = "the readings an evicted node had should come back as they were";
        goto cleanup;
    }

cleanup:
    (void)mesh_test_remove_tree(dir);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
