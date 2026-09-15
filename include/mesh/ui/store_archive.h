#ifndef MESH_UI_STORE_ARCHIVE_H
#define MESH_UI_STORE_ARCHIVE_H

/*
 * The transcript on the card: one append-only log file per conversation, and the only part of
 * this client that remembers more than the radio does.
 *
 * Everything else about a message is sized against the transport ring - 64 entries, shared by
 * every conversation at once, which on a busy mesh is less than a day and on a very busy one is
 * an afternoon. That is the right size for the *live view*: the conversation list wants the
 * newest line of each conversation and the all-traffic screen is a window on the radio. It is
 * the wrong size for a transcript, and it was also what the card held, because the handshake
 * cache wrote the same 64 entries the store carried. What was in RAM and what was on disk were
 * one number, and a chatty channel spent it on behalf of every quiet one.
 *
 * So this is a second format with a different shape and a different job:
 *
 *   mesh/ui/store_keys.def   the handshake cache  - one file, rewritten whole, the live view
 *   here                     the archive          - a file per conversation, appended to
 *
 * They share their line syntax, their escaping and their readers on purpose (the writers in
 * store_keys.h, the field lists in store_fields.h), so a record means the same thing in both
 * and a person reading either file is reading one format. What they do not share is the
 * compatibility rule: the cache is rewritten every save, so a key it stops writing is simply
 * gone, while an archive file holds records written by every build that ever ran on this card.
 *
 * Three things about the format are decisions rather than details:
 *
 *   - **The row index groups a record; it does not number one.** A record is five lines -
 *     msg[i], msg_meta[i], msg_name[i], msg_relay[i], msg_text[i] - and `i` exists so the four
 *     detail lines can be told to belong to the msg[] line above them. It is a counter that
 *     restarts at zero every run, because the alternative is knowing how many records the file
 *     already holds, and knowing that means reading the whole file before the first append.
 *     The reader's rule is therefore "a msg[] line always begins a record", which is what makes
 *     an index that repeats across a restart harmless.
 *   - **Only traffic from the transport ring is appended.** A message restored from the
 *     handshake cache was archived by the run that heard it, so re-appending it on the next
 *     start would duplicate the whole transcript once per launch. This is also why the archive
 *     needs no read to deduplicate against: what it has already written this run is a ring of
 *     packet ids in RAM, and what it wrote in an earlier run cannot reach it again. A radio
 *     that re-delivers a packet id across a restart is the one case that slips through, and the
 *     reader drops it - see mesh_ui_archive_load_thread().
 *   - **A file is capped by rewriting it, not by refusing to grow.** Compaction happens on
 *     append, off the file's size, and keeps the newest MESH_UI_ARCHIVE_MAX_MESSAGES records -
 *     the cap, not the window the thread screen reads, so a compaction is never a truncation of
 *     the transcript to what was on screen. The threshold is set above what that many records
 *     can possibly occupy, so a compacted file is never immediately over it again: a cap that
 *     could thrash would rewrite the transcript on every message, on a card mounted sync.
 *
 * See docs/ui.md for how this sits beside the cache, and mesh/ui/store_message.h for the window
 * the thread screen draws from.
 */

#include "mesh/ui/store_message.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How many messages one conversation keeps on the card once its file has been compacted.
 *
 * Eight times the flat list and twice the window that reads it, so scrolling to the top of a
 * thread is not scrolling to the end of what was kept. It is a per-conversation number, which
 * is the whole point: a channel that fills its own log has taken nothing from anybody else's.
 *
 * A file ordinarily holds far more than this - compaction only fires when it outgrows the byte
 * budget below, which for traffic of an ordinary size is thousands of records - so this is the
 * floor a conversation is cut back to rather than the ceiling it lives at. Both compaction and
 * the per-message delete read through a buffer of this size, which is where the number is paid
 * for: MESH_UI_ARCHIVE_MAX_MESSAGES * sizeof(struct mesh_ui_message) of stack, briefly.
 */
#define MESH_UI_ARCHIVE_MAX_MESSAGES 512U

/*
 * When a file is rewritten, in bytes.
 *
 * Deliberately above the worst case for MESH_UI_ARCHIVE_MAX_MESSAGES records rather than at the
 * typical case for them: a record is about 150 bytes of ordinary Meshtastic traffic and a little
 * over 1200 in the worst case the format allows (a full 233-byte payload in which every byte
 * needs escaping, plus two names in the same state). Under a threshold the worst case could
 * exceed, compaction would fire again on the very next append and go on firing - which on a card
 * mounted sync is a rewrite of the transcript per message. So the slack is the feature.
 */
#define MESH_UI_ARCHIVE_FILE_MAX_BYTES (768U * 1024U)

/* How many conversations the in-RAM side tracks. The read-mark table's number, for the read
   mark's reason: it is the same "conversations anyone realistically keeps in view". */
#define MESH_UI_ARCHIVE_CONVERSATIONS_MAX 32U

/* Messages already written this run, so one sitting in the transport ring across several
   publishes is appended once. Four times the ring, so a message stays remembered for well past
   the point the ring itself has let it go. */
#define MESH_UI_ARCHIVE_RECENT_MAX 256U

/*
 * What this run has written, and in what state.
 *
 * A packet id is not an identity on its own. MeshPacket.id only has to be unique *per sender*
 * for a few minutes (see mesh_session_next_packet_id), and a channel's log holds every sender
 * on that channel - so two nodes can legitimately land on the same id, and a check that
 * compared ids alone would decide the second one had already been written and drop it. The peer
 * and the direction name the other end and which end that is, which between them separate two
 * senders inside one conversation's file.
 *
 * The delivery state is here because it is the one thing about a message that *changes* after
 * it is first seen: an outbound message is published pending and acknowledged a few seconds
 * later. Without it the archive would keep the pending copy for good, and after a restart a
 * message that had actually failed would read as still in flight - with no resend offered on
 * it, because the bar asks whether the ack is FAILED. A changed ack is therefore not a message
 * already written: it is appended again, and the reader folds the later copy onto the earlier
 * one (see mesh_ui_archive_load_thread).
 */
struct mesh_ui_archive_recent {
    uint32_t packet_id;
    uint32_t peer;
    uint8_t direction; /* enum mesh_message_direction */
    uint8_t ack;       /* enum mesh_message_ack */
    uint8_t ack_error; /* meshtastic_Routing_Error behind a FAILED ack */
};

/*
 * The archive's in-RAM half: where the files are, and what has already been written to them.
 *
 * Deliberately not a handle on an open file. Every operation opens, does its work and closes,
 * because the alternative on this device is a descriptor held across a card that can be pulled
 * and a client that can be killed with SIGKILL by its own deploy script. An append is one
 * fopen("a") and one fclose, which is also what makes the format safe to have two builds
 * writing: a record is whole or it is the tail the reader discards.
 */
struct mesh_ui_archive {
    /* The directory the files live in, with no trailing slash. Empty disables the archive
       entirely, which is what a client with nowhere to write runs as. */
    char dir[512];
    /* The record index handed to the next append - a grouping counter, not a count of what is
       on disk. See the header comment. */
    uint32_t next_index;
    /* Messages appended this run, oldest first, wrapping. A zero packet id is "no id" and never
       matches, so a slot cleared to zero is a slot that holds nothing. */
    struct mesh_ui_archive_recent recent[MESH_UI_ARCHIVE_RECENT_MAX];
    uint32_t recent_count;
    uint32_t recent_next;
    /* Conversations whose file has been seen to exist this run, so the seed below runs once per
       conversation rather than once per publish. */
    struct {
        uint8_t kind;
        uint8_t channel;
        uint32_t node;
    } seeded[MESH_UI_ARCHIVE_CONVERSATIONS_MAX];
    uint32_t seeded_count;
};

/*
 * Points the archive at `dir`, creating it if it is not there.
 *
 * Returns 0, or a negative errno. A failure leaves the archive disabled rather than unusable:
 * every call below then does nothing and reports success, because a Brick that cannot write a
 * transcript is still a client, exactly as one that cannot write a crash report is.
 */
int mesh_ui_archive_init(struct mesh_ui_archive *archive, const char *dir);

/*
 * Writes the messages in `list` that this run has not already written.
 *
 * `list` is the *live* half of a publish - what the transport ring is holding - and never the
 * merged list the store carries. Handing it the merged list would append the restored history
 * again on every start; see the header.
 *
 * Returns how many records were written, or a negative errno if nothing could be.
 */
int mesh_ui_archive_append(struct mesh_ui_archive *archive,
                           const struct mesh_ui_message_list *list);

/*
 * Writes `list` into the files of conversations that have none yet, and nothing into the rest.
 *
 * The upgrade path, called once at startup with the history restored from the handshake cache:
 * a card that has been running an older build has 64 messages in the cache and no archive at
 * all, and without this their transcript would begin at the moment of the upgrade. A
 * conversation whose file already exists is left entirely alone, which is what makes this safe
 * to call on every start.
 *
 * Returns how many records were written, or a negative errno.
 */
int mesh_ui_archive_seed(struct mesh_ui_archive *archive, const struct mesh_ui_message_list *list);

/*
 * Fills `out` with the newest messages of one conversation, oldest first.
 *
 * `kind` is an enum mesh_ui_conversation_kind: CHANNEL reads `channel`, DIRECT reads `node`.
 * `out->dropped` is set to however many records the file held that the window had no room for,
 * so the transcript can say there is more behind it.
 *
 * A record whose packet id already appeared is folded onto the earlier one rather than drawn
 * twice - the newer copy's contents win, since it carries the later delivery state, and the
 * earlier copy's position is kept, since that is where the conversation actually reached it.
 *
 * Returns 0 (including for a conversation with no file, which yields an empty window), or a
 * negative errno.
 */
int mesh_ui_archive_load_thread(const struct mesh_ui_archive *archive, uint8_t kind, uint32_t node,
                                uint8_t channel, struct mesh_ui_thread *out);

/* Removes a conversation's file. Returns 0, including when there was nothing to remove. */
int mesh_ui_archive_forget_conversation(struct mesh_ui_archive *archive, uint8_t kind,
                                        uint32_t node, uint8_t channel);

/*
 * Rewrites a conversation's file without the message carrying `packet_id`, or the reactions
 * that named it.
 *
 * A rewrite rather than a tombstone, because the record has to be gone rather than marked: this
 * is the press that answers "delete this", and a transcript that kept what the reader deleted
 * and merely stopped drawing it would be the one thing that press must not do.
 *
 * Returns how many records went, or a negative errno.
 */
int mesh_ui_archive_forget_message(struct mesh_ui_archive *archive, uint8_t kind, uint32_t node,
                                   uint8_t channel, uint32_t packet_id);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_STORE_ARCHIVE_H */
