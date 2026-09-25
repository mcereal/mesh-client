#ifndef MESH_UI_STORE_JOURNAL_H
#define MESH_UI_STORE_JOURNAL_H

/*
 * An append-only journal per subject: a directory of line-record files, one per subject, each
 * appended to as things happen and cut back to its newest records when it outgrows a cap.
 *
 * The first application on this stack wrote it twice, once for a history per exchange and once
 * for a time series per peer, and wrote it the same way both times: the same directory that
 * disables itself rather than failing, the same open-append-close, the same compaction off one
 * size check, the same rewrite through a temporary, the same ring a reader folds a file into.
 * This is that half. What a record *is* - its keys, its fields, how several lines make one, what
 * counts as the same record twice - stays with whoever keeps it, as the callbacks below.
 *
 * Four decisions are the reason it is shaped the way it is:
 *
 *   - **A disabled journal is a quiet one.** A directory that could not be made leaves the
 *     journal disabled, and every call on it then succeeds having done nothing - a read finds no
 *     file. A device with full or read-only storage is still a working program, and what a journal
 *     holds is always the part of it that degrades most gracefully: it is the memory *behind*
 *     what is live, never the live thing itself.
 *
 *   - **A subject is a name, and a name is a plain word.** Letters, digits, '-' and '_', and
 *     nothing that could climb out of the directory. The caller spells its subjects - a prefix and
 *     a number in hex, say - and this refuses anything else rather than escaping it, because a
 *     journal whose names had to be escaped is one a person listing the directory cannot read.
 *
 *   - **Every question about a file is asked of the open handle.** Whether an append continues a
 *     file some earlier run wrote, and how large the file now is, are both read off the stream the
 *     append wrote through - not off a stat() of the name before or after. A size read off a path
 *     and acted on by reopening it is a check on one file and a write to whatever wears that name
 *     a moment later.
 *
 *   - **Compaction is the caller's, and the cap is what asks for it.** An append that leaves the
 *     file over the cap says so; the caller reads the newest records it wants to keep through a
 *     ring and writes them back through replace(). The journal cannot do that alone, because only
 *     the caller knows where one record ends - and a compaction that cut between the lines of one
 *     record would be a torn write the journal made on purpose.
 *
 * Not threads, not locks: one loop owns a journal, and a file is opened, used and closed within
 * one call.
 */

#include "inkwell/base/record_file.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest directory a journal will live in, its terminator included. A longer one disables
   the journal rather than writing to a truncated path somewhere the caller did not ask for. */
#define MESH_UI_JOURNAL_DIR_MAX 512U

/* The longest file suffix, terminator included - ".log", ".trend". */
#define MESH_UI_JOURNAL_SUFFIX_MAX 16U

/* The longest subject name, terminator included. */
#define MESH_UI_JOURNAL_SUBJECT_MAX 64U

/* The longest path any call builds: the directory, a separator, a subject, the suffix and the
   ".tmp" a rewrite goes through. A caller sizing a buffer for mesh_ui_journal_path() uses it. */
#define MESH_UI_JOURNAL_PATH_MAX                                                                   \
    (MESH_UI_JOURNAL_DIR_MAX + MESH_UI_JOURNAL_SUBJECT_MAX + MESH_UI_JOURNAL_SUFFIX_MAX + 8U)

struct mesh_ui_journal {
    /* With no trailing slash. Empty is a disabled journal. */
    char dir[MESH_UI_JOURNAL_DIR_MAX];
    /* What every file here ends in, so a wipe can tell this journal's files from anything else
       that ends up in the directory. */
    char suffix[MESH_UI_JOURNAL_SUFFIX_MAX];
    /* The size past which an append reports the file over its cap. 0 is uncapped. */
    uint64_t max_bytes;
};

/*
 * Opens a journal on `dir`, making the directory if it is not there (the parent must be).
 *
 * 0 on success. On failure the journal is left disabled - usable, and quiet - and the reason is
 * returned: -EINVAL for an empty directory or a suffix that is not a plain word after its dot,
 * -ENAMETOOLONG for a directory or suffix longer than the limits above, or the errno making the
 * directory failed with. A caller that only wants to know whether it has somewhere to write asks
 * mesh_ui_journal_enabled() afterwards.
 */
int mesh_ui_journal_init(struct mesh_ui_journal *journal, const char *dir, const char *suffix,
                         uint64_t max_bytes);

bool mesh_ui_journal_enabled(const struct mesh_ui_journal *journal);

/* Where `subject`'s file is. 0, -EINVAL for a subject that is not a plain word, -ENOENT for a
   disabled journal, or -ENAMETOOLONG when `out` is too short. */
int mesh_ui_journal_path(const struct mesh_ui_journal *journal, const char *subject, char *out,
                         size_t out_len);

/* Whether `subject` has a file yet. False for a disabled journal or a bad name. */
bool mesh_ui_journal_exists(const struct mesh_ui_journal *journal, const char *subject);

/*
 * Appends to `subject`'s file through `write`, creating it if it is not there.
 *
 * `resumed` tells the writer whether the file already held something when it was opened - the
 * question a record chained to the one before it needs answered, and one that has to be about
 * the file this handle holds rather than whatever the name meant a moment earlier.
 *
 * `*out_over_cap` (may be NULL) is set when the file is now larger than the journal's cap, which
 * is the caller's cue to compact it; see the header comment for why the journal does not.
 *
 * 0 or a negative errno. A writer reports its own failure through ferror(file), as
 * inkwell_record_append()'s does, and a failed append is one the caller should not remember as
 * written: the next attempt writes it again.
 */
typedef void (*mesh_ui_journal_append_fn)(FILE *file, bool resumed, void *context);

int mesh_ui_journal_append(const struct mesh_ui_journal *journal, const char *subject,
                           mesh_ui_journal_append_fn write, void *context, bool *out_over_cap);

/*
 * One pass over `subject`'s file, a decoded key and value at a time - inkwell_record_read()'s
 * rules exactly, so a torn final append or an overlong line is never seen. `line` is the
 * caller's buffer and bounds the longest record line it will accept.
 *
 * 0, -ENOENT when there is no file (or the journal is disabled), or another negative errno.
 */
int mesh_ui_journal_read(const struct mesh_ui_journal *journal, const char *subject, char *line,
                         size_t capacity, inkwell_record_visit_fn visit, void *context);

/* Replaces `subject`'s file with what `write` writes, through a temporary beside it, so a reader
   sees the old file or the new one and an interrupted rewrite leaves the old. 0 or a negative
   errno; 0 without calling `write` for a disabled journal. */
int mesh_ui_journal_replace(const struct mesh_ui_journal *journal, const char *subject,
                            inkwell_record_write_fn write, void *context);

/*
 * Streams `subject`'s file through a filter into a replacement, for a change to a file too large
 * to read into memory and write back - a delete, most often, where whatever did not fit a buffer
 * would be discarded along with what was asked for.
 *
 * `line` sees each complete line as it sits on disk, undecoded and without its newline, with the
 * stream the replacement is being written to; it writes what it keeps. It sees the lines a reader
 * would and no others: an overlong line or a torn final one is not carried into the replacement,
 * which is no loss, because no reader would ever have returned it. `end` is called once after the
 * last line, to write anything the filter was still holding back, and returns how many records
 * it dropped.
 *
 * Returns that count. 0 leaves the file exactly as it was - a filter that found nothing to do
 * must not rewrite it. -ENOENT is never returned: a subject with no file has nothing to drop.
 */
typedef void (*mesh_ui_journal_filter_fn)(void *context, const char *line, FILE *out);
typedef uint32_t (*mesh_ui_journal_filter_end_fn)(void *context, FILE *out);

int mesh_ui_journal_filter(const struct mesh_ui_journal *journal, const char *subject, char *line,
                           size_t capacity, mesh_ui_journal_filter_fn filter,
                           mesh_ui_journal_filter_end_fn end, void *context);

/* Removes `subject`'s file. 0 when it is gone, including when it never existed. */
int mesh_ui_journal_forget(const struct mesh_ui_journal *journal, const char *subject);

/* Removes every file this journal wrote, and any temporary an interrupted rewrite left beside
   one. Returns how many subjects' files were removed, or a negative errno. */
int mesh_ui_journal_forget_all(const struct mesh_ui_journal *journal);

/*
 * The newest `capacity` records of a file, in a caller's buffer.
 *
 * A ring rather than a seek to the tail: a file is capped, and a single forward pass into a fixed
 * destination has no offset arithmetic to get wrong on a file whose last record may be torn. The
 * capacity is the caller's because two readers of one file want different amounts - a screen
 * wants what it can draw, a compaction what the file is allowed to keep - and a reader fixed to
 * the smaller would make every compaction a truncation to the window.
 *
 * `dropped` counts the records the ring pushed out of the front, which is what lets a reader say
 * there is more behind the oldest one it holds. It is counted as a slot is taken, not as the ring
 * wraps: the record that *fills* the last slot has pushed nothing out.
 */
struct mesh_ui_journal_ring {
    unsigned char *entries;
    size_t size;
    uint32_t capacity;
    /* Where the next record goes while the buffer is filling, then the oldest slot. */
    uint32_t next;
    uint32_t dropped;
    bool wrapped;
};

void mesh_ui_journal_ring_init(struct mesh_ui_journal_ring *ring, void *entries, size_t size,
                               uint32_t capacity);

/* The slot the next record goes in - the oldest one once the ring is full. Never NULL for a ring
   with a capacity. The caller fills it. */
void *mesh_ui_journal_ring_push(struct mesh_ui_journal_ring *ring);

/* How many slots hold a record, and slot `i` of them in storage order - not transcript order,
   until mesh_ui_journal_ring_finish() has run. For a reader that folds a record it has seen
   before into the slot it already holds. */
uint32_t mesh_ui_journal_ring_held(const struct mesh_ui_journal_ring *ring);
void *mesh_ui_journal_ring_at(const struct mesh_ui_journal_ring *ring, uint32_t i);

/* Rotates the buffer oldest-first, in place, and returns how many records it holds. */
uint32_t mesh_ui_journal_ring_finish(struct mesh_ui_journal_ring *ring);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_STORE_JOURNAL_H */
