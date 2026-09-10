#ifndef MESH_UI_HISTORY_H
#define MESH_UI_HISTORY_H

/*
 * What the client has been told, and when - as opposed to the snapshot, which is what is true
 * now.
 *
 * Every other structure the store publishes is the present tense: this many nodes, this much of
 * the air in use, this battery at this percent. That is the whole of what the radio sends, and
 * it is why nothing on this panel could answer a question about a *direction* - the reader had
 * to have looked before and remembered. The sparkline needed somewhere for the client to
 * remember instead, and this is it: a small, bounded set of `struct mesh_ui_series`
 * (layout.h) filled as the readings arrive.
 *
 * Three decisions, because each of them is a way this could have been bigger and worse:
 *
 *   - **It keeps only what a screen draws.** Two series about the radio we are attached to, and
 *     one battery per node, for a bounded number of nodes. Not every reading, not every node:
 *     a general store of everything the mesh ever said is a database, and the screens that
 *     would justify one do not exist.
 *   - **It is never persisted.** The store's cache carries the node roster across restarts on
 *     purpose - a roster is what we know about the mesh - but a trend is what we *watched*, and
 *     the gap where the client was not running is not a silence it can draw. A trend that
 *     resumed across a restart would put a line over a period nothing observed.
 *   - **Nothing here knows what a node is.** The store walks the roster and hands over
 *     readings; this holds series and decides which ones are worth a slot. That is what lets
 *     store.h include this rather than the other way round, and it is the same seam
 *     mesh_ui_signal_level() sits on - what a number means is one layer, what it is a number
 *     *about* is another.
 */

#include "mesh/ui/layout.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Nodes we keep a battery trend for.
 *
 * A screen budget, not a mesh limit, exactly as MESH_UI_MAX_HANDSHAKE_NODES is: only the node
 * whose detail screen is open is ever drawn, and a slot exists so that the trend is already
 * there when it is opened rather than starting from nothing. Twelve is comfortably more than
 * the nodes anybody keeps an eye on, and the least recently heard from is what a thirteenth
 * costs.
 */
#define MESH_UI_HISTORY_NODES 12U

/*
 * How long a silence has to be before a line breaks rather than sloping across it, per source.
 *
 * Both are a few times the interval the readings actually arrive on, so an ordinary late report
 * is still a slope and a link that was down is a gap. LocalStats is the radio talking to the
 * client it is attached to and comes every few minutes; a node's device metrics ride the
 * telemetry broadcast, which is half an hour at the firmware's default and longer on anything
 * running on a battery it cares about.
 */
#define MESH_UI_HISTORY_RADIO_GAP_MS (15U * 60U * 1000U)
#define MESH_UI_HISTORY_NODE_GAP_MS (2U * 60U * 60U * 1000U)

struct mesh_ui_history_node {
    uint32_t node_id; /* 0 for a free slot */
    uint32_t seen;    /* the clock at the last push, so the least recent slot can be evicted */
    struct mesh_ui_series battery;
};

struct mesh_ui_history {
    /* The radio we are attached to, from its own LocalStats report. Both in permille of the
       air, which is the unit mesh_ui_percent_permille() puts the wire's floats on and the unit
       the Status card's meter already reads. */
    struct mesh_ui_series channel_utilization;
    struct mesh_ui_series air_util_tx;
    struct mesh_ui_history_node nodes[MESH_UI_HISTORY_NODES];
};

/* Empties everything and states the per-source gaps. Call before the first note. */
void mesh_ui_history_reset(struct mesh_ui_history *history);

/*
 * The radio's own report of how much of the air is in use and how much of that is ours, at
 * `now_ms` on the client's clock.
 *
 * One call for the pair rather than one per series, because they arrive in one LocalStats and
 * two series stamped a publish apart would draw two readings of one moment as two moments.
 */
void mesh_ui_history_note_airtime(struct mesh_ui_history *history, uint32_t now_ms,
                                  int32_t utilization_permille, int32_t tx_permille);

/*
 * A node's battery level, in whole percent as the radio reports it (101 is "plugged in", which
 * is not a level and is refused here rather than drawn as a reading above full).
 *
 * A node with no slot takes the least recently heard from, which is the one whose trend has
 * least left to say. Our own node is a node like any other here: it reports its battery through
 * the same telemetry every other node does.
 */
void mesh_ui_history_note_battery(struct mesh_ui_history *history, uint32_t now_ms,
                                  uint32_t node_id, uint8_t battery_level);

/*
 * Whether the radio's airtime has been reported often enough to draw a line between.
 *
 * A drawable *segment* rather than two readings, which is not the same test and is the way this
 * was first written wrong: every sample that follows a silence the series calls a break starts a
 * line rather than continuing one, so two reports either side of a link that was down for a
 * quarter of an hour are two samples the ring holds and no stroke at all. Counted, the verb
 * offers a chart with axes, a legend and nothing between them.
 *
 * It is a question of the history rather than of a series because three places ask it - the verb
 * table that offers the chart, the action bar that names the press, and the clamp that closes
 * the screen when it empties - and three of them working it out by hand is three chances to
 * disagree about whether a screen exists.
 */
bool mesh_ui_history_has_airtime(const struct mesh_ui_history *history);

/* That node's battery trend, or NULL when nothing has been kept for it. Borrowed: it lives as
   long as the history does, which for a snapshot is the frame being drawn from it. */
const struct mesh_ui_series *mesh_ui_history_battery(const struct mesh_ui_history *history,
                                                     uint32_t node_id);

/*
 * Drops everything about the radio and its mesh.
 *
 * A different radio is a different mesh with different nodes behind the same node numbers, so
 * carrying a trend across a swap would draw one node's battery as another's. Called for the
 * same event mesh_session_forget_nodes() is, and for the same reason.
 */
void mesh_ui_history_forget(struct mesh_ui_history *history);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_HISTORY_H */
