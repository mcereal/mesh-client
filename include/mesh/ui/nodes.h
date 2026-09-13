#pragma once

#include "mesh/i18n/strings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_handshake_state;
struct mesh_ui_node_summary;

/*
 * ---- the Nodes list's filter ------------------------------------------------------------------
 *
 * Which of the roster the list is showing, as a table read by everything that has an opinion
 * about it - src/ui/nav.c for the row count and the row-to-node mapping, src/ui/actions.c for
 * the verb the press gets named with, and the backend for the chips it draws. That is
 * src/ui/status.c's shape one tab over, and it is here for status.c's reason: three files
 * walking the same list is how the chip under the cursor and the rows under the chip come to
 * disagree about what "Direct" means.
 *
 * Why a filter exists at all. The roster deliberately outlives the connection and the radio's
 * NodeDB evicts, so this list is the one screen in the client that grows without bound - a busy
 * mesh publishes the best 128 of 256 and the reader is looking for one of them. Sorting cannot
 * answer that (the list is already ranked, and a rank is what put the node they want at 94), and
 * a search needs the on-screen keyboard for a name the reader may not know how to spell. What
 * they do know is which *kind* of node they are after, and there are only ever two kinds worth
 * asking for: the ones they chose to keep, and the ones that are actually in earshot.
 *
 * So the set is deliberately three and closed. Every candidate for a fourth failed the same
 * test - a filter has to be a question a reader arrives with, not a column the client happens to
 * hold. "Positioned" is the map's roster and the map row is two rows away; "off radio" is a
 * state the rows already say in the column where a signal would be; "has telemetry" is a fact
 * about a node nobody goes looking for a node by.
 */
enum mesh_ui_node_filter {
    /* Everything published, in the rank the app put it in. The list as it has always been. */
    MESH_UI_NODE_FILTER_ALL = 0,
    /*
     * Heard directly: no relay, no MQTT, and a reading behind it.
     *
     * mesh_ui_node_signal_heard() is the whole of the test rather than a looser one written
     * here, and that matters more than it looks. A node reached over hops has an SNR that
     * describes the last relay, one arriving over MQTT describes nothing on the air at all, and
     * an unset `hops_away` is the firmware declining to say rather than a zero - so a filter
     * that asked `hops_away == 0` would answer "in earshot" for every node the firmware was
     * quiet about. The list already refuses to draw a staircase on those three, for exactly
     * this reason, so a filter with its own opinion would put a node in "Direct" and then draw
     * it with no signal.
     */
    MESH_UI_NODE_FILTER_DIRECT,
    /*
     * Pinned: the nodes the reader chose to keep at the top.
     *
     * "Pinned" rather than "favourites" because X is called pin, the row wears
     * MESH_UI_ICON_PINNED, and a third word for one fact is the thing the action bar's table
     * exists to stop.
     */
    MESH_UI_NODE_FILTER_PINNED,
    MESH_UI_NODE_FILTER_COUNT,
};

/* Whether this node belongs in that filter. NULL is in nothing, including ALL. */
bool mesh_ui_node_filter_matches(const struct mesh_ui_node_summary *node,
                                 enum mesh_ui_node_filter filter);

/* How many of the published roster the filter keeps. */
uint32_t mesh_ui_node_filter_count(const struct mesh_ui_handshake_state *handshake,
                                   enum mesh_ui_node_filter filter);

/*
 * The `index`-th node the filter keeps, or NULL past the end.
 *
 * The counterpart of mesh_ui_node_detail_at() and the only way a row becomes a node while a
 * filter is on. It walks rather than indexing because a filter is a predicate over a list that
 * is re-ranked on every publish: there is nothing to precompute that would not be stale by the
 * next frame.
 */
const struct mesh_ui_node_summary *mesh_ui_node_filter_at(
    const struct mesh_ui_handshake_state *handshake, enum mesh_ui_node_filter filter,
    uint32_t index);

/* The next filter along, wrapping. What A on the filter row does - the settings enum row's step,
   and the reason the chips need no second key. */
enum mesh_ui_node_filter mesh_ui_node_filter_step(enum mesh_ui_node_filter filter, int delta);

/* The chip's word. */
enum mesh_str_id mesh_ui_node_filter_label(enum mesh_ui_node_filter filter);

#ifdef __cplusplus
}
#endif
