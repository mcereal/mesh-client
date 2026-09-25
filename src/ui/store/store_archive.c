#define _POSIX_C_SOURCE 200809L

/*
 * The per-conversation transcript on the card.
 *
 * What this is for and the three decisions behind its format are in
 * include/mesh/ui/store_archive.h; this file is the mechanics. It sits beside store_file.c in
 * the store group and shares its record codec over src/ui/store/store_internal.h, so a message is
 * spelled the same way in both files on the card.
 *
 * Every entry point tolerates a disabled archive - one whose directory could not be made - and
 * reports success for it. A Brick with a full or read-only card is still a client, and the
 * transcript is the part of it that degrades most gracefully: the live view is unaffected,
 * because that comes from the transport ring.
 */

#include "mesh/ui/store_archive.h"
#include "inkwell/base/record_file.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"

#include "store_internal.h"

#include "mesh/ui/nav.h"
#include "mesh/ui/store_keys.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ---- naming a conversation ----------------------------------------------------------------- */

/* What every file here ends in. */
#define MESH_UI_ARCHIVE_SUFFIX ".log"

/* The longest subject name archive_subject() writes: `n` and eight hex digits. */
#define MESH_UI_ARCHIVE_SUBJECT_LEN 16U

/*
 * One conversation's subject in the journal, as `c07` or `n1a2b3c4d` - so its file is
 * `<dir>/c07.log` or `<dir>/n1a2b3c4d.log`.
 *
 * The two prefixes rather than one name space, because a channel index and a node number are
 * different kinds of number that would otherwise collide at the low end: channel 1 and node 1
 * are different conversations and upstream allows both. Hex for the node because that is how
 * every other surface in this client and in the firmware spells one.
 *
 * Nothing user-supplied reaches the name - a channel index is a uint8_t and a node number a
 * uint32_t, both printed by us - so there is no escaping to do, and the journal refuses a name
 * that could climb out of its directory besides.
 */
static bool archive_subject(const struct mesh_ui_archive *archive, uint8_t kind, uint32_t node,
                            uint8_t channel, char *out, size_t out_len) {
    if (archive == NULL || !inkstand_journal_enabled(&archive->journal) || out == NULL) {
        return false;
    }
    int written;
    switch ((enum mesh_ui_conversation_kind)kind) {
    case MESH_UI_CONVERSATION_CHANNEL:
        written = snprintf(out, out_len, "c%02x", (unsigned)channel);
        break;
    case MESH_UI_CONVERSATION_DIRECT:
        written = snprintf(out, out_len, "n%08x", node);
        break;
    /* Neither names a conversation - one is a view over all of them and the other is a button -
       so neither has a file, exactly as neither can be deleted. */
    case MESH_UI_CONVERSATION_ALL:
    case MESH_UI_CONVERSATION_NEW:
    default:
        return false;
    }
    return written > 0 && written < (int)out_len;
}

/* Which conversation a message belongs to, in the terms the rest of the store names one. The
   store's copy already carries the resolved peer and a broadcast flag, so this needs no roster. */
static void archive_conversation_of(const struct mesh_ui_message *message, uint8_t *out_kind,
                                    uint32_t *out_node, uint8_t *out_channel) {
    if (message->broadcast) {
        *out_kind = (uint8_t)MESH_UI_CONVERSATION_CHANNEL;
        *out_node = 0U;
        *out_channel = message->channel;
    } else {
        *out_kind = (uint8_t)MESH_UI_CONVERSATION_DIRECT;
        *out_node = message->peer;
        *out_channel = 0U;
    }
}

/* ---- what this run has already written ------------------------------------------------------ */

/*
 * Whether two records are the same message.
 *
 * Not the packet id alone: MeshPacket.id only has to be unique per sender for a few minutes, and
 * one conversation's file holds every sender on that channel, so two nodes can legitimately land
 * on the same id. Folding those together would lose one of the two messages for good, which is
 * the worst thing a transcript can do quietly. See struct mesh_ui_archive_recent.
 *
 * Deliberately not the text or the delivery state: those are what *changes* about a message
 * that is still the same message, and this is the question "have I seen this one before".
 */
static bool archive_same_message(const struct mesh_ui_message *a, const struct mesh_ui_message *b) {
    return a->packet_id != 0U && a->packet_id == b->packet_id && a->peer == b->peer &&
           a->direction == b->direction;
}

/* The slot this run already holds for `message`, or NULL. */
static struct mesh_ui_archive_recent *archive_recent_slot(struct mesh_ui_archive *archive,
                                                          const struct mesh_ui_message *message) {
    if (message->packet_id == 0U) {
        return NULL;
    }
    for (uint32_t i = 0; i < archive->recent_count; ++i) {
        struct mesh_ui_archive_recent *slot = &archive->recent[i];
        if (slot->packet_id != 0U && slot->packet_id == message->packet_id &&
            slot->peer == message->peer && slot->direction == message->direction) {
            return slot;
        }
    }
    return NULL;
}

/*
 * Whether this run has already put this message on the card *in this state*.
 *
 * The state half is the point: an outbound message goes out pending and is acknowledged a few
 * seconds later, and an archive that only asked "have I written this id" would keep the pending
 * copy for good. A changed ack is a record worth appending again.
 */
static bool archive_recently_written(struct mesh_ui_archive *archive,
                                     const struct mesh_ui_message *message) {
    const struct mesh_ui_archive_recent *slot = archive_recent_slot(archive, message);
    return slot != NULL && slot->ack == message->ack && slot->ack_error == message->ack_error;
}

/* Records what was written. A message already in the ring keeps its slot and takes the new
   delivery state, so a message acknowledged twice does not spend two slots. */
static void archive_remember(struct mesh_ui_archive *archive,
                             const struct mesh_ui_message *message) {
    if (message->packet_id == 0U) {
        return;
    }
    struct mesh_ui_archive_recent *slot = archive_recent_slot(archive, message);
    if (slot == NULL) {
        slot = &archive->recent[archive->recent_next];
        archive->recent_next = (archive->recent_next + 1U) % MESH_UI_ARCHIVE_RECENT_MAX;
        if (archive->recent_count < MESH_UI_ARCHIVE_RECENT_MAX) {
            archive->recent_count++;
        }
    }
    slot->packet_id = message->packet_id;
    slot->peer = message->peer;
    slot->direction = message->direction;
    slot->ack = message->ack;
    slot->ack_error = message->ack_error;
}

/*
 * A message with no packet id is written once and then forgotten about.
 *
 * Upstream's "no id" is 0, so such a record names no single message and cannot be deduplicated
 * against anything - which also means it cannot be recognised on the next publish while it is
 * still sitting in the ring. Rather than append it on every publish for as long as the ring
 * holds it, the archive declines it: the live view still shows it, and the one place it could
 * have come from is a radio that did not fill in MeshPacket.id.
 */
static bool archive_writable(const struct mesh_ui_message *message) {
    return message != NULL && message->packet_id != 0U;
}

/* ---- reading a file ------------------------------------------------------------------------- */

/* One line of either file on the card. store_file.c's own reader uses the same number, and for
   the same reason: a msg_text[] line is a 233-byte payload in which every byte may have gone out
   as a four-character escape, plus its key. */
#define MESH_UI_ARCHIVE_LINE_MAX 1280U

/*
 * Every record in one file, newest last, folded into a caller's buffer that holds the newest
 * `capacity` of them - the journal's ring, which is where the argument for a ring over a seek to
 * the tail now lives. A record here is five short lines, so the reader's own part is assembling
 * them and recognising a message it already holds. `dropped` is what lets the transcript say
 * there is more behind the top of it.
 *
 * The capacity is the caller's because the two readers want different amounts of the same file:
 * the thread screen wants what it can draw (MESH_UI_MAX_THREAD_MESSAGES) and compaction wants
 * what the file is allowed to keep (MESH_UI_ARCHIVE_MAX_MESSAGES).
 */
struct archive_reader {
    struct inkstand_journal_ring ring;
    /* The record being assembled, and the index its msg[] line carried. */
    struct mesh_ui_message current;
    uint32_t current_index;
    bool current_open;
};

/* An id already in the buffer, or NULL when there is none. A linear scan over at most a few
   hundred records, run once per record: the alternative is an index kept in step with a ring
   that overwrites its own oldest slot, for a file read at most once per thread the reader
   opens. */
static struct mesh_ui_message *archive_buffer_find(const struct archive_reader *reader,
                                                   const struct mesh_ui_message *message) {
    if (message->packet_id == 0U) {
        return NULL;
    }
    const uint32_t held = inkstand_journal_ring_held(&reader->ring);
    for (uint32_t i = 0; i < held; ++i) {
        struct mesh_ui_message *entry = inkstand_journal_ring_at(&reader->ring, i);
        if (archive_same_message(entry, message)) {
            return entry;
        }
    }
    return NULL;
}

/* Close whatever record is open and put it in the buffer. */
static void archive_reader_commit(struct archive_reader *reader) {
    if (!reader->current_open) {
        return;
    }
    reader->current_open = false;

    /*
     * A packet id we already hold is the same message written twice - a radio that re-delivered
     * it across a restart, since within one run the recent ring stops it. The later copy's
     * contents win, because delivery state is the thing that moves after a message is first
     * seen; its position does not, because where the conversation reached it is where it
     * happened.
     */
    struct mesh_ui_message *seen = archive_buffer_find(reader, &reader->current);
    if (seen != NULL) {
        *seen = reader->current;
        return;
    }
    struct mesh_ui_message *slot = inkstand_journal_ring_push(&reader->ring);
    if (slot != NULL) {
        *slot = reader->current;
    }
}

static void archive_reader_line(struct archive_reader *reader, const char *key, char *value) {
    uint32_t index = 0U;
    uint32_t slot = 0U;
    const enum mesh_ui_store_key id = mesh_ui_store_key_lookup(key, &index, &slot);
    if (!mesh_ui_store_key_is_message(id)) {
        return;
    }

    if (id == MESH_UI_STORE_KEY_MSG) {
        /* A msg[] line always begins a record, whatever index it carries - that is the rule
           that makes an index restarting at zero every run harmless. See the header. */
        archive_reader_commit(reader);
        memset(&reader->current, 0, sizeof reader->current);
        if (!mesh_ui_store_read_message_line(&reader->current, id, value)) {
            return; /* a torn or hand-edited line: no record opens */
        }
        reader->current_index = index;
        reader->current_open = true;
        return;
    }

    /* A detail line belongs to the record above it only if it carries the same index. One that
       does not is a line from a record whose msg[] line did not read. */
    if (!reader->current_open || index != reader->current_index) {
        return;
    }
    (void)mesh_ui_store_read_message_line(&reader->current, id, value);
}

/*
 * One pass over a conversation's file into `entries`.
 *
 * The line loop is store_file.c's, deliberately: same length, same split on the first '=', same
 * unescape, same tolerance of a comment or a blank. Returns -ENOENT when there is no such file,
 * which every caller treats as an empty conversation rather than as a failure.
 */
static void archive_read_line(void *context, const char *key, char *value) {
    archive_reader_line(context, key, value);
}

static int archive_read_file(const struct mesh_ui_archive *archive, const char *subject,
                             struct mesh_ui_message *entries, uint32_t capacity,
                             uint32_t *out_count, uint32_t *out_dropped) {
    if (entries == NULL || capacity == 0U || out_count == NULL) {
        return -EINVAL;
    }
    struct archive_reader reader;
    memset(&reader, 0, sizeof reader);
    inkstand_journal_ring_init(&reader.ring, entries, sizeof *entries, capacity);

    char line[MESH_UI_ARCHIVE_LINE_MAX];
    const int result = inkstand_journal_read(&archive->journal, subject, line, sizeof line,
                                             archive_read_line, &reader);
    if (result != 0) {
        return result;
    }
    archive_reader_commit(&reader);
    *out_count = inkstand_journal_ring_finish(&reader.ring);
    if (out_dropped != NULL) {
        *out_dropped = reader.ring.dropped;
    }
    return 0;
}

/* ---- writing a file ------------------------------------------------------------------------- */

/*
 * Replaces a conversation's file with `messages`, through the journal's temporary.
 *
 * The file a reader opens is either the old transcript or the new one. The handshake cache next
 * door is written the same way now, and for the same reason: the argument that it was only "a
 * snapshot the next publish rebuilds" held for every section of it except the roster, which is
 * the one record of the nodes a radio has evicted and which no publish can rebuild.
 */
struct archive_rewrite_context {
    const struct mesh_ui_message *messages;
    uint32_t count;
};

static void archive_write_replacement(FILE *file, void *context) {
    const struct archive_rewrite_context *records = context;
    for (uint32_t i = 0; i < records->count; ++i) {
        mesh_ui_store_write_message(file, i, &records->messages[i]);
    }
}

static int archive_rewrite(const struct mesh_ui_archive *archive, const char *subject,
                           const struct mesh_ui_message *messages, uint32_t count) {
    struct archive_rewrite_context context = {messages, count};
    return inkstand_journal_replace(&archive->journal, subject, archive_write_replacement,
                                    &context);
}

/*
 * Cuts a conversation's file back to its newest records, once an append has found it over the
 * cap. The size is the journal's, read off the stream the append wrote through: the read below
 * only happens on the append that actually trips the threshold, which for a conversation of
 * ordinary traffic is once every few thousand messages.
 *
 * The buffer is MESH_UI_ARCHIVE_MAX_MESSAGES rather than the window the thread screen reads,
 * which is the point - compacting into the window would make every compaction a truncation of
 * the transcript to what happened to be on screen. It is about 150 KB on the stack, held for
 * the length of one rewrite, as store_file.c holds a whole cache for the length of one load:
 * the client is single-threaded with the main thread's stack under it.
 */
static void archive_compact(const struct mesh_ui_archive *archive, const char *subject) {
    struct mesh_ui_message keep[MESH_UI_ARCHIVE_MAX_MESSAGES];
    uint32_t count = 0U;
    if (archive_read_file(archive, subject, keep, MESH_UI_ARCHIVE_MAX_MESSAGES, &count, NULL) !=
        0) {
        return;
    }
    const int result = archive_rewrite(archive, subject, keep, count);
    if (result != 0) {
        inkwell_log_warn("ui", "Could not compact message archive %s: %d", subject, result);
        return;
    }
    inkwell_log_info("ui", "Compacted message archive %s to %u messages", subject, (unsigned)count);
}

/* Appends whole records to a conversation's file, creating it if it is not there. */
struct archive_append_context {
    struct mesh_ui_archive *archive;
    const struct mesh_ui_message *const *messages;
    uint32_t count;
};

static void archive_write_append(FILE *file, bool resumed, void *context) {
    (void)resumed; /* a message carries its own time; nothing here is measured from the last */
    struct archive_append_context *records = context;
    for (uint32_t i = 0; i < records->count; ++i) {
        mesh_ui_store_write_message(file, records->archive->next_index, records->messages[i]);
        records->archive->next_index++;
    }
}

static int archive_append_records(struct mesh_ui_archive *archive, const char *subject,
                                  const struct mesh_ui_message *const *messages, uint32_t count) {
    if (count == 0U) {
        return 0;
    }
    struct archive_append_context context = {archive, messages, count};
    bool over_cap = false;
    const int result = inkstand_journal_append(&archive->journal, subject, archive_write_append,
                                               &context, &over_cap);
    if (result != 0) {
        return result;
    }
    if (over_cap) {
        archive_compact(archive, subject);
    }
    return (int)count;
}

/* ---- the public half ------------------------------------------------------------------------ */

int mesh_ui_archive_init(struct mesh_ui_archive *archive, const char *dir) {
    if (archive == NULL) {
        return -EINVAL;
    }
    memset(archive, 0, sizeof *archive);
    const int result = inkstand_journal_init(&archive->journal, dir, MESH_UI_ARCHIVE_SUFFIX,
                                             MESH_UI_ARCHIVE_FILE_MAX_BYTES);
    if (result != 0 && result != -EINVAL) {
        inkwell_log_warn("ui", "Message archive unavailable at %s: %d", dir, result);
    }
    return result;
}

/*
 * One publish's worth of new traffic, grouped by conversation so each file is opened once.
 *
 * The grouping matters on a busy mesh rather than in principle: without it a publish carrying
 * ten messages across four conversations is ten opens on a card mounted sync.
 */
int mesh_ui_archive_append(struct mesh_ui_archive *archive,
                           const struct mesh_ui_message_list *list) {
    if (archive == NULL || list == NULL) {
        return -EINVAL;
    }
    if (!inkstand_journal_enabled(&archive->journal)) {
        return 0;
    }

    const uint32_t count = list->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : list->count;
    bool taken[MESH_UI_MAX_MESSAGES];
    memset(taken, 0, sizeof taken);
    int written = 0;

    for (uint32_t i = 0; i < count; ++i) {
        if (taken[i]) {
            continue;
        }
        const struct mesh_ui_message *first = &list->entries[i];
        if (!archive_writable(first) || archive_recently_written(archive, first)) {
            taken[i] = true;
            continue;
        }

        uint8_t kind = 0U;
        uint32_t node = 0U;
        uint8_t channel = 0U;
        archive_conversation_of(first, &kind, &node, &channel);

        char subject[MESH_UI_ARCHIVE_SUBJECT_LEN];
        if (!archive_subject(archive, kind, node, channel, subject, sizeof subject)) {
            taken[i] = true;
            continue;
        }

        /* Everything else in this publish bound for the same file, in transcript order. */
        const struct mesh_ui_message *batch[MESH_UI_MAX_MESSAGES];
        uint32_t batch_count = 0U;
        for (uint32_t j = i; j < count; ++j) {
            if (taken[j]) {
                continue;
            }
            const struct mesh_ui_message *candidate = &list->entries[j];
            if (!archive_writable(candidate) || archive_recently_written(archive, candidate)) {
                continue;
            }
            uint8_t other_kind = 0U;
            uint32_t other_node = 0U;
            uint8_t other_channel = 0U;
            archive_conversation_of(candidate, &other_kind, &other_node, &other_channel);
            if (other_kind != kind || other_node != node || other_channel != channel) {
                continue;
            }
            batch[batch_count++] = candidate;
            taken[j] = true;
        }

        const int result = archive_append_records(archive, subject, batch, batch_count);
        if (result < 0) {
            inkwell_log_warn("ui", "Could not append to message archive %s: %d", subject, result);
            continue;
        }
        /* Remembered only once the records are on the card, so a failed write is retried on the
           next publish rather than silently dropped. */
        for (uint32_t k = 0; k < batch_count; ++k) {
            archive_remember(archive, batch[k]);
        }
        written += result;
    }

    return written;
}

/* Whether this run has already established that a conversation's file exists. */
static bool archive_seen(const struct mesh_ui_archive *archive, uint8_t kind, uint32_t node,
                         uint8_t channel) {
    for (uint32_t i = 0; i < archive->seeded_count; ++i) {
        if (archive->seeded[i].kind != kind) {
            continue;
        }
        if (kind == (uint8_t)MESH_UI_CONVERSATION_CHANNEL &&
            archive->seeded[i].channel == channel) {
            return true;
        }
        if (kind == (uint8_t)MESH_UI_CONVERSATION_DIRECT && archive->seeded[i].node == node) {
            return true;
        }
    }
    return false;
}

static void archive_mark_seen(struct mesh_ui_archive *archive, uint8_t kind, uint32_t node,
                              uint8_t channel) {
    if (archive->seeded_count >= MESH_UI_ARCHIVE_CONVERSATIONS_MAX) {
        return;
    }
    archive->seeded[archive->seeded_count].kind = kind;
    archive->seeded[archive->seeded_count].node = node;
    archive->seeded[archive->seeded_count].channel = channel;
    archive->seeded_count++;
}

int mesh_ui_archive_seed(struct mesh_ui_archive *archive, const struct mesh_ui_message_list *list) {
    if (archive == NULL || list == NULL) {
        return -EINVAL;
    }
    if (!inkstand_journal_enabled(&archive->journal)) {
        return 0;
    }

    const uint32_t count = list->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : list->count;
    bool taken[MESH_UI_MAX_MESSAGES];
    memset(taken, 0, sizeof taken);
    int written = 0;

    for (uint32_t i = 0; i < count; ++i) {
        if (taken[i]) {
            continue;
        }
        taken[i] = true;
        const struct mesh_ui_message *first = &list->entries[i];

        uint8_t kind = 0U;
        uint32_t node = 0U;
        uint8_t channel = 0U;
        archive_conversation_of(first, &kind, &node, &channel);
        if (archive_seen(archive, kind, node, channel)) {
            continue;
        }
        archive_mark_seen(archive, kind, node, channel);

        char subject[MESH_UI_ARCHIVE_SUBJECT_LEN];
        if (!archive_subject(archive, kind, node, channel, subject, sizeof subject)) {
            continue;
        }
        /* A conversation that already has a file has a transcript at least as good as this one,
           and rewriting it with the cache's 64 would be throwing history away to save it. */
        if (inkstand_journal_exists(&archive->journal, subject)) {
            continue;
        }

        struct mesh_ui_message batch[MESH_UI_MAX_MESSAGES];
        uint32_t batch_count = 0U;
        for (uint32_t j = i; j < count; ++j) {
            const struct mesh_ui_message *candidate = &list->entries[j];
            uint8_t other_kind = 0U;
            uint32_t other_node = 0U;
            uint8_t other_channel = 0U;
            archive_conversation_of(candidate, &other_kind, &other_node, &other_channel);
            if (other_kind != kind || other_node != node || other_channel != channel) {
                continue;
            }
            taken[j] = true;
            batch[batch_count++] = *candidate;
        }
        if (batch_count == 0U) {
            continue;
        }

        const int result = archive_rewrite(archive, subject, batch, batch_count);
        if (result != 0) {
            inkwell_log_warn("ui", "Could not seed message archive %s: %d", subject, result);
            continue;
        }
        /* Seeded records are on the card now, so the append path must not write them again. */
        for (uint32_t k = 0; k < batch_count; ++k) {
            archive_remember(archive, &batch[k]);
        }
        written += (int)batch_count;
    }

    return written;
}

int mesh_ui_archive_load_thread(const struct mesh_ui_archive *archive, uint8_t kind, uint32_t node,
                                uint8_t channel, struct mesh_ui_thread *out) {
    if (archive == NULL || out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof *out);
    out->kind = kind;
    out->node = node;
    out->channel = channel;

    char subject[MESH_UI_ARCHIVE_SUBJECT_LEN];
    if (!archive_subject(archive, kind, node, channel, subject, sizeof subject)) {
        /* No file is possible for this conversation - it is the all-traffic view or the "New
           message" row, or the archive is disabled. An empty window, and `valid` left false so
           the transcript falls back to the flat list rather than drawing nothing. */
        return 0;
    }

    const int result = archive_read_file(archive, subject, out->entries,
                                         MESH_UI_MAX_THREAD_MESSAGES, &out->count, &out->dropped);
    if (result == -ENOENT) {
        /* A conversation nobody has said anything in yet. Valid and empty, so the window is the
           authority for it rather than the flat list - which for a brand new conversation holds
           the same nothing anyway. */
        out->valid = true;
        return 0;
    }
    if (result != 0) {
        memset(out, 0, sizeof *out);
        return result;
    }
    out->kind = kind;
    out->node = node;
    out->channel = channel;
    out->valid = true;
    return 0;
}

int mesh_ui_archive_forget_conversation(struct mesh_ui_archive *archive, uint8_t kind,
                                        uint32_t node, uint8_t channel) {
    if (archive == NULL) {
        return -EINVAL;
    }
    char subject[MESH_UI_ARCHIVE_SUBJECT_LEN];
    if (!archive_subject(archive, kind, node, channel, subject, sizeof subject)) {
        return 0;
    }
    return inkstand_journal_forget(&archive->journal, subject);
}

/*
 * One record's worth of streaming state, for a delete that must not lose the rest of the file.
 *
 * The file is capped by *bytes* and ordinarily holds thousands of records, so a delete cannot
 * read it into a buffer and write the buffer back: whatever did not fit would be discarded along
 * with the message the user asked to be rid of. It copies instead, line by line, and the only
 * thing it has to hold is the one line whose verdict is not settled yet.
 *
 * That line is the msg[] line. A record is dropped when it *is* the target or when it is a
 * reaction naming the target, and the second of those is only knowable from the msg_meta[] line
 * that follows - so msg[] is held back until its meta arrives, or until anything else shows the
 * record has no meta to come.
 */
struct archive_filter {
    uint32_t target;
    /* The msg[] line, verbatim, waiting for its verdict. */
    char pending[MESH_UI_ARCHIVE_LINE_MAX];
    bool pending_held;
    uint32_t pending_index;
    uint32_t pending_packet_id;
    /* The settled verdict for the record now streaming through. */
    bool dropping;
    uint32_t dropped_records;
};

/* Write the held msg[] line, or drop it, and settle the record either way. */
static void archive_filter_settle(struct archive_filter *filter, FILE *out, bool is_reaction,
                                  uint32_t reply_id) {
    if (!filter->pending_held) {
        return;
    }
    filter->pending_held = false;
    filter->dropping = (filter->pending_packet_id == filter->target) ||
                       (is_reaction && reply_id == filter->target);
    if (filter->dropping) {
        filter->dropped_records++;
        return;
    }
    fputs(filter->pending, out);
    fputc('\n', out);
}

/*
 * One line of the file, copied through or dropped.
 *
 * `raw` is the line as it sits on disk, which is what gets written back - re-escaping a value
 * this code never had a reason to decode would be a second spelling of the format, and the
 * whole point of sharing store_keys.h is that there is only one.
 */
static void archive_filter_line(void *context, const char *raw, FILE *out) {
    struct archive_filter *filter = context;
    char work[MESH_UI_ARCHIVE_LINE_MAX];
    inkwell_str_copy(work, sizeof work, raw);
    char *equals = strchr(work, '=');
    if (equals == NULL) {
        /* Not a key line at all - a comment, a blank, something hand-added. It belongs to
           nobody, so it is copied rather than attached to whatever record is open. */
        archive_filter_settle(filter, out, false, 0U);
        fputs(raw, out);
        fputc('\n', out);
        return;
    }
    *equals = '\0';
    char *value = equals + 1;
    mesh_ui_store_unescape_value(value);

    uint32_t index = 0U;
    uint32_t slot = 0U;
    const enum mesh_ui_store_key id = mesh_ui_store_key_lookup(work, &index, &slot);

    if (id == MESH_UI_STORE_KEY_MSG) {
        /* A msg[] line always begins a record, so whatever was open is settled on what is known
           about it - which for a record with no msg_meta[] is its packet id alone. */
        archive_filter_settle(filter, out, false, 0U);
        struct mesh_ui_message opened;
        memset(&opened, 0, sizeof opened);
        if (!mesh_ui_store_read_message_line(&opened, id, value)) {
            /* A torn or hand-edited line opens no record. Kept: this is a delete of one named
               message, not a tidy-up of the file. */
            filter->dropping = false;
            fputs(raw, out);
            fputc('\n', out);
            return;
        }
        inkwell_str_copy(filter->pending, sizeof filter->pending, raw);
        filter->pending_held = true;
        filter->pending_index = index;
        filter->pending_packet_id = opened.packet_id;
        return;
    }

    if (id == MESH_UI_STORE_KEY_MSG_META && filter->pending_held &&
        index == filter->pending_index) {
        struct mesh_ui_message meta;
        memset(&meta, 0, sizeof meta);
        (void)mesh_ui_store_read_message_line(&meta, id, value);
        archive_filter_settle(filter, out, meta.is_reaction, meta.reply_id);
        if (filter->dropping) {
            return;
        }
        fputs(raw, out);
        fputc('\n', out);
        return;
    }

    /* Any other line: the record it belongs to is settled by now. */
    archive_filter_settle(filter, out, false, 0U);
    if (mesh_ui_store_key_is_message(id) && filter->dropping) {
        return;
    }
    fputs(raw, out);
    fputc('\n', out);
}

/* The end of the file settles the last record on its packet id alone. */
static uint32_t archive_filter_end(void *context, FILE *out) {
    struct archive_filter *filter = context;
    archive_filter_settle(filter, out, false, 0U);
    return filter->dropped_records;
}

int mesh_ui_archive_forget_message(struct mesh_ui_archive *archive, uint8_t kind, uint32_t node,
                                   uint8_t channel, uint32_t packet_id) {
    if (archive == NULL || packet_id == 0U) {
        return -EINVAL;
    }
    char subject[MESH_UI_ARCHIVE_SUBJECT_LEN];
    if (!archive_subject(archive, kind, node, channel, subject, sizeof subject)) {
        return 0;
    }

    struct archive_filter filter;
    memset(&filter, 0, sizeof filter);
    filter.target = packet_id;

    /* A delete that found its message somewhere else leaves this file exactly as it was: the
       journal replaces it only when the filter says it dropped something. */
    char line[MESH_UI_ARCHIVE_LINE_MAX];
    const int result = inkstand_journal_filter(&archive->journal, subject, line, sizeof line,
                                               archive_filter_line, archive_filter_end, &filter);
    if (result <= 0) {
        return result;
    }

    /* The message must not be remembered as written: if the radio delivers the same packet again
       the user should see it arrive rather than have it swallowed by this run's ring. */
    for (uint32_t i = 0; i < archive->recent_count; ++i) {
        if (archive->recent[i].packet_id == packet_id) {
            memset(&archive->recent[i], 0, sizeof archive->recent[i]);
        }
    }
    return result;
}
