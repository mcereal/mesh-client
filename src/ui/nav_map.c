#define _POSIX_C_SOURCE 200809L

/*
 * The map's own key handling: opening it, moving it, and what A does to whatever is under the
 * crosshair.
 *
 * It is the one screen in this client where the d-pad is not a cursor. Every other list moves a
 * selection between rows; here the four directions move the *world*, and the selection follows
 * from where the world ended up. That is why this file exists at all rather than a branch in
 * nav.c: the presses have to be taken before the routing that turns Left and Right into tabs,
 * and a screen that quietly redefines two of the four directions deserves to say so somewhere a
 * reader will find it.
 *
 * What moving the world does not mean is moving it by a fixed amount. A press goes about a step
 * that way and stops exactly on a marker when there is one within the step, falling back to a
 * plain pan when there is not - because a view that only ever landed on multiples of its own
 * step could not be aimed at most of what it was drawing, and a press that went to the next
 * marker however far away it was could not be used to look around at all. See
 * mesh_ui_map_step() for the arithmetic, and MESH_UI_MAP_PAN_STEP_X for the length both halves
 * share - one number in map.h rather than a pan of this file's own, because a reach longer than
 * the pan is a map that hands the reader the next node on every tap and never the ground
 * between two of them, and a pan longer than the reach is a map that steps over markers.
 *
 * "A pan is many enough to stop on something" is what this file used to claim, and it was
 * false: a pan of a fixed number of pixels only ever visits a lattice - 176 across and 84 down
 * from wherever the view opened - while the crosshair captures a disc of
 * MESH_UI_MAP_SELECT_RADIUS_PX, which is 28. A sixth of the plane is inside that lattice's
 * discs, so five markers in six could not be put under the crosshair at all at a given zoom.
 * What stops on something is mesh_ui_map_step(); the pan below is what runs when it has nothing
 * within reach to stop on.
 *
 * The shoulders keep the tabs. L1/R1 and Left/Right are the same press everywhere else in the
 * client, and this is the one screen that splits them - so the pair that still walks the tab
 * strip is the pair the action bar has always named, and the strip above the body never stops
 * working while the map is open.
 */

#include "nav_internal.h"

#include "mesh/ui/map.h"

#include <string.h>

/* Builds the marker set for the store as it is now. The map has no cached view: the roster
   re-ranks and fixes arrive, and a set built once at open would be the map disagreeing with the
   list it was opened from. */
static void map_view(const struct mesh_ui_store *store, struct mesh_ui_map_view *out) {
    mesh_ui_map_build(store, out);
}

/*
 * Frames everything the map holds.
 *
 * Both the state the map opens in and what START goes back to, which is one idea and therefore
 * one function: "show me all of it". A map that opened on a fit and then offered no way back to
 * one would make the first pan a decision the reader could not undo.
 */
static bool mesh_ui_nav_map_fit(struct mesh_ui_nav *nav, const struct mesh_ui_store *store) {
    struct mesh_ui_map_view view;
    map_view(store, &view);
    if (view.count == 0U) {
        return false;
    }
    struct mesh_geo_point points[MESH_UI_MAP_MARKERS_MAX];
    const uint32_t count = mesh_ui_map_points(&view, points, MESH_UI_MAP_MARKERS_MAX);
    return mesh_map_viewport_fit(&nav->map_viewport, points, (size_t)count, MESH_UI_MAP_FIT_MARGIN);
}

void mesh_ui_nav_open_map(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                          uint32_t focus_node) {
    if (nav == NULL) {
        return;
    }
    if (!nav->map_open) {
        /* The list's position, parked exactly as opening a node detail parks it - the map is a
           level over the same list, so backing out has to land where the reader left. */
        nav->node_list_cursor = nav->cursor[MESH_UI_SCREEN_NODES];
    }
    nav->map_open = true;
    nav->screen = MESH_UI_SCREEN_NODES;

    mesh_map_viewport_init(&nav->map_viewport, 0, 0, MESH_MAP_ZOOM_DEFAULT);
    /* The declared box rather than a measured one; see MESH_UI_MAP_FIT_WIDTH for why the nav
       cannot have a measured one and why a small declared box is the safe direction. */
    mesh_map_viewport_resize(&nav->map_viewport, MESH_UI_MAP_FIT_WIDTH, MESH_UI_MAP_FIT_HEIGHT);

    /*
     * Aimed, or framed. A press that named a node is a press about that node, so the map opens
     * looking at it and close enough to see where it is; a press that named nothing is "show me
     * the mesh", and that is a fit.
     */
    struct mesh_ui_map_view view;
    map_view(store, &view);
    uint32_t index = 0U;
    if (focus_node != 0U &&
        (mesh_ui_map_find(&view, MESH_UI_MAP_MARKER_SELF, focus_node, &index) ||
         mesh_ui_map_find(&view, MESH_UI_MAP_MARKER_NODE, focus_node, &index))) {
        (void)mesh_map_viewport_center_on(&nav->map_viewport, view.markers[index].latitude_i,
                                          view.markers[index].longitude_i);
        nav->map_viewport.zoom = MESH_UI_MAP_ZOOM_FOCUS;
        return;
    }
    (void)mesh_ui_nav_map_fit(nav, store);
}

bool mesh_ui_nav_close_map(struct mesh_ui_nav *nav) {
    if (nav == NULL || !nav->map_open) {
        return false;
    }
    nav->map_open = false;
    memset(&nav->map_viewport, 0, sizeof nav->map_viewport);
    nav->cursor[MESH_UI_SCREEN_NODES] = nav->node_list_cursor;
    return true;
}

bool mesh_ui_nav_map_clamp(struct mesh_ui_nav *nav, const struct mesh_ui_store *store) {
    if (nav == NULL || store == NULL || !nav->map_open) {
        return false;
    }
    /*
     * A map with nothing left to draw closes, the way a node detail whose node has left the
     * roster does. The two cases are the same case - a screen about something that is no longer
     * there - and they arrive the same way: a forget, a radio swap, a cache that emptied.
     *
     * The grid would still draw, which is exactly the trap: a map of empty graticule looks like
     * a working map that has been panned into the ocean, and a reader would sooner believe they
     * had lost their place than that the roster had gone.
     */
    if (!mesh_ui_map_has_markers(store)) {
        mesh_ui_nav_close_map(nav);
        return true;
    }
    return false;
}

/*
 * A direction on the map: onto the next marker that way, or a pan when there is none.
 *
 * Both halves are the same sentence - "look west" - and the difference between them is only
 * whether there is anything west to look at. Landing on the marker's own coordinates is what
 * makes the aim exact: the selection is still derived from the centre of the view by
 * mesh_ui_map_selected(), so the ring a backend draws and the marker A opens remain one answer,
 * and the nav still holds no selection of its own. See mesh_ui_map_step() for why a pan alone
 * could not aim at all.
 *
 * The fall-through is a real path rather than a safety net: it is what pans across open grid,
 * and it is what walks a distant marker into reach so the press after it can land.
 */
static bool mesh_ui_nav_map_move(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                 enum mesh_ui_map_direction direction) {
    struct mesh_ui_map_view view;
    map_view(store, &view);

    uint32_t index = 0U;
    if (mesh_ui_map_step(&view, &nav->map_viewport, direction, &index) &&
        mesh_map_viewport_center_on(&nav->map_viewport, view.markers[index].latitude_i,
                                    view.markers[index].longitude_i)) {
        return true;
    }

    switch (direction) {
    case MESH_UI_MAP_NORTH:
        return mesh_map_viewport_pan(&nav->map_viewport, 0, -MESH_UI_MAP_PAN_STEP_Y);
    case MESH_UI_MAP_SOUTH:
        return mesh_map_viewport_pan(&nav->map_viewport, 0, MESH_UI_MAP_PAN_STEP_Y);
    case MESH_UI_MAP_WEST:
        return mesh_map_viewport_pan(&nav->map_viewport, -MESH_UI_MAP_PAN_STEP_X, 0);
    case MESH_UI_MAP_EAST:
    default:
        return mesh_map_viewport_pan(&nav->map_viewport, MESH_UI_MAP_PAN_STEP_X, 0);
    }
}

/*
 * A on the map: open whatever the crosshair is on.
 *
 * The two kinds of marker go to two different places, and the second one changes tab. That is
 * the node detail's "Message this node" precedent rather than a new idea - a press that names a
 * conversation lands on the conversation wherever it lives - and it is the honest destination:
 * a waypoint's detail is a Waypoints screen, and building a second one under the Nodes tab would
 * be two screens about one place.
 */
static bool mesh_ui_nav_map_confirm(struct mesh_ui_nav *nav, const struct mesh_ui_store *store) {
    struct mesh_ui_map_view view;
    map_view(store, &view);

    uint32_t index = 0U;
    if (!mesh_ui_map_selected(&view, &nav->map_viewport, &index)) {
        /*
         * Nothing under the crosshair, and nothing happens - deliberately without a toast. A
         * reader panning across empty grid is not making a mistake and does not need to be told
         * so on every press; the ring under the crosshair is already the whole of the feedback,
         * by being absent.
         */
        return false;
    }

    const struct mesh_ui_map_marker *marker = &view.markers[index];
    if (!marker->openable) {
        /*
         * A node the map can draw and the client cannot open: the map takes every positioned
         * node the session holds and the list publishes the ranked 128, so a node far enough
         * down that ranking has a marker and no row. Nothing happens, for the same reason
         * nothing happens on empty grid - and the same silence, because the reader is not
         * making a mistake. The line under the map is still naming the node and its range,
         * which is most of what a detail would have said about a node this far away.
         *
         * Refused here rather than left to mesh_ui_nav_clamp(). The clamp does close such a
         * detail - it runs on every snapshot, so the frame after the press is drawn without it
         * - but the press has already reset the list cursor on its way past and left
         * `node_detail_node` naming a node nothing is showing. A press that does nothing and
         * moves something is worse than one that does nothing. See struct mesh_ui_map_node's
         * `has_row` for the seam that would let this press work instead.
         */
        return false;
    }
    if (marker->kind == MESH_UI_MAP_MARKER_WAYPOINT) {
        mesh_ui_nav_open_waypoint(nav, marker->id);
        return true;
    }
    /*
     * A node's detail, opened *over* the map rather than instead of it: `map_open` stays set, so
     * B lands back on the map with the view exactly where it was left. Opening a node from a map
     * and being returned to a list is the move that loses a reader's place.
     */
    nav->node_detail_node = marker->id;
    nav->node_detail_open = true;
    nav->node_remove_armed = false;
    nav->cursor[MESH_UI_SCREEN_NODES] = 0U;
    return true;
}

bool mesh_ui_nav_map_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                         enum mesh_ui_key key, bool *handled) {
    if (handled != NULL) {
        *handled = false;
    }
    if (nav == NULL || store == NULL || !nav->map_open) {
        return false;
    }
    /*
     * The map has to be the thing on the panel, not merely a thing that is open.
     *
     * `map_open` deliberately outlives a change of tab - every tab in this client keeps its own
     * place, and coming back to Nodes should show the view that was left - so it says where the
     * Nodes tab is standing and *not* what the reader is looking at. Two presses make the
     * difference visible: a shoulder walks off this tab with the map still open behind it, and A
     * on a waypoint marker jumps to the Waypoints tab outright. Without the screen test below,
     * the arrows would pan a map nobody can see and the first B on that place would close it
     * instead of the place.
     *
     * A node's detail is the same question one level in: it is drawn over the map and owns its
     * own presses, and the map underneath is not being looked at either.
     */
    if (nav->screen != MESH_UI_SCREEN_NODES || nav->node_detail_open) {
        return false;
    }

    switch (key) {
    /*
     * The four directions, taken here rather than falling through to the routing that turns
     * Left and Right into a change of tab. This is the only screen in the client that takes
     * them, and the shoulders are left alone so the tab strip still works from here.
     *
     * North-up, so the d-pad's names become the ground's: Left is west. Which is also what the
     * move is expressed in, because the marker it is looking for is on the ground and only the
     * pan underneath it is in pixels.
     */
    case MESH_UI_KEY_UP:
        if (handled != NULL) {
            *handled = true;
        }
        return mesh_ui_nav_map_move(nav, store, MESH_UI_MAP_NORTH);
    case MESH_UI_KEY_DOWN:
        if (handled != NULL) {
            *handled = true;
        }
        return mesh_ui_nav_map_move(nav, store, MESH_UI_MAP_SOUTH);
    case MESH_UI_KEY_LEFT:
        if (handled != NULL) {
            *handled = true;
        }
        return mesh_ui_nav_map_move(nav, store, MESH_UI_MAP_WEST);
    case MESH_UI_KEY_RIGHT:
        if (handled != NULL) {
            *handled = true;
        }
        return mesh_ui_nav_map_move(nav, store, MESH_UI_MAP_EAST);
    /*
     * X in, Y out. The pair the roadmap named, and the pair that is free here: A is the press
     * that opens, B is the way back, and the two remaining face buttons are the two remaining
     * things a map does.
     */
    case MESH_UI_KEY_X:
        if (handled != NULL) {
            *handled = true;
        }
        return mesh_map_viewport_zoom_by(&nav->map_viewport, +1);
    case MESH_UI_KEY_Y:
        if (handled != NULL) {
            *handled = true;
        }
        return mesh_map_viewport_zoom_by(&nav->map_viewport, -1);
    case MESH_UI_KEY_START:
        /*
         * Back to everything.
         *
         * START rather than SELECT, which the roadmap suggested before there was a help screen:
         * SELECT explains where you are standing on every screen in this client that has
         * anything to explain, and a key that meant something else on one screen would be the
         * one exception a reader has no way to know about. START is already not an alias for A
         * on the keyboard, so it is a key this client has precedent for spending.
         */
        if (handled != NULL) {
            *handled = true;
        }
        return mesh_ui_nav_map_fit(nav, store);
    case MESH_UI_KEY_A:
        if (handled != NULL) {
            *handled = true;
        }
        return mesh_ui_nav_map_confirm(nav, store);
    case MESH_UI_KEY_B:
        if (handled != NULL) {
            *handled = true;
        }
        return mesh_ui_nav_close_map(nav);
    /* Everything else - the shoulders, SELECT, the quit key - is not the map's, and falls
       through to the routing that has always answered for it. */
    case MESH_UI_KEY_L1:
    case MESH_UI_KEY_R1:
    case MESH_UI_KEY_SELECT:
    case MESH_UI_KEY_NONE:
    default:
        return false;
    }
}
