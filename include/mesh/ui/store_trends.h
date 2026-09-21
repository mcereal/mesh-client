#ifndef MESH_UI_STORE_TRENDS_H
#define MESH_UI_STORE_TRENDS_H

/*
 * What a node's readings have been doing, on the card: one append-only log per node, and the
 * third file this client keeps.
 *
 * `struct mesh_ui_history` is what the client has watched happen, and until this existed the
 * whole of it died at exit except the radio's own airtime pair - which rides the handshake cache
 * because that file is rewritten whole on every save and six hours of one-minute readings fits
 * in it. A node's readings do not: twelve nodes times five readings times two dozen samples is a
 * different kind of number, and it would be rewritten in full every time a read mark moved.
 *
 * So this is the archive's shape applied to a different record (mesh/ui/store_archive.h states
 * that shape; docs/ui.md sets the three files side by side):
 *
 *   mesh/ui/store_keys.def   the handshake cache  - one file, rewritten whole, the live view
 *   mesh/ui/store_archive.h  the transcript       - a file per conversation, appended to
 *   here                     the trend log        - a file per node, appended to
 *
 * It shares their line syntax and their escape, over the same key table, and it shares their
 * rule that a file is capped by rewriting rather than by refusing to grow. What is different is
 * the record, and everything interesting here follows from one fact about it.
 *
 * **A reading's time is not a time.** Every stamp in the history is the client's own monotonic
 * clock, which counts from boot and means nothing to the next run - and a Brick has no RTC, so
 * there is no second clock to write down instead. The cache gets away with it by writing *ages*
 * relative to its newest sample, which it can do because it rewrites the whole file every save.
 * An append cannot: the record written an hour ago is already on the card and its age is already
 * wrong.
 *
 * So a record carries the time since the record *above* it, and the reader adds them up. Three
 * things fall out of that, and they are the whole design:
 *
 *   - **A run's first record carries the seam rather than a measurement.** How long the client
 *     was not running is the one thing nothing on this device can measure, so the first append
 *     of a run writes MESH_UI_HISTORY_NODE_GAP_MS - the shortest silence that is already a break
 *     - and marks the record as one that continues nothing. That is
 *     mesh_ui_history_resume()'s rule for the radio's pair, written into the format instead of
 *     into a call.
 *   - **Every reading of one node shares one chain.** A node's battery and its temperature are
 *     interleaved in one file and the deltas run over the file as a whole, so the two come back
 *     on one timeline - which is what the node detail's SNR and RSSI rows need, since they are
 *     two measurements of one packet and are drawn against one axis.
 *   - **The reader is the one that bounds the span.** Deltas accumulate, and a uint32 of
 *     milliseconds is seven weeks, so a long-lived file read from the top would overflow the
 *     clock it is being put back onto. The reader keeps the newest MESH_UI_TRENDS_MAX_RECORDS
 *     and then drops whatever is more than MESH_UI_TRENDS_SPAN_MAX_MS behind the last of them,
 *     which costs nothing real: a series holds two dozen samples, and at a node's half-hourly
 *     cadence that is half a day.
 *
 * The other half of the design is *when*. The transcript is read when a conversation is opened;
 * this is read when a node's detail screen is opened, and for the same reason - it is the
 * expensive half, it changes only when the reader moves, and only one node's trend is ever
 * drawn. Unlike the transcript it is not merged with the live half but *replaces* it: the log
 * already holds everything this run has pushed, because the append below runs on every publish.
 * See mesh_ui_trends_restore() and mesh_ui_history_restore_node_reset().
 */

#include "mesh/ui/history.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Nodes whose write state this run tracks.
 *
 * Not a cap on the files - every node the client has ever kept a trend for has one - but on how
 * many the *writer* is holding a delta chain for at once. The writer only ever appends for a
 * node the history has a slot for, so MESH_UI_HISTORY_NODES is the real number and this is it
 * with slack: a slot whose node has left the history is dropped on the next append, which is
 * what keeps the slack from ever being spent.
 */
#define MESH_UI_TRENDS_NODES 16U

/*
 * Records one node's file keeps once it has been compacted.
 *
 * Deliberately more than a restore can use. Five readings at INKCELL_SERIES_MAX apiece is 120
 * records, and that is a node reporting everything it possibly could; the rest is headroom so
 * that a file cut back is not immediately a file that has lost a reading. Both the reader's
 * buffer and the rewrite are this size, which is about 4.5 KB of stack while one runs.
 */
#define MESH_UI_TRENDS_MAX_RECORDS 384U

/*
 * When a file is rewritten, in bytes.
 *
 * Above the worst case for MESH_UI_TRENDS_MAX_RECORDS records rather than at the typical case,
 * for the reason MESH_UI_ARCHIVE_FILE_MAX_BYTES is: a threshold the worst case could exceed
 * would compact again on the very next append, and on a card mounted sync that is a rewrite per
 * reading. A record is about 24 bytes and at most 45, so 384 of them cannot reach 17 KB.
 */
#define MESH_UI_TRENDS_FILE_MAX_BYTES (32U * 1024U)

/*
 * How far back a restored trend may reach.
 *
 * The bound that keeps the accumulated deltas inside the clock they are put back onto: a restore
 * places its oldest reading this far behind the live stamp, and MESH_UI_HISTORY_EPOCH_MS is
 * where the clock starts so that it can. Seven days against eight is the margin.
 *
 * Nothing real is lost to it. A series holds INKCELL_SERIES_MAX samples, so a week of them is a
 * reading every seven hours - well past the point where what is being drawn is a trend.
 */
#define MESH_UI_TRENDS_SPAN_MAX_MS (7U * 24U * 60U * 60U * 1000U)

/*
 * One node's delta chain, as the writer holds it.
 *
 * `written_at` is on the history's own timeline and is the whole of what "have I written this
 * already" means here - there is no id on a reading to deduplicate against, so the rule is that
 * a sample later than the last one written is new and everything else has been seen. It is what
 * the next record's delta is measured from, which is why a restore sets it too: the restore has
 * just established that the file runs up to a particular moment.
 */
struct mesh_ui_trends_node {
    uint32_t node_id; /* 0 for a free slot */
    uint32_t written_at;
    bool written; /* this run has put a record in this node's file */
};

/*
 * The trend log's in-RAM half: where the files are, which radio they are about, and how far each
 * one has been written.
 *
 * Deliberately not a handle on an open file, for mesh_ui_archive's reason: an append is one
 * fopen("a") and one fclose, on a device whose card can be pulled and whose client is killed
 * with SIGKILL by its own deploy script.
 */
struct mesh_ui_trends {
    /* The directory the files live in, with no trailing slash. Empty disables the log entirely,
       which is what a client with nowhere to write runs as. */
    char dir[512];
    /*
     * The radio these files are about.
     *
     * Node numbers are the mesh's rather than the radio's, but a different radio is a different
     * mesh - so a trend carried across a swap would draw one node's battery as another's. The
     * history drops everything for that reason (mesh_ui_history_forget); the card has to be told
     * the same thing, and this is what notices. Seeded from the handshake cache at startup, so a
     * client relaunched against a different radio is the same event as one swapped mid-run.
     */
    uint32_t roster_owner;
    bool has_owner;
    struct mesh_ui_trends_node nodes[MESH_UI_TRENDS_NODES];
};

/*
 * Points the log at `dir`, creating it if it is not there.
 *
 * Returns 0, or a negative errno. A failure leaves the log disabled rather than unusable: every
 * call below then does nothing and reports success, because a Brick that cannot write a trend is
 * still a client - the live history is unaffected, and what is lost is only the part of it that
 * would have survived the next restart.
 */
int mesh_ui_trends_init(struct mesh_ui_trends *trends, const char *dir);

/*
 * Says which radio the client is attached to now, and drops every file if it is a different one.
 *
 * Returns true when it dropped them, so a caller holding anything derived from a restored trend
 * knows to let go of it. A `roster_owner` of 0 is "no radio yet" and says nothing; the first
 * real one is remembered rather than treated as a swap, exactly as mesh_ui_store_note_roster()
 * treats it.
 */
bool mesh_ui_trends_note_radio(struct mesh_ui_trends *trends, uint32_t roster_owner);

/*
 * Writes whatever the history holds that this run has not written yet.
 *
 * Walks the node slots, gathers every sample later than that node's high-water mark, and appends
 * them in time order so that one file is one chain - a battery and a temperature that arrived in
 * the same publish are two records, in the order they were taken.
 *
 * Called on every publish rather than at a save, which is the point: `deploy-stop` sends SIGKILL
 * and a battery can be pulled, so a reading that reached disk only at a clean exit would be a
 * reading that usually did not.
 *
 * Returns how many records were written, or a negative errno if nothing could be.
 */
int mesh_ui_trends_append(struct mesh_ui_trends *trends, const struct mesh_ui_history *history);

/*
 * Puts one node's saved trend back into `history`, replacing whatever it holds for that node.
 *
 * A replacement rather than a merge, and that is safe for one reason: mesh_ui_trends_append()
 * runs on every publish, so the file already holds this run's readings too. The alternative -
 * folding the card's copy in under the live one - would need an identity for a reading, and a
 * reading has none.
 *
 * The saved readings are laid out ending where this run's newest reading about this node already
 * sits, so a trend that was never interrupted is not drawn with a seam in it. When there is no
 * such reading - the ordinary case, a node whose detail is opened before it has said anything
 * this run - they end a gap short of the live clock and the next reading starts a segment of its
 * own, because how long ago the last saved one was taken is exactly what a client with no RTC
 * cannot know.
 *
 * `now_ms` is the caller's clock, as a note_* takes it. Returns how many readings came back
 * (0 for a node with no file, which is not a failure), or a negative errno.
 */
int mesh_ui_trends_restore(struct mesh_ui_trends *trends, uint32_t node_id,
                           struct mesh_ui_history *history, uint32_t now_ms);

/*
 * Removes every file, and forgets every chain. Returns how many went, or a negative errno.
 *
 * The only thing that drops anything here, and that is the rule rather than an omission: **the
 * log follows the history, not the roster.** Removing a node or emptying the cached roster are
 * edits to what the client knows about the *mesh* and neither clears the history, so neither
 * clears this - a node removed and heard again keeps the trend it had, exactly as its slot in
 * the history does. A different radio is the one event that invalidates a reading, because it is
 * the one that makes a node number mean something else.
 */
int mesh_ui_trends_forget(struct mesh_ui_trends *trends);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_STORE_TRENDS_H */
