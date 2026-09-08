#pragma once

/*
 * The places the mesh has shared, as data: the rows the Waypoints tab lists and the rows it
 * shows when you open one, in the same shape the node detail uses (label, value already
 * formatted, kind). Backends draw; the nav walks and only ever asks how many rows there are.
 *
 * The tab is useful with no map at all, which is why it exists before one: a waypoint is a
 * name and a point, and what a handheld can say about a point is how far away it is and which
 * way it lies. That answer comes from src/geo/vector.c, so the map layer that arrives later
 * inherits it rather than growing a second copy.
 *
 * Everything here is derived from the snapshot the store already holds - the waypoints
 * themselves, our own node's fix out of the roster, and the radio's metric/imperial preference
 * - so opening the tab costs no radio traffic and works on a cached, disconnected client.
 */

#include "mesh/geo/vector.h"
#include "mesh/ui/store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_UI_WAYPOINT_LABEL_MAX 16U
/* Long enough to hold a whole description, which is the longest thing a value column here can
   be asked for; every other value is a coordinate, an age or a name. */
#define MESH_UI_WAYPOINT_VALUE_MAX MESH_UI_WAYPOINT_DESCRIPTION_MAX
/* "1.2 km NE" and its imperial and metre-scale variants. */
#define MESH_UI_WAYPOINT_RANGE_MAX 24U

/*
 * Every row one waypoint can produce: two headings, five place rows, six sharing rows, a note
 * and two actions is sixteen. Rounded up, and pinned by waypoint_detail_row_budget in the
 * waypoints suite so a new row cannot quietly push the last one off the screen.
 */
#define MESH_UI_WAYPOINT_ITEMS_MAX 24U

/* ---- the list ------------------------------------------------------------------------------ */

/* What a row of the Waypoints tab is. */
enum mesh_ui_waypoint_row_type {
    MESH_UI_WAYPOINT_ROW_PLACE = 0,
    /* The last row: "New waypoint here", which names our own radio's fix. It is a row rather
       than a button on the app bar for the reason the conversation list's "New message" row is
       one - a list whose only way to add to it is a keycap nothing on the screen mentions is a
       list people believe is read-only. */
    MESH_UI_WAYPOINT_ROW_NEW,
};

/* One row of the list, resolved for display. */
struct mesh_ui_waypoint_row {
    uint8_t type; /* enum mesh_ui_waypoint_row_type */
    uint32_t id;  /* PLACE: the waypoint's id, so a cursor names a place rather than a position */
    /* PLACE: the waypoint itself, borrowed from the snapshot the row was built against and
       valid for as long as it is. NULL on the NEW row. */
    const struct mesh_ui_waypoint *waypoint;
    /* The name, or the stand-in for a waypoint whose sender left it empty - which is legal on
       the wire and is what the phone apps send for a pin dropped in a hurry. */
    char name[MESH_UI_WAYPOINT_NAME_MAX];
    /* "1.2 km NE", or empty when there is no answer: no fix of our own, or no coordinates on
       the waypoint. A row with nothing here draws no range rather than drawing a zero. */
    char range[MESH_UI_WAYPOINT_RANGE_MAX];
    /* Who shared it and when, as one line. */
    char shared[48];
    /* Whether this client may withdraw it from the mesh rather than only from itself. */
    bool editable;
    bool ours;
};

/*
 * How many rows the list has: every place, then the "New waypoint here" row. Never 0.
 *
 * Places are ordered nearest first when we have a fix to measure from, because that is the
 * order the list exists to give. Everything unmeasurable - a waypoint with no coordinates, or
 * every waypoint when our own radio has no fix - follows in most-recently-heard order, which is
 * the only other thing that distinguishes one place from another.
 */
uint32_t mesh_ui_waypoint_count(const struct mesh_ui_store *store);

/* Describes row `index`. False past the end. */
bool mesh_ui_waypoint_row(const struct mesh_ui_store *store, uint32_t index,
                          struct mesh_ui_waypoint_row *out);

/* The waypoint with this id in a published list, or NULL - the same "resolve by id, not by
   index" rule the node detail follows, because the list re-ranks as our own fix moves. */
const struct mesh_ui_waypoint *mesh_ui_waypoint_find(const struct mesh_ui_waypoint_list *list,
                                                     uint32_t id);

/*
 * Our own radio's last known fix, out of the published roster.
 *
 * False when we have no node record for ourselves or it carries no position, which is the
 * ordinary state of a radio with no GPS and no fixed position set - and the reason the "New
 * waypoint here" row can be present and unpressable rather than absent. A row that vanishes
 * does not tell anybody why.
 */
bool mesh_ui_waypoint_our_fix(const struct mesh_ui_handshake_state *handshake,
                              int32_t *out_latitude_i, int32_t *out_longitude_i);

/* "1.2 km NE" for the vector between two points, or false when there is no vector to describe.
   `imperial` follows the radio's own display units, so the client and the radio agree. */
bool mesh_ui_waypoint_format_range(int32_t from_latitude_i, int32_t from_longitude_i,
                                   int32_t to_latitude_i, int32_t to_longitude_i, bool imperial,
                                   char *out, size_t out_len);

/* A distance on its own, in the reader's units. Public because the node detail wants the same
   words for the same quantity. */
void mesh_ui_format_distance(double metres, bool imperial, char *out, size_t out_len);

/* ---- one waypoint's detail ------------------------------------------------------------------ */

enum mesh_ui_waypoint_item_kind {
    MESH_UI_WAYPOINT_ITEM_INFO = 0, /* label and value */
    MESH_UI_WAYPOINT_ITEM_HEADING,  /* a group title; no value, not selectable */
    /*
     * The sharer's own description, with no label and the whole width to itself.
     *
     * Its own kind because it is the one thing on this screen that is a sentence rather than a
     * fact: a hundred characters upstream, against a value column sized for a coordinate. A
     * backend draws it as a supporting line under a heading rather than squeezing it into a
     * column it does not fit.
     */
    MESH_UI_WAYPOINT_ITEM_NOTE,
    MESH_UI_WAYPOINT_ITEM_ACTION, /* A does something; `action` says what */
};

enum mesh_ui_waypoint_action {
    MESH_UI_WAYPOINT_ACTION_NONE = 0,
    /* Broadcast it again, unchanged. What a place shared before this client connected needs,
       and the only way to hand a waypoint to a node that has just joined. */
    MESH_UI_WAYPOINT_ACTION_SHARE,
    /* Withdraw it. Whether that reaches the mesh or only this client is `editable` on the
       waypoint, and the row says which before it is pressed rather than after. */
    MESH_UI_WAYPOINT_ACTION_DELETE,
};

struct mesh_ui_waypoint_item {
    char label[MESH_UI_WAYPOINT_LABEL_MAX];
    char value[MESH_UI_WAYPOINT_VALUE_MAX];
    uint8_t kind;   /* enum mesh_ui_waypoint_item_kind */
    uint8_t action; /* enum mesh_ui_waypoint_action */
};

/*
 * Fills `out` with one waypoint's rows and returns how many were written (at most `capacity`).
 *
 * `handshake` supplies our own fix and the names of the nodes involved; it may be NULL, and the
 * rows that need it are then left out rather than guessed at. `now` is the wall clock used to
 * age stamps - pass 0 to leave ages out, which is what a Brick with no clock wants, and which
 * is also what stops an expiry row claiming a place has outlived a date we cannot read.
 *
 * `delete_armed` is the nav's "the next press really does it" state for the delete row, which
 * is the one row here whose consequence cannot be walked back: the place leaves the list, and
 * on the mesh's copy too. It only changes what that row's value column says.
 */
uint32_t mesh_ui_waypoint_detail_build(const struct mesh_ui_waypoint *waypoint,
                                       const struct mesh_ui_handshake_state *handshake,
                                       const struct mesh_ui_settings *settings, uint32_t now,
                                       bool delete_armed, struct mesh_ui_waypoint_item *out,
                                       uint32_t capacity);

#ifdef __cplusplus
}
#endif
