#pragma once

/*
 * One node as data: the rows the Nodes tab shows when you open a node, in the same shape the
 * Settings tab uses (label, value already formatted, kind). Backends draw the list; the nav
 * walks it and only ever asks how many rows there are.
 *
 * Everything here comes from the node record the session already holds - the NodeDB sync fills
 * it and NODEINFO/POSITION/TELEMETRY packets keep it current - so opening a node costs no
 * radio traffic and works just as well on a cached, disconnected node list.
 *
 * Rows are built rather than indexed: which of them exist depends on what the node has
 * actually reported, so a builder that emits the list in one pass is the only place the layout
 * lives. The backend builds once per frame and the nav asks for the count.
 */

#include "mesh/ui/store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_UI_NODE_LABEL_MAX 20U
#define MESH_UI_NODE_VALUE_MAX 48U
/* Every row every node can produce: five headings, the action, and the widest set of
   readings a sensor node reports. */
#define MESH_UI_NODE_ITEMS_MAX 48U

enum mesh_ui_node_row_kind {
    MESH_UI_NODE_ROW_INFO = 0, /* label and value */
    MESH_UI_NODE_ROW_HEADING,  /* a group title; no value, not selectable */
    MESH_UI_NODE_ROW_ACTION,   /* A does something; `action` says what */
};

enum mesh_ui_node_action {
    MESH_UI_NODE_ACTION_NONE = 0,
    MESH_UI_NODE_ACTION_MESSAGE,  /* open this node's conversation */
    MESH_UI_NODE_ACTION_FAVORITE, /* pin or unpin the node in the radio's NodeDB */
};

struct mesh_ui_node_item {
    char label[MESH_UI_NODE_LABEL_MAX];
    char value[MESH_UI_NODE_VALUE_MAX];
    uint8_t kind;   /* enum mesh_ui_node_row_kind */
    uint8_t action; /* enum mesh_ui_node_action */
};

/*
 * Fills `out` with the node's rows and returns how many were written (at most `capacity`).
 * `is_self` drops the rows that make no sense for our own node. `now` is the wall clock used
 * to age timestamps; pass 0 to leave ages out, which is what a Brick with no clock wants.
 */
uint32_t mesh_ui_node_detail_build(const struct mesh_ui_node_summary *node, bool is_self,
                                   uint32_t now, struct mesh_ui_node_item *out, uint32_t capacity);

/* Rows the node would produce. The nav needs nothing else from this module. */
uint32_t mesh_ui_node_detail_count(const struct mesh_ui_node_summary *node, bool is_self);

/*
 * The node with that id, or NULL when it is not in the list. The open detail is remembered by
 * id rather than by row because app.c re-ranks the node list on every publish (by last_heard,
 * which changes constantly) - a row index would quietly slide onto a different node while the
 * user was reading one.
 */
const struct mesh_ui_node_summary *
mesh_ui_node_detail_find(const struct mesh_ui_handshake_state *handshake, uint32_t node_id);

/* The node on row `row` of the Nodes list, or NULL past the end. */
const struct mesh_ui_node_summary *
mesh_ui_node_detail_at(const struct mesh_ui_handshake_state *handshake, uint32_t row);

#ifdef __cplusplus
}
#endif
