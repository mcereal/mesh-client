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

#include "store_internal.h"

#include "mesh/ui/nav.h"
#include "mesh/ui/store_keys.h"
#include "mesh/utils/log.h"
#include "mesh/utils/text.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- naming a conversation ----------------------------------------------------------------- */

/*
 * One conversation's file, as `<dir>/c07.log` or `<dir>/n1a2b3c4d.log`.
 *
 * The two prefixes rather than one name space, because a channel index and a node number are
 * different kinds of number that would otherwise collide at the low end: channel 1 and node 1
 * are different conversations and upstream allows both. Hex for the node because that is how
 * every other surface in this client and in the firmware spells one.
 *
 * Nothing user-supplied reaches the name - a channel index is a uint8_t and a node number a
 * uint32_t, both printed by us - so there is no path traversal to defend against here, and no
 * escaping either.
 */
static bool archive_path(const struct mesh_ui_archive *archive, uint8_t kind, uint32_t node,
                         uint8_t channel, char *out, size_t out_len) {
    if (archive == NULL || archive->dir[0] == '\0' || out == NULL) {
        return false;
    }
    int written;
    switch ((enum mesh_ui_conversation_kind)kind) {
    case MESH_UI_CONVERSATION_CHANNEL:
        written = snprintf(out, out_len, "%s/c%02x.log", archive->dir, (unsigned)channel);
        break;
    case MESH_UI_CONVERSATION_DIRECT:
        written = snprintf(out, out_len, "%s/n%08x.log", archive->dir, node);
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
 * `capacity` of them.
 *
 * A ring rather than a seek to the tail: the file is capped, a record is five short lines, and
 * a single forward pass with a fixed destination has no offset arithmetic to get wrong on a
 * file whose last record may be a torn write. `dropped` counts what the ring overwrote, which
 * is what lets the transcript say there is more behind the top of it.
 *
 * The capacity is the caller's because the two readers want different amounts of the same file:
 * the thread screen wants what it can draw (MESH_UI_MAX_THREAD_MESSAGES) and compaction wants
 * what the file is allowed to keep (MESH_UI_ARCHIVE_MAX_MESSAGES). A reader fixed to the
 * smaller of those would have made every compaction a truncation to the window.
 */
struct archive_reader {
    struct mesh_ui_message *entries;
    uint32_t capacity;
    uint32_t dropped;
    /* Where the next record goes while the buffer is still filling, then the oldest slot. */
    uint32_t next;
    bool wrapped;
    /* The record being assembled, and the index its msg[] line carried. */
    struct mesh_ui_message current;
    uint32_t current_index;
    bool current_open;
};

/* The buffer in transcript order, unwound from the ring. Returns how many records it holds. */
static uint32_t archive_reader_finish(struct archive_reader *reader);

/* An id already in the buffer, or `capacity` when there is none. A linear scan over at most a
   few hundred `uint32_t`, run once per record: the alternative is an index kept in step with a
   ring that overwrites its own oldest slot, for a file read at most once per thread the reader
   opens. */
static uint32_t archive_buffer_find(const struct archive_reader *reader,
                                    const struct mesh_ui_message *message) {
    if (message->packet_id == 0U) {
        return reader->capacity;
    }
    const uint32_t held = reader->wrapped ? reader->capacity : reader->next;
    for (uint32_t i = 0; i < held; ++i) {
        if (archive_same_message(&reader->entries[i], message)) {
            return i;
        }
    }
    return reader->capacity;
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
    const uint32_t seen = archive_buffer_find(reader, &reader->current);
    if (seen < reader->capacity) {
        reader->entries[seen] = reader->current;
        return;
    }

    /* Counted before the slot is taken, not after the ring wraps: the record that *fills* the
       last slot has pushed nothing out, and counting on the wrap itself made the buffer claim
       one dropped message the moment it was merely full. */
    if (reader->wrapped) {
        reader->dropped++;
    }
    reader->entries[reader->next] = reader->current;
    reader->next = (reader->next + 1U) % reader->capacity;
    if (reader->next == 0U) {
        reader->wrapped = true;
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

static uint32_t archive_reader_finish(struct archive_reader *reader) {
    archive_reader_commit(reader);

    if (!reader->wrapped) {
        return reader->next;
    }

    /*
     * The ring is full and `next` points at its oldest entry, so the buffer has to be rotated
     * left by that much to read oldest-first. Done as three reversals rather than with a
     * scratch copy, because a copy is another `capacity` messages of stack in a function whose
     * caller is already holding that many.
     */
    struct mesh_ui_message *entries = reader->entries;
    const uint32_t n = reader->capacity;
    const uint32_t k = reader->next;
    for (uint32_t lo = 0U, hi = k; lo + 1U < hi; ++lo, --hi) {
        const struct mesh_ui_message tmp = entries[lo];
        entries[lo] = entries[hi - 1U];
        entries[hi - 1U] = tmp;
    }
    for (uint32_t lo = k, hi = n; lo + 1U < hi; ++lo, --hi) {
        const struct mesh_ui_message tmp = entries[lo];
        entries[lo] = entries[hi - 1U];
        entries[hi - 1U] = tmp;
    }
    for (uint32_t lo = 0U, hi = n; lo + 1U < hi; ++lo, --hi) {
        const struct mesh_ui_message tmp = entries[lo];
        entries[lo] = entries[hi - 1U];
        entries[hi - 1U] = tmp;
    }
    return n;
}

/*
 * One pass over a conversation's file into `out`.
 *
 * The line loop is store_file.c's, deliberately: same length, same split on the first '=', same
 * unescape, same tolerance of a comment or a blank. Returns -ENOENT when there is no such file,
 * which every caller treats as an empty conversation rather than as a failure.
 */
static int archive_read_file(const char *path, struct mesh_ui_message *entries, uint32_t capacity,
                             uint32_t *out_count, uint32_t *out_dropped) {
    if (entries == NULL || capacity == 0U || out_count == NULL) {
        return -EINVAL;
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -errno;
    }

    struct archive_reader reader;
    memset(&reader, 0, sizeof reader);
    reader.entries = entries;
    reader.capacity = capacity;

    char line[MESH_UI_ARCHIVE_LINE_MAX];
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
        archive_reader_line(&reader, line, value);
    }

    fclose(file);
    *out_count = archive_reader_finish(&reader);
    if (out_dropped != NULL) {
        *out_dropped = reader.dropped;
    }
    return 0;
}

/* ---- writing a file ------------------------------------------------------------------------- */

/*
 * Replaces a conversation's file with `messages`, through a temporary beside it.
 *
 * The handshake cache is written straight over itself, and this is not, because the two lose
 * different things when a write is interrupted: the cache is a snapshot the next publish
 * rebuilds, and this is history nothing else holds. A rename() on the same directory is atomic,
 * so the file a reader opens is either the old transcript or the new one.
 */
static int archive_rewrite(const char *path, const struct mesh_ui_message *messages,
                           uint32_t count) {
    char temp[sizeof(((struct mesh_ui_archive *)0)->dir) + 64];
    const int named = snprintf(temp, sizeof temp, "%s.tmp", path);
    if (named <= 0 || named >= (int)sizeof temp) {
        return -ENAMETOOLONG;
    }

    FILE *file = fopen(temp, "w");
    if (file == NULL) {
        return -errno;
    }
    for (uint32_t i = 0; i < count; ++i) {
        mesh_ui_store_write_message(file, i, &messages[i]);
    }
    int result = ferror(file) ? -EIO : 0;
    if (fclose(file) != 0) {
        result = -errno;
    }
    if (result != 0) {
        (void)unlink(temp);
        return result;
    }
    if (rename(temp, path) != 0) {
        result = -errno;
        (void)unlink(temp);
        return result;
    }
    return 0;
}

/*
 * Cuts a conversation's file back to its newest records when it has outgrown the cap.
 *
 * Checked on append off the file's size, which is one stat() rather than a read: the read only
 * happens on the append that actually trips the threshold, which for a conversation of ordinary
 * traffic is once every few thousand messages.
 *
 * The buffer is MESH_UI_ARCHIVE_MAX_MESSAGES rather than the window the thread screen reads,
 * which is the point - compacting into the window would make every compaction a truncation of
 * the transcript to what happened to be on screen. It is about 150 KB on the stack, held for
 * the length of one rewrite, as store_file.c holds a whole cache for the length of one load:
 * the client is single-threaded with the main thread's stack under it.
 */
static void archive_compact(const char *path) {
    struct stat info;
    if (stat(path, &info) != 0 || info.st_size <= (off_t)MESH_UI_ARCHIVE_FILE_MAX_BYTES) {
        return;
    }

    struct mesh_ui_message keep[MESH_UI_ARCHIVE_MAX_MESSAGES];
    uint32_t count = 0U;
    if (archive_read_file(path, keep, MESH_UI_ARCHIVE_MAX_MESSAGES, &count, NULL) != 0) {
        return;
    }
    const int result = archive_rewrite(path, keep, count);
    if (result != 0) {
        mesh_log_warn("ui", "Could not compact message archive %s: %d", path, result);
        return;
    }
    mesh_log_info("ui", "Compacted message archive %s to %u messages", path, (unsigned)count);
}

/* Appends whole records to a conversation's file, creating it if it is not there. */
static int archive_append_records(struct mesh_ui_archive *archive, const char *path,
                                  const struct mesh_ui_message *const *messages, uint32_t count) {
    if (count == 0U) {
        return 0;
    }
    FILE *file = fopen(path, "a");
    if (file == NULL) {
        return -errno;
    }
    for (uint32_t i = 0; i < count; ++i) {
        mesh_ui_store_write_message(file, archive->next_index, messages[i]);
        archive->next_index++;
    }
    int result = ferror(file) ? -EIO : 0;
    if (fclose(file) != 0) {
        result = -errno;
    }
    if (result != 0) {
        return result;
    }

    archive_compact(path);
    return (int)count;
}

/* ---- the public half ------------------------------------------------------------------------ */

int mesh_ui_archive_init(struct mesh_ui_archive *archive, const char *dir) {
    if (archive == NULL) {
        return -EINVAL;
    }
    memset(archive, 0, sizeof *archive);
    if (dir == NULL || dir[0] == '\0') {
        return -EINVAL;
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        const int failed = -errno;
        mesh_log_warn("ui", "Message archive unavailable at %s: %d", dir, failed);
        return failed;
    }
    mesh_str_copy(archive->dir, sizeof archive->dir, dir);
    /* Truncation would put the files somewhere other than where the caller asked, so it
       disables the archive rather than writing to a shortened path. */
    if (strcmp(archive->dir, dir) != 0) {
        archive->dir[0] = '\0';
        return -ENAMETOOLONG;
    }
    return 0;
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
    if (archive->dir[0] == '\0') {
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

        char path[sizeof archive->dir + 32];
        if (!archive_path(archive, kind, node, channel, path, sizeof path)) {
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

        const int result = archive_append_records(archive, path, batch, batch_count);
        if (result < 0) {
            mesh_log_warn("ui", "Could not append to message archive %s: %d", path, result);
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
    if (archive->dir[0] == '\0') {
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

        char path[sizeof archive->dir + 32];
        if (!archive_path(archive, kind, node, channel, path, sizeof path)) {
            continue;
        }
        /* A conversation that already has a file has a transcript at least as good as this one,
           and rewriting it with the cache's 64 would be throwing history away to save it. */
        if (access(path, F_OK) == 0) {
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

        const int result = archive_rewrite(path, batch, batch_count);
        if (result != 0) {
            mesh_log_warn("ui", "Could not seed message archive %s: %d", path, result);
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

    char path[sizeof archive->dir + 32];
    if (!archive_path(archive, kind, node, channel, path, sizeof path)) {
        /* No file is possible for this conversation - it is the all-traffic view or the "New
           message" row, or the archive is disabled. An empty window, and `valid` left false so
           the transcript falls back to the flat list rather than drawing nothing. */
        return 0;
    }

    const int result = archive_read_file(path, out->entries, MESH_UI_MAX_THREAD_MESSAGES,
                                         &out->count, &out->dropped);
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
    char path[sizeof archive->dir + 32];
    if (!archive_path(archive, kind, node, channel, path, sizeof path)) {
        return 0;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        return -errno;
    }
    return 0;
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
    FILE *out;
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
static void archive_filter_settle(struct archive_filter *filter, bool is_reaction,
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
    fputs(filter->pending, filter->out);
    fputc('\n', filter->out);
}

/*
 * One line of the file, copied through or dropped.
 *
 * `raw` is the line as it sits on disk, which is what gets written back - re-escaping a value
 * this code never had a reason to decode would be a second spelling of the format, and the
 * whole point of sharing store_keys.h is that there is only one.
 */
static void archive_filter_line(struct archive_filter *filter, const char *raw, char *work) {
    char *equals = strchr(work, '=');
    if (equals == NULL) {
        /* Not a key line at all - a comment, a blank, something hand-added. It belongs to
           nobody, so it is copied rather than attached to whatever record is open. */
        archive_filter_settle(filter, false, 0U);
        fputs(raw, filter->out);
        fputc('\n', filter->out);
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
        archive_filter_settle(filter, false, 0U);
        struct mesh_ui_message opened;
        memset(&opened, 0, sizeof opened);
        if (!mesh_ui_store_read_message_line(&opened, id, value)) {
            /* A torn or hand-edited line opens no record. Kept: this is a delete of one named
               message, not a tidy-up of the file. */
            filter->dropping = false;
            fputs(raw, filter->out);
            fputc('\n', filter->out);
            return;
        }
        mesh_str_copy(filter->pending, sizeof filter->pending, raw);
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
        archive_filter_settle(filter, meta.is_reaction, meta.reply_id);
        if (filter->dropping) {
            return;
        }
        fputs(raw, filter->out);
        fputc('\n', filter->out);
        return;
    }

    /* Any other line: the record it belongs to is settled by now. */
    archive_filter_settle(filter, false, 0U);
    if (mesh_ui_store_key_is_message(id) && filter->dropping) {
        return;
    }
    fputs(raw, filter->out);
    fputc('\n', filter->out);
}

int mesh_ui_archive_forget_message(struct mesh_ui_archive *archive, uint8_t kind, uint32_t node,
                                   uint8_t channel, uint32_t packet_id) {
    if (archive == NULL || packet_id == 0U) {
        return -EINVAL;
    }
    char path[sizeof archive->dir + 32];
    if (!archive_path(archive, kind, node, channel, path, sizeof path)) {
        return 0;
    }

    FILE *source = fopen(path, "r");
    if (source == NULL) {
        return (errno == ENOENT) ? 0 : -errno;
    }

    char temp[sizeof archive->dir + 64];
    const int named = snprintf(temp, sizeof temp, "%s.tmp", path);
    if (named <= 0 || named >= (int)sizeof temp) {
        fclose(source);
        return -ENAMETOOLONG;
    }
    FILE *out = fopen(temp, "w");
    if (out == NULL) {
        const int failed = -errno;
        fclose(source);
        return failed;
    }

    struct archive_filter filter;
    memset(&filter, 0, sizeof filter);
    filter.out = out;
    filter.target = packet_id;

    char raw[MESH_UI_ARCHIVE_LINE_MAX];
    char work[MESH_UI_ARCHIVE_LINE_MAX];
    while (fgets(raw, sizeof raw, source) != NULL) {
        raw[strcspn(raw, "\r\n")] = '\0';
        mesh_str_copy(work, sizeof work, raw);
        archive_filter_line(&filter, raw, work);
    }
    archive_filter_settle(&filter, false, 0U);

    int result = ferror(source) || ferror(out) ? -EIO : 0;
    fclose(source);
    if (fclose(out) != 0) {
        result = -errno;
    }
    if (result != 0) {
        (void)unlink(temp);
        return result;
    }

    if (filter.dropped_records == 0U) {
        /* Nothing to say, so nothing is replaced: a delete that found its message somewhere else
           must not rewrite this file at all. */
        (void)unlink(temp);
        return 0;
    }
    if (rename(temp, path) != 0) {
        result = -errno;
        (void)unlink(temp);
        return result;
    }

    /* The message must not be remembered as written: if the radio delivers the same packet again
       the user should see it arrive rather than have it swallowed by this run's ring. */
    for (uint32_t i = 0; i < archive->recent_count; ++i) {
        if (archive->recent[i].packet_id == packet_id) {
            memset(&archive->recent[i], 0, sizeof archive->recent[i]);
        }
    }
    return (int)filter.dropped_records;
}
