#define _POSIX_C_SOURCE 200809L

/*
 * The per-node trend log on the card.
 *
 * What this is for and the three decisions behind its format are in
 * include/mesh/ui/store_trends.h; this file is the mechanics. It sits beside store_archive.c in
 * the store group and stands on the same journal (inkstand/persist/journal.h) - the same
 * open-write-close append, the same compaction off one size check, the same rewrite through a
 * temporary - over a record that is four numbers instead of five lines. It is the one of the two
 * that needs the journal's `resumed`: every record is measured from the one before it, so whether a
 * file was already there is a question about the chain, not the directory.
 *
 * Every entry point tolerates a disabled log - one whose directory could not be made - and
 * reports success for it. A Brick with a full or read-only card is still a client, and what
 * degrades is only the part of a trend that would have outlived this run.
 */

#include "mesh/ui/store_trends.h"

#include "inkwell/base/array.h"
#include "inkwell/base/log.h"

#include "inkstand/persist/fields.h"
#include "mesh/ui/store_keys.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ---- naming a node -------------------------------------------------------------------------- */

/* The suffix every file here carries, so that a wipe can tell its own files from anything else
   that ends up in the directory. The journal does the telling. */
#define MESH_UI_TRENDS_SUFFIX ".trend"

/* One line of the file. Far shorter than the cache's, because a record is four numbers with no
   text in it at all - but the same buffer size, so that a hand-edited file with a long line in
   it is skipped the way the other two readers skip one rather than being split into two. */
#define MESH_UI_TRENDS_LINE_MAX 1280U

/* The most records one publish can have to write for one node: every reading this history keeps,
   each with a full ring behind it. What that costs is the gather buffer below. */
#define MESH_UI_TRENDS_BATCH_MAX ((MESH_UI_HISTORY_READING_COUNT - 1U) * INKCELL_SERIES_MAX)

/* The longest subject name trends_subject() writes: `n` and eight hex digits. */
#define MESH_UI_TRENDS_SUBJECT_LEN 16U

/*
 * One node's subject in the journal, as `n1a2b3c4d` - so its file is `<dir>/n1a2b3c4d.trend`.
 *
 * Hex for the node because that is how every other surface in this client and in the firmware
 * spells one, and the same `n` prefix the archive gives a direct conversation - these are the
 * same kind of name for the same kind of thing. Nothing user-supplied reaches it: a node number
 * is a uint32_t printed by us, so there is no escaping to do.
 */
static bool trends_subject(const struct mesh_ui_trends *trends, uint32_t node_id, char *out,
                           size_t out_len) {
    if (trends == NULL || !inkstand_journal_enabled(&trends->journal) || node_id == 0U ||
        out == NULL) {
        return false;
    }
    const int written = snprintf(out, out_len, "n%08x", node_id);
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
 * `capacity` of them - the journal's ring, for the reason it gives: the file is capped, a record
 * is one short line, and a single forward pass with a fixed destination has no offset arithmetic
 * to get wrong on a file whose last record may be a torn write.
 *
 * What the ring drops off the front is counted but not asked for. The archive asks so a
 * transcript can say there is more behind the top of it; a trend that reached further back than
 * the series can hold has nowhere to say so, and INKCELL_SERIES_MAX is the smaller bound anyway.
 */
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
 * The line loop is the journal's, which is inkwell_record_read()'s - the cache's and the
 * archive's, deliberately: same length, same split on the first '=', same unescape, same tolerance
 * of a comment or a blank, and a torn final line that is never seen. Returns -ENOENT when there
 * is no such file, which every caller treats as a node nothing has been kept for rather than as a
 * failure.
 */
static void trends_read_line(void *context, const char *key, char *value) {
    if (mesh_ui_store_key_lookup(key, NULL, NULL) != MESH_UI_STORE_KEY_TREND) {
        return;
    }
    struct trend_record record;
    if (!trend_read_value(value, &record)) {
        return;
    }
    struct trend_record *slot = inkstand_journal_ring_push(context);
    if (slot != NULL) {
        *slot = record;
    }
}

static int trends_read_file(const struct mesh_ui_trends *trends, const char *subject,
                            struct trend_record *entries, uint32_t capacity, uint32_t *out_count) {
    if (entries == NULL || capacity == 0U || out_count == NULL) {
        return -EINVAL;
    }
    struct inkstand_journal_ring ring;
    inkstand_journal_ring_init(&ring, entries, sizeof *entries, capacity);

    char line[MESH_UI_TRENDS_LINE_MAX];
    const int result = inkstand_journal_read(&trends->journal, subject, line, sizeof line,
                                             trends_read_line, &ring);
    if (result != 0) {
        return result;
    }
    *out_count = inkstand_journal_ring_finish(&ring);
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
 * journal that measured it - off the stream it wrote through, not a stat() on the name. A size
 * read off the path and then acted on by reopening it is a check on one file and a write to
 * whatever wears that name a moment later, which is the time-of-check race CodeQL names.
 *
 * The read only happens on the append that actually trips the threshold, which for a node
 * reporting every half hour is once every few weeks.
 *
 * Through the journal's temporary, as the archive's rewrite is and unlike the cache's: what is
 * here is history nothing else holds, so an interrupted rewrite has to leave the old file rather
 * than half of a new one.
 *
 * The first kept record's delta is zeroed, because what it was measured from has just been
 * dropped. Everything else about the chain is untouched - a compaction changes where the file
 * *starts*, not what it says.
 */
struct trends_rewrite_context {
    const struct trend_record *records;
    uint32_t count;
};

static void trends_write_replacement(FILE *file, void *context) {
    const struct trends_rewrite_context *rewrite = context;
    for (uint32_t i = 0U; i < rewrite->count; ++i) {
        trend_write_record(file, &rewrite->records[i]);
    }
}

static void trends_compact(const struct mesh_ui_trends *trends, const char *subject) {
    struct trend_record keep[MESH_UI_TRENDS_MAX_RECORDS];
    uint32_t count = 0U;
    if (trends_read_file(trends, subject, keep, MESH_UI_TRENDS_MAX_RECORDS, &count) != 0 ||
        count == 0U) {
        return;
    }
    keep[0].delta = 0U;

    struct trends_rewrite_context context = {keep, count};
    const int result =
        inkstand_journal_replace(&trends->journal, subject, trends_write_replacement, &context);
    if (result != 0) {
        inkwell_log_warn("ui", "Could not compact trend log %s: %d", subject, result);
        return;
    }
    inkwell_log_info("ui", "Compacted trend log %s to %u readings", subject, (unsigned)count);
}

/* ---- the public half ------------------------------------------------------------------------ */

int mesh_ui_trends_init(struct mesh_ui_trends *trends, const char *dir) {
    if (trends == NULL) {
        return -EINVAL;
    }
    memset(trends, 0, sizeof *trends);
    const int result = inkstand_journal_init(&trends->journal, dir, MESH_UI_TRENDS_SUFFIX,
                                             MESH_UI_TRENDS_FILE_MAX_BYTES);
    if (result != 0 && result != -EINVAL) {
        inkwell_log_warn("ui", "Trend log unavailable at %s: %d", dir, result);
    }
    return result;
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

/*
 * One node's pending samples, written as a chain.
 *
 * The chain moves on a copy, and the copy reaches the node's state only once the records are on
 * the card.
 *
 * Advancing as each record is written would mean a card that filled, or was pulled, between the
 * last fputs and the fclose left every attempted reading marked as written - so the next publish
 * would filter them out and the file would be missing samples the history had. That matters more
 * here than it would elsewhere, because a restore *replaces* the live trend with the log: the gap
 * would not merely fail to be saved, it would be read back over the readings that were still in
 * RAM. archive_remember() makes the same statement for the same reason.
 */
struct trends_append_context {
    const struct trend_pending *pending;
    uint32_t count;
    /* The chain as it stands, advanced by the write. */
    bool chain_open;
    uint32_t chain_at;
    uint32_t records;
};

static void trends_write_append(FILE *file, bool resumed, void *context) {
    struct trends_append_context *append = context;
    /*
     * `resumed` is whether an earlier run already wrote this file, which the journal asks of the
     * stream it opened rather than of the name: an access() or a stat() beforehand is a check on
     * one file and an append to whatever wears that name a moment later, and a file left empty by
     * an open that got no further is a file with nothing to continue, which the name could not
     * have said.
     *
     * A file with something in it, and no chain this run, means the run that wrote it has ended -
     * so the first record of this one carries the seam rather than a measurement.
     */
    const bool seam = resumed && !append->chain_open;
    for (uint32_t j = 0U; j < append->count; ++j) {
        const struct trend_pending *pending = &append->pending[j];
        struct trend_record record = {
            .reading = pending->reading, .value = pending->value, .gap = pending->gap};
        if (append->chain_open) {
            record.delta = pending->time - append->chain_at;
        } else {
            /* How long the client was not running is unknowable on a Brick, so the seam is
               the shortest silence that is already a break - and the record says so itself
               rather than leaving a reader to work it out from the length. */
            record.delta = seam ? MESH_UI_HISTORY_NODE_GAP_MS : 0U;
            record.gap = record.gap || seam;
        }
        trend_write_record(file, &record);
        append->chain_open = true;
        append->chain_at = pending->time;
        ++append->records;
    }
}

int mesh_ui_trends_append(struct mesh_ui_trends *trends, const struct mesh_ui_history *history) {
    if (trends == NULL || history == NULL) {
        return -EINVAL;
    }
    if (!inkstand_journal_enabled(&trends->journal)) {
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

        char subject[MESH_UI_TRENDS_SUBJECT_LEN];
        if (!trends_subject(trends, node_id, subject, sizeof subject)) {
            continue;
        }
        struct trends_append_context append = {
            .pending = pending,
            .count = count,
            .chain_open = state->written,
            .chain_at = state->written_at,
        };
        bool over_cap = false;
        const int result = inkstand_journal_append(&trends->journal, subject, trends_write_append,
                                                   &append, &over_cap);
        if (result != 0) {
            /* `state` is untouched, so these readings are written again on the next publish. */
            inkwell_log_warn("ui", "Could not append to trend log %s: %d", subject, result);
            continue;
        }
        state->written = append.chain_open;
        state->written_at = append.chain_at;
        written += (int)append.records;
        if (over_cap) {
            trends_compact(trends, subject);
        }
    }

    return written;
}

int mesh_ui_trends_restore(struct mesh_ui_trends *trends, uint32_t node_id,
                           struct mesh_ui_history *history, uint32_t now_ms) {
    if (trends == NULL || history == NULL || node_id == 0U) {
        return -EINVAL;
    }
    char subject[MESH_UI_TRENDS_SUBJECT_LEN];
    if (!trends_subject(trends, node_id, subject, sizeof subject)) {
        return 0; /* the log is disabled: the live history is all there is, and it stands */
    }

    struct trend_record records[MESH_UI_TRENDS_MAX_RECORDS];
    uint32_t count = 0U;
    const int result =
        trends_read_file(trends, subject, records, MESH_UI_TRENDS_MAX_RECORDS, &count);
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
    /* Only what the journal wrote, matched on the suffix, and any temporary an interrupted
       compaction left beside one: it goes with the file it was going to replace. */
    return inkstand_journal_forget_all(&trends->journal);
}
