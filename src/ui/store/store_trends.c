#define _POSIX_C_SOURCE 200809L

/*
 * The per-node trend log on the card.
 *
 * What this is for and the three decisions behind its format are in
 * include/mesh/ui/store_trends.h; this file is the mechanics. It sits beside store_archive.c in
 * the store group and is deliberately its mirror - the same open-write-close append, the same
 * compaction off one size check, the same rewrite through a temporary - over a record that is
 * four numbers instead of five lines. The one place it is not the archive's twin is that every
 * question it asks the filesystem is asked of an open descriptor rather than of a path, because
 * a file here is opened for append and then measured; see mesh_ui_trends_append().
 *
 * Every entry point tolerates a disabled log - one whose directory could not be made - and
 * reports success for it. A Brick with a full or read-only card is still a client, and what
 * degrades is only the part of a trend that would have outlived this run.
 */

#include "mesh/ui/store_trends.h"

#include "inkwell/base/array.h"
#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "mesh/utils/file.h"

#include "inkstand/persist/fields.h"
#include "mesh/ui/store_keys.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- naming a node -------------------------------------------------------------------------- */

/* The suffix every file here carries, so that a wipe can tell its own files from anything else
   that ends up in the directory. */
#define MESH_UI_TRENDS_SUFFIX ".trend"

/* One line of the file. Far shorter than the cache's, because a record is four numbers with no
   text in it at all - but the same buffer size, so that a hand-edited file with a long line in
   it is skipped the way the other two readers skip one rather than being split into two. */
#define MESH_UI_TRENDS_LINE_MAX 1280U

/* The most records one publish can have to write for one node: every reading this history keeps,
   each with a full ring behind it. What that costs is the gather buffer below. */
#define MESH_UI_TRENDS_BATCH_MAX ((MESH_UI_HISTORY_READING_COUNT - 1U) * INKCELL_SERIES_MAX)

/*
 * One node's file, as `<dir>/n1a2b3c4d.trend`.
 *
 * Hex for the node because that is how every other surface in this client and in the firmware
 * spells one, and the same `n` prefix the archive gives a direct conversation - these are the
 * same kind of name for the same kind of thing. Nothing user-supplied reaches it: a node number
 * is a uint32_t printed by us, so there is no path traversal to defend against and no escaping
 * to do.
 */
static bool trends_path(const struct mesh_ui_trends *trends, uint32_t node_id, char *out,
                        size_t out_len) {
    if (trends == NULL || trends->dir[0] == '\0' || node_id == 0U || out == NULL) {
        return false;
    }
    const int written =
        snprintf(out, out_len, "%s/n%08x%s", trends->dir, node_id, MESH_UI_TRENDS_SUFFIX);
    return written > 0 && written < (int)out_len;
}

/* ---- the delta chain this run is holding ---------------------------------------------------- */

static struct mesh_ui_trends_node *trends_find(struct mesh_ui_trends *trends, uint32_t node_id) {
    for (uint32_t i = 0U; i < MESH_UI_TRENDS_NODES; ++i) {
        if (trends->nodes[i].node_id == node_id) {
            return &trends->nodes[i];
        }
    }
    return NULL;
}

/*
 * This node's chain, or a free slot taken for it - NULL only when there is no free slot.
 *
 * There is always one in practice, and that is what trends_prune() is for rather than an
 * eviction rule: the writer only ever appends for a node the history has a slot for, so the
 * table can only be as full as the history is, and this one is larger. A NULL here would cost a
 * node its high-water mark and so re-append its whole ring, which is why the slack exists rather
 * than an eviction that would quietly do it.
 */
static struct mesh_ui_trends_node *trends_slot(struct mesh_ui_trends *trends, uint32_t node_id) {
    struct mesh_ui_trends_node *held = trends_find(trends, node_id);
    if (held != NULL) {
        return held;
    }
    for (uint32_t i = 0U; i < MESH_UI_TRENDS_NODES; ++i) {
        if (trends->nodes[i].node_id == 0U) {
            memset(&trends->nodes[i], 0, sizeof trends->nodes[i]);
            trends->nodes[i].node_id = node_id;
            return &trends->nodes[i];
        }
    }
    return NULL;
}

/* Chains for nodes the history no longer holds, dropped. A node that comes back gets a seam
   record and a fresh chain, which is the truth about it: nothing was watching it in between. */
static void trends_prune(struct mesh_ui_trends *trends, const struct mesh_ui_history *history) {
    for (uint32_t i = 0U; i < MESH_UI_TRENDS_NODES; ++i) {
        struct mesh_ui_trends_node *state = &trends->nodes[i];
        if (state->node_id == 0U) {
            continue;
        }
        bool live = false;
        for (uint32_t j = 0U; j < MESH_UI_HISTORY_NODES && !live; ++j) {
            live = history->nodes[j].node_id == state->node_id;
        }
        if (!live) {
            memset(state, 0, sizeof *state);
        }
    }
}

/* ---- reading a file ------------------------------------------------------------------------- */

/* One record as it sits on the card: which reading, how long after the record above it, what it
   said, and whether it continues that record at all. */
struct trend_record {
    uint32_t delta;
    int32_t value;
    uint8_t reading;
    bool gap;
};

/*
 * Every record in one file, newest last, folded into a caller's buffer that holds the newest
 * `capacity` of them.
 *
 * A ring rather than a seek to the tail, for archive_read_file()'s reason: the file is capped, a
 * record is one short line, and a single forward pass with a fixed destination has no offset
 * arithmetic to get wrong on a file whose last record may be a torn write.
 *
 * What is dropped off the front is not counted, because nothing asks. The archive counts it so a
 * transcript can say there is more behind the top of it; a trend that reached further back than
 * the series can hold has nowhere to say so, and INKCELL_SERIES_MAX is the smaller bound anyway.
 */
struct trend_reader {
    struct trend_record *entries;
    uint32_t capacity;
    uint32_t next;
    bool wrapped;
};

static void trend_reader_take(struct trend_reader *reader, const struct trend_record *record) {
    reader->entries[reader->next] = *record;
    reader->next = (reader->next + 1U) % reader->capacity;
    if (reader->next == 0U) {
        reader->wrapped = true;
    }
}

/* The buffer in the order it was written, unwound from the ring. Returns how many it holds. */
static uint32_t trend_reader_finish(struct trend_reader *reader) {
    if (!reader->wrapped) {
        return reader->next;
    }
    /* `next` points at the oldest entry, so the buffer is rotated left by that much - as three
       reversals rather than with a scratch copy, exactly as archive_reader_finish() does it. */
    struct trend_record *entries = reader->entries;
    const uint32_t n = reader->capacity;
    const uint32_t k = reader->next;
    for (uint32_t lo = 0U, hi = k; lo + 1U < hi; ++lo, --hi) {
        const struct trend_record tmp = entries[lo];
        entries[lo] = entries[hi - 1U];
        entries[hi - 1U] = tmp;
    }
    for (uint32_t lo = k, hi = n; lo + 1U < hi; ++lo, --hi) {
        const struct trend_record tmp = entries[lo];
        entries[lo] = entries[hi - 1U];
        entries[hi - 1U] = tmp;
    }
    for (uint32_t lo = 0U, hi = n; lo + 1U < hi; ++lo, --hi) {
        const struct trend_record tmp = entries[lo];
        entries[lo] = entries[hi - 1U];
        entries[hi - 1U] = tmp;
    }
    return n;
}

/*
 * A record's value, read through the field list that is the format's other half.
 *
 * Only the four fields are checked here, and a reading this build has no name for is *kept*.
 * That is store_keys.def's forward-compatibility rule applied to a format where skipping a line
 * costs more than the line: every delta is measured from the record above, so a record dropped
 * on the way in takes its elapsed interval with it and pulls everything after it earlier - which
 * is a real silence redrawn as a connected line. Kept, its interval stays in the chain, and a
 * compaction writes it back for the build that does know what it is.
 *
 * Which reading it is stays the restore's question, asked at the one place a record becomes a
 * sample. A line whose fields do not parse at all is the other case and is dropped, because a
 * torn line has no interval to contribute either.
 */
static bool trend_read_value(const char *value, struct trend_record *out) {
    memset(out, 0, sizeof *out);
    const struct inkstand_field fields[] = {
        INKSTAND_FIELD(&out->reading),
        INKSTAND_FIELD(&out->delta),
        INKSTAND_FIELD(&out->value),
        INKSTAND_FIELD(&out->gap),
    };
    return inkstand_fields_read(value, fields, INKWELL_ARRAY_LEN(fields)) ==
           INKWELL_ARRAY_LEN(fields);
}

/* Whether this build has a series to put the record on. */
static bool trend_reading_known(uint8_t reading) {
    return reading > (uint8_t)MESH_UI_HISTORY_NONE &&
           reading < (uint8_t)MESH_UI_HISTORY_READING_COUNT;
}

/*
 * One pass over a node's file into `entries`.
 *
 * The line loop is store_file.c's and store_archive.c's, deliberately: same length, same split
 * on the first '=', same unescape, same tolerance of a comment or a blank. Returns -ENOENT when
 * there is no such file, which every caller treats as a node nothing has been kept for rather
 * than as a failure.
 */
static int trends_read_file(const char *path, struct trend_record *entries, uint32_t capacity,
                            uint32_t *out_count) {
    if (entries == NULL || capacity == 0U || out_count == NULL) {
        return -EINVAL;
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -errno;
    }

    struct trend_reader reader;
    memset(&reader, 0, sizeof reader);
    reader.entries = entries;
    reader.capacity = capacity;

    char line[MESH_UI_TRENDS_LINE_MAX];
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }
        *equals = '\0';
        char *value = equals + 1;
        mesh_ui_store_unescape_value(value);
        if (mesh_ui_store_key_lookup(line, NULL, NULL) != MESH_UI_STORE_KEY_TREND) {
            continue;
        }
        struct trend_record record;
        if (trend_read_value(value, &record)) {
            trend_reader_take(&reader, &record);
        }
    }

    fclose(file);
    *out_count = trend_reader_finish(&reader);
    return 0;
}

/* ---- writing a file ------------------------------------------------------------------------- */

static void trend_write_record(FILE *file, const struct trend_record *record) {
    mesh_ui_store_write(file, MESH_UI_STORE_KEY_TREND, "%u,%u,%d,%u", (unsigned)record->reading,
                        (unsigned)record->delta, (int)record->value, record->gap ? 1U : 0U);
}

/*
 * Cuts a node's file back to its newest records.
 *
 * Called only when the append has just established that the file is over the cap, and it is the
 * append that asks - off an fstat() on the handle it wrote through, not a stat() on the name.
 * The difference is not tidiness: a size read off the path and then acted on by reopening it is
 * a check on one file and a write to whatever wears that name a moment later, which is the
 * time-of-check race CodeQL names. Nothing here asks the filesystem a question about a path and
 * then acts on the answer; the read below simply opens what is there.
 *
 * The read only happens on the append that actually trips the threshold, which for a node
 * reporting every half hour is once every few weeks.
 *
 * Through a temporary and a rename, as the archive's rewrite is and unlike the cache's: what is
 * here is history nothing else holds, so an interrupted rewrite has to leave the old file rather
 * than half of a new one.
 *
 * The first kept record's delta is zeroed, because what it was measured from has just been
 * dropped. Everything else about the chain is untouched - a compaction changes where the file
 * *starts*, not what it says.
 */
static void trends_compact(const char *path) {
    struct trend_record keep[MESH_UI_TRENDS_MAX_RECORDS];
    uint32_t count = 0U;
    if (trends_read_file(path, keep, MESH_UI_TRENDS_MAX_RECORDS, &count) != 0 || count == 0U) {
        return;
    }
    keep[0].delta = 0U;

    char temp[sizeof(((struct mesh_ui_trends *)0)->dir) + 64];
    const int named = snprintf(temp, sizeof temp, "%s.tmp", path);
    if (named <= 0 || named >= (int)sizeof temp) {
        return;
    }
    FILE *file = fopen(temp, "w");
    if (file == NULL) {
        return;
    }
    for (uint32_t i = 0U; i < count; ++i) {
        trend_write_record(file, &keep[i]);
    }
    int result = ferror(file) ? -EIO : 0;
    if (fclose(file) != 0) {
        result = -errno;
    }
    if (result == 0 && rename(temp, path) != 0) {
        result = -errno;
    }
    if (result != 0) {
        (void)unlink(temp);
        inkwell_log_warn("ui", "Could not compact trend log %s: %d", path, result);
        return;
    }
    inkwell_log_info("ui", "Compacted trend log %s to %u readings", path, (unsigned)count);
}

/* ---- the public half ------------------------------------------------------------------------ */

int mesh_ui_trends_init(struct mesh_ui_trends *trends, const char *dir) {
    if (trends == NULL) {
        return -EINVAL;
    }
    memset(trends, 0, sizeof *trends);
    if (dir == NULL || dir[0] == '\0') {
        return -EINVAL;
    }
    const int failed = mesh_file_mkdir(dir);
    if (failed < 0 && failed != -EEXIST) {
        inkwell_log_warn("ui", "Trend log unavailable at %s: %d", dir, failed);
        return failed;
    }
    inkwell_str_copy(trends->dir, sizeof trends->dir, dir);
    /* Truncation would put the files somewhere other than where the caller asked, so it disables
       the log rather than writing to a shortened path. */
    if (strcmp(trends->dir, dir) != 0) {
        trends->dir[0] = '\0';
        return -ENAMETOOLONG;
    }
    return 0;
}

bool mesh_ui_trends_note_radio(struct mesh_ui_trends *trends, uint32_t roster_owner) {
    if (trends == NULL || roster_owner == 0U) {
        return false;
    }
    if (!trends->has_owner) {
        trends->roster_owner = roster_owner;
        trends->has_owner = true;
        return false;
    }
    if (trends->roster_owner == roster_owner) {
        return false;
    }
    trends->roster_owner = roster_owner;
    const int dropped = mesh_ui_trends_forget(trends);
    trends->has_owner = true;
    if (dropped > 0) {
        inkwell_log_info("ui", "Dropped %d trend log(s) for the radio that was swapped out",
                         dropped);
    }
    return true;
}

/* One sample waiting to be written, with the time it was taken on the history's own timeline. */
struct trend_pending {
    uint32_t time;
    int32_t value;
    uint8_t reading;
    bool gap;
};

/*
 * Everything about one node that is later than what has been written, in the order it was taken.
 *
 * The order is the reason this gathers before it writes: the deltas run over the file as a whole,
 * so a temperature and the battery that arrived with it have to go down in the order their stamps
 * put them or the chain says one happened before the other. An insertion sort over at most a few
 * entries in the ordinary case, and at most MESH_UI_TRENDS_BATCH_MAX on the first publish after a
 * node's chain was dropped.
 */
static uint32_t trends_gather(const struct mesh_ui_history *history,
                              const struct mesh_ui_trends_node *state, uint32_t node_id,
                              struct trend_pending *out) {
    uint32_t count = 0U;
    for (uint32_t reading = MESH_UI_HISTORY_NONE + 1U; reading < MESH_UI_HISTORY_READING_COUNT;
         ++reading) {
        const struct inkcell_series *series =
            mesh_ui_history_series(history, node_id, (enum mesh_ui_history_reading)reading);
        if (series == NULL) {
            continue;
        }
        for (uint32_t i = 0U; i < series->count; ++i) {
            const struct inkcell_sample *sample = inkcell_series_at(series, i);
            if (sample == NULL || (state->written && sample->time <= state->written_at)) {
                continue;
            }
            if (count >= MESH_UI_TRENDS_BATCH_MAX) {
                break;
            }
            struct trend_pending entry = {.time = sample->time,
                                          .value = sample->value,
                                          .reading = (uint8_t)reading,
                                          .gap = sample->gap};
            uint32_t at = count++;
            while (at > 0U && out[at - 1U].time > entry.time) {
                out[at] = out[at - 1U];
                --at;
            }
            out[at] = entry;
        }
    }
    return count;
}

int mesh_ui_trends_append(struct mesh_ui_trends *trends, const struct mesh_ui_history *history) {
    if (trends == NULL || history == NULL) {
        return -EINVAL;
    }
    if (trends->dir[0] == '\0') {
        return 0;
    }
    trends_prune(trends, history);

    int written = 0;
    for (uint32_t i = 0U; i < MESH_UI_HISTORY_NODES; ++i) {
        const uint32_t node_id = history->nodes[i].node_id;
        if (node_id == 0U) {
            continue;
        }
        struct mesh_ui_trends_node *state = trends_slot(trends, node_id);
        if (state == NULL) {
            continue;
        }
        /* The ordinary publish, answered without walking a ring: the loop turns for a message, a
           position, a tick, and a node's telemetry is half an hour apart. One reading's newest
           stamp against the high-water mark says there is nothing to write far more cheaply than
           five series of two dozen samples each would. */
        uint32_t newest = 0U;
        const bool held = mesh_ui_history_node_newest(history, node_id, &newest);
        if (state->written) {
            if (!held) {
                continue;
            }
            if (newest < state->written_at) {
                /*
                 * The clock went backwards, which on a uint32 of milliseconds is the wrap 41 days
                 * of running reaches (MESH_UI_HISTORY_EPOCH_MS). inkcell_series_push() has
                 * already emptied what it holds, so this node's trend is a fresh start - and a
                 * high-water mark left up near the top of the range would read every reading
                 * after the wrap as one already written, and quietly stop persisting the node
                 * for the rest of the run.
                 *
                 * So the chain starts again, which also writes the seam: how much time the wrap
                 * covered is precisely what a wrapped clock cannot say.
                 */
                state->written = false;
                state->written_at = 0U;
            } else if (newest == state->written_at) {
                continue;
            }
        }

        struct trend_pending pending[MESH_UI_TRENDS_BATCH_MAX];
        const uint32_t count = trends_gather(history, state, node_id, pending);
        if (count == 0U) {
            continue;
        }

        char path[sizeof trends->dir + 32];
        if (!trends_path(trends, node_id, path, sizeof path)) {
            continue;
        }
        FILE *file = fopen(path, "a");
        if (file == NULL) {
            inkwell_log_warn("ui", "Could not append to trend log %s: %d", path, -errno);
            continue;
        }
        /*
         * Whether an earlier run already wrote this file, asked of the descriptor rather than of
         * the path.
         *
         * The question is "is the record I am about to write the first in this file", and the
         * answer has to be about the file this handle holds: an access() or a stat() on the name
         * beforehand is a check on one file and an append to whatever wears that name a moment
         * later. Opening first and measuring what was opened has no such gap, and it answers
         * more precisely besides - a file left empty by an open that got no further is a file
         * with nothing to continue, which the name could not have told us.
         *
         * A file with something in it means the run that wrote it has ended, so the first record
         * of this one carries the seam rather than a measurement.
         */
        struct stat before;
        const bool resumed =
            !state->written && fstat(fileno(file), &before) == 0 && before.st_size > 0;

        /*
         * The chain moves on a copy, and the copy reaches `state` only once the records are on
         * the card.
         *
         * Advancing as each record is written would mean a card that filled, or was pulled,
         * between the last fputs and the fclose left every attempted reading marked as written -
         * so the next publish would filter them out and the file would be missing samples the
         * history had. That matters more here than it would elsewhere, because a restore
         * *replaces* the live trend with the log: the gap would not merely fail to be saved, it
         * would be read back over the readings that were still in RAM. archive_remember() makes
         * the same statement for the same reason.
         */
        bool chain_open = state->written;
        uint32_t chain_at = state->written_at;
        uint32_t records = 0U;
        for (uint32_t j = 0U; j < count; ++j) {
            struct trend_record record = {
                .reading = pending[j].reading, .value = pending[j].value, .gap = pending[j].gap};
            if (chain_open) {
                record.delta = pending[j].time - chain_at;
            } else {
                /* How long the client was not running is unknowable on a Brick, so the seam is
                   the shortest silence that is already a break - and the record says so itself
                   rather than leaving a reader to work it out from the length. */
                record.delta = resumed ? MESH_UI_HISTORY_NODE_GAP_MS : 0U;
                record.gap = record.gap || resumed;
            }
            trend_write_record(file, &record);
            chain_open = true;
            chain_at = pending[j].time;
            ++records;
        }
        /* And the size the cap is measured against, off the same handle and before it is let go
           - for the reason the question above is asked that way. */
        int result = ferror(file) ? -EIO : 0;
        struct stat after;
        off_t size = 0;
        if (result == 0 && fflush(file) == 0 && fstat(fileno(file), &after) == 0) {
            size = after.st_size;
        }
        if (fclose(file) != 0) {
            result = -errno;
        }
        if (result != 0) {
            /* `state` is untouched, so these readings are written again on the next publish. */
            inkwell_log_warn("ui", "Could not append to trend log %s: %d", path, result);
            continue;
        }
        state->written = chain_open;
        state->written_at = chain_at;
        written += (int)records;
        if (size > (off_t)MESH_UI_TRENDS_FILE_MAX_BYTES) {
            trends_compact(path);
        }
    }

    return written;
}

int mesh_ui_trends_restore(struct mesh_ui_trends *trends, uint32_t node_id,
                           struct mesh_ui_history *history, uint32_t now_ms) {
    if (trends == NULL || history == NULL || node_id == 0U) {
        return -EINVAL;
    }
    char path[sizeof trends->dir + 32];
    if (!trends_path(trends, node_id, path, sizeof path)) {
        return 0; /* the log is disabled: the live history is all there is, and it stands */
    }

    struct trend_record records[MESH_UI_TRENDS_MAX_RECORDS];
    uint32_t count = 0U;
    const int result = trends_read_file(path, records, MESH_UI_TRENDS_MAX_RECORDS, &count);
    if (result == -ENOENT) {
        return 0; /* a node nothing has ever been kept about */
    }
    if (result != 0) {
        return result;
    }
    if (count == 0U) {
        return 0;
    }

    /*
     * The chain, added up - in 64 bits, because that is the sum this format cannot bound. Each
     * delta fits a uint32 and there are hundreds of them, so the total is free to exceed the
     * clock these readings are about to be put back onto; the window below is what brings it
     * back inside it.
     *
     * The first record's own delta is dropped rather than added: it was measured from whatever
     * came before it, which is either nothing at all or a record the ring above has let go.
     */
    uint64_t times[MESH_UI_TRENDS_MAX_RECORDS];
    times[0] = 0U;
    for (uint32_t i = 1U; i < count; ++i) {
        times[i] = times[i - 1U] + records[i].delta;
    }
    uint32_t first = 0U;
    while (first + 1U < count && times[count - 1U] - times[first] > MESH_UI_TRENDS_SPAN_MAX_MS) {
        ++first;
    }
    const uint32_t span = (uint32_t)(times[count - 1U] - times[first]);

    /*
     * Where the saved trend ends.
     *
     * This run's own newest reading about the node when there is one, because the log holds that
     * reading too - so laying the file down to end anywhere else would either draw a seam through
     * a session that was never interrupted or put the restored copy in front of the live clock.
     * A node that has said nothing this run gets the seam instead: a gap short of where a reading
     * arriving now would land, with the break armed below.
     */
    uint32_t end_ms = 0U;
    const bool continuous = mesh_ui_history_node_newest(history, node_id, &end_ms);
    if (!continuous) {
        end_ms = mesh_ui_history_stamp_now(history, now_ms) - MESH_UI_HISTORY_NODE_GAP_MS;
    }

    mesh_ui_history_restore_node_reset(history, node_id);
    uint32_t restored = 0U;
    for (uint32_t i = first; i < count; ++i) {
        /* A reading a newer build keeps and this one does not. Its interval is already in the
           times above, which is the whole reason the reader held on to it; what it has no series
           to go on is the value. */
        if (!trend_reading_known(records[i].reading)) {
            continue;
        }
        const uint32_t behind = span - (uint32_t)(times[i] - times[first]);
        mesh_ui_history_restore_node(history, node_id,
                                     (enum mesh_ui_history_reading)records[i].reading,
                                     end_ms - behind, records[i].value, records[i].gap);
        ++restored;
    }
    if (!continuous) {
        mesh_ui_history_resume_node(history, node_id);
    }

    /*
     * And the chain picks up from where the file now provably runs to.
     *
     * Without this the next append would see a slot full of readings it has no high-water mark
     * for and write every one of them back - the restore's own copy of the file, appended to the
     * file. It is the same statement archive_remember() makes and for the same reason: what is
     * on the card must not go onto the card again.
     */
    struct mesh_ui_trends_node *state = trends_slot(trends, node_id);
    if (state != NULL) {
        state->written = true;
        state->written_at = end_ms;
    }
    return (int)restored;
}

int mesh_ui_trends_forget(struct mesh_ui_trends *trends) {
    if (trends == NULL) {
        return -EINVAL;
    }
    memset(trends->nodes, 0, sizeof trends->nodes);
    if (trends->dir[0] == '\0') {
        return 0;
    }
    DIR *dir = opendir(trends->dir);
    if (dir == NULL) {
        return (errno == ENOENT) ? 0 : -errno;
    }
    int dropped = 0;
    const struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        /* Only what this writes, matched on the suffix rather than on the whole name: the
           directory is ours, but a `.tmp` left behind by an interrupted compaction is ours too
           and goes with the file it was going to replace. */
        const char *name = entry->d_name;
        const char *at = strstr(name, MESH_UI_TRENDS_SUFFIX);
        if (at == NULL) {
            continue;
        }
        const bool is_log = at[sizeof MESH_UI_TRENDS_SUFFIX - 1U] == '\0';
        const bool is_temp = strcmp(at, MESH_UI_TRENDS_SUFFIX ".tmp") == 0;
        if (!is_log && !is_temp) {
            continue;
        }
        char path[sizeof trends->dir + 256];
        const int written = snprintf(path, sizeof path, "%s/%s", trends->dir, name);
        if (written <= 0 || written >= (int)sizeof path) {
            continue;
        }
        if (unlink(path) == 0 && is_log) {
            ++dropped;
        }
    }
    closedir(dir);
    return dropped;
}
