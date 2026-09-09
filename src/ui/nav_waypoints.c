#define _POSIX_C_SOURCE 200809L

/*
 * The Waypoints tab's own key handling: the list, the place opened from it, and the keyboard
 * that names a new one.
 *
 * The shape is the Nodes tab's, deliberately - a list, one of its rows opened over it, and
 * actions inside the open row - because a place and a node are the same kind of thing to
 * navigate even though they are nothing alike to read. What is different is the last row of the
 * list, which makes a place rather than opening one, and the keyboard that row raises.
 */

#include "nav_internal.h"

#include "mesh/ui/waypoints.h"
#include "mesh/utils/time.h"

#include <stdio.h>
#include <string.h>

void mesh_ui_nav_open_waypoint(struct mesh_ui_nav *nav, uint32_t id) {
    if (!nav->waypoint_detail_open) {
        nav->waypoint_list_cursor = nav->cursor[MESH_UI_SCREEN_WAYPOINTS];
    }
    nav->waypoint_detail_id = id;
    nav->waypoint_detail_open = true;
    nav->waypoint_delete_armed = false;
    nav->screen = MESH_UI_SCREEN_WAYPOINTS;
    nav->cursor[MESH_UI_SCREEN_WAYPOINTS] = 0U;
}

bool mesh_ui_nav_close_waypoint(struct mesh_ui_nav *nav) {
    if (!nav->waypoint_detail_open) {
        return false;
    }
    nav->waypoint_detail_open = false;
    nav->waypoint_detail_id = 0U;
    nav->waypoint_delete_armed = false;
    nav->cursor[MESH_UI_SCREEN_WAYPOINTS] = nav->waypoint_list_cursor;
    return true;
}

void mesh_ui_nav_open_waypoint_keyboard(struct mesh_ui_nav *nav, uint32_t source_node) {
    /* The Compose draft is parked in the one parking slot, exactly as a settings keyboard parks
       it: the text in front of the user is the one worth keeping, and there is only ever one
       keyboard open. */
    snprintf(nav->draft_saved, sizeof nav->draft_saved, "%s", nav->draft);
    nav->draft[0] = '\0';
    nav->keyboard_waypoint = true;
    nav->waypoint_source_node = source_node;
    nav->keyboard_field = MESH_UI_FIELD_NONE;
    nav->keyboard_open = true;
    nav->compose_open = false;
    nav->kb_row = 0U;
    nav->kb_col = 0U;
    /* Upper case first: a place is a proper noun far more often than a message is a sentence,
       and the layer falls back to lower after the first letter the way a phone's does. */
    nav->kb_layer = MESH_UI_KB_UPPER;
    nav->screen = MESH_UI_SCREEN_WAYPOINTS;
}

bool mesh_ui_nav_commit_waypoint(struct mesh_ui_nav *nav, struct mesh_ui_action *action) {
    if (nav->draft[0] == '\0') {
        /* An unnamed place is legal on the wire and useless in a list; refuse rather than
           broadcast one. Leaving the keyboard up is the point - there is nothing to go back
           to, and the user is one letter away from a place. */
        return false;
    }
    if (action != NULL) {
        action->type = MESH_UI_ACTION_SHARE_WAYPOINT;
        action->dest = nav->waypoint_source_node;
        action->number = 0U; /* a new place, not a re-share of one we hold */
        snprintf(action->text, sizeof action->text, "%s", nav->draft);
    }
    nav->draft[0] = '\0';
    mesh_ui_nav_keyboard_close(nav);
    /* Land on the list, where the new place is about to appear. */
    nav->screen = MESH_UI_SCREEN_WAYPOINTS;
    return true;
}

uint32_t mesh_ui_nav_waypoint_row_count(const struct mesh_ui_nav *nav,
                                        const struct mesh_ui_store *store) {
    if (nav == NULL || !nav->waypoint_detail_open) {
        return mesh_ui_waypoint_count(store);
    }
    const struct mesh_ui_waypoint *waypoint =
        mesh_ui_waypoint_find(&store->waypoints, nav->waypoint_detail_id);
    if (waypoint == NULL) {
        return 0U;
    }
    struct mesh_ui_waypoint_item items[MESH_UI_WAYPOINT_ITEMS_MAX];
    return mesh_ui_waypoint_detail_build(
        waypoint, store->handshake_valid ? &store->handshake : NULL, &store->settings, 0U,
        nav->waypoint_delete_armed, items, MESH_UI_WAYPOINT_ITEMS_MAX);
}

/* A on the list: open the place under the cursor, or start a new one on the last row. */
static bool mesh_ui_nav_waypoint_list_confirm(struct mesh_ui_nav *nav,
                                              const struct mesh_ui_store *store, uint32_t cursor) {
    struct mesh_ui_waypoint_row row;
    if (!mesh_ui_waypoint_row(store, cursor, &row)) {
        return false;
    }
    if (row.type == MESH_UI_WAYPOINT_ROW_NEW) {
        int32_t latitude_i = 0;
        int32_t longitude_i = 0;
        if (!mesh_ui_waypoint_our_fix(store->handshake_valid ? &store->handshake : NULL,
                                      &latitude_i, &longitude_i)) {
            /*
             * Refused, and said out loud. Opening a keyboard for a place with nowhere to put
             * it would waste the typing, but a press that does nothing at all is the client
             * telling the user their Brick is broken: the row's own line says why, and the one
             * reader guaranteed not to have read it is the one who just pressed A.
             *
             * The app raises this same string when a name arrives with no fix behind it
             * (mesh_app_on_ui_action), which is the path a node's "New waypoint here" takes;
             * this is the same refusal one step earlier, where the nav can see it coming.
             */
            mesh_ui_nav_set_toast(nav, mesh_time_monotonic_ms(),
                                  mesh_str(MESH_STR_TOAST_WAYPOINT_NO_FIX));
            return true; /* the toast is nav state, so the frame has changed */
        }
        mesh_ui_nav_open_waypoint_keyboard(nav, 0U);
        return true;
    }
    mesh_ui_nav_open_waypoint(nav, row.id);
    return true;
}

/* A inside an open place: one of the two action rows, or nothing. */
static bool mesh_ui_nav_waypoint_detail_confirm(struct mesh_ui_nav *nav,
                                                const struct mesh_ui_store *store, uint32_t cursor,
                                                struct mesh_ui_action *action) {
    const struct mesh_ui_waypoint *waypoint =
        mesh_ui_waypoint_find(&store->waypoints, nav->waypoint_detail_id);
    if (waypoint == NULL) {
        return false;
    }

    struct mesh_ui_waypoint_item items[MESH_UI_WAYPOINT_ITEMS_MAX];
    const uint32_t count = mesh_ui_waypoint_detail_build(
        waypoint, store->handshake_valid ? &store->handshake : NULL, &store->settings, 0U,
        nav->waypoint_delete_armed, items, MESH_UI_WAYPOINT_ITEMS_MAX);
    if (cursor >= count || items[cursor].kind != MESH_UI_WAYPOINT_ITEM_ACTION) {
        return false;
    }

    switch ((enum mesh_ui_waypoint_action)items[cursor].action) {
    case MESH_UI_WAYPOINT_ACTION_SHARE:
        if (action != NULL) {
            action->type = MESH_UI_ACTION_SHARE_WAYPOINT;
            /* The place already has coordinates, so no node's fix is being read: `number`
               naming the waypoint is what tells the app this is a re-share rather than a new
               place at somebody's position. */
            action->number = waypoint->id;
            snprintf(action->text, sizeof action->text, "%s", waypoint->name);
        }
        return false; /* nothing on screen changes; the toast reports what happened */
    case MESH_UI_WAYPOINT_ACTION_DELETE:
        if (!nav->waypoint_delete_armed) {
            nav->waypoint_delete_armed = true; /* the row now says "A again to delete" */
            return true;
        }
        nav->waypoint_delete_armed = false;
        if (action != NULL) {
            action->type = MESH_UI_ACTION_FORGET_WAYPOINT;
            action->number = waypoint->id;
        }
        /* The detail closes on the next clamp, when the place is gone from the list. */
        return false;
    case MESH_UI_WAYPOINT_ACTION_NONE:
    default:
        return false;
    }
}

bool mesh_ui_nav_waypoint_confirm(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                  uint32_t cursor, struct mesh_ui_action *action) {
    if (!nav->waypoint_detail_open) {
        return mesh_ui_nav_waypoint_list_confirm(nav, store, cursor);
    }
    return mesh_ui_nav_waypoint_detail_confirm(nav, store, cursor, action);
}

bool mesh_ui_nav_waypoint_clamp(struct mesh_ui_nav *nav, const struct mesh_ui_store *store) {
    bool moved = false;
    /*
     * A place can leave the list while its detail is open - somebody withdraws it, or the book
     * evicts it - so back out rather than draw a screen about nothing. This is the node
     * detail's rule and it runs before the cursor clamp for the same reason: the restored list
     * position has to be clamped along with everything else.
     */
    if (nav->waypoint_detail_open &&
        mesh_ui_waypoint_find(&store->waypoints, nav->waypoint_detail_id) == NULL) {
        mesh_ui_nav_close_waypoint(nav);
        moved = true;
    }

    /* The parked list position, so backing out lands on a real row even when places went while
       the detail was open. The list always has its "New waypoint here" row, so it is never
       empty and this never has to invent a position. */
    if (nav->waypoint_detail_open) {
        const uint32_t rows = mesh_ui_waypoint_count(store);
        if (nav->waypoint_list_cursor >= rows) {
            nav->waypoint_list_cursor = rows > 0U ? rows - 1U : 0U;
        }
    }
    return moved;
}
