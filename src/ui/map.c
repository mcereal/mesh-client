#define _POSIX_C_SOURCE 200809L

/*
 * The markers: which of the things the client knows about have somewhere to be drawn, and what
 * is written beside each one.
 *
 * Nothing here computes a pixel. A marker carries a coordinate and its projection onto the unit
 * square, and the viewport turns that into a position when a screen asks - so this file is the
 * same shape as src/ui/waypoints.c, and the map's backend is the same shape as every other
 * screen renderer: a description of content.
 */

#include "mesh/ui/map.h"

#include "mesh/geo/coords.h"
#include "mesh/i18n/strings.h"
#include "mesh/utils/text.h"

#include <string.h>

/* Adds one marker, if there is room and if it has a place to be. Every field a marker carries
   is filled here, which is what keeps "what goes on the map" one function rather than three. */
static void map_add(struct mesh_ui_map_view *view, enum mesh_ui_map_marker_kind kind, uint32_t id,
                    int32_t latitude_i, int32_t longitude_i, const char *label,
                    uint8_t precision_bits, uint32_t received, bool stale, bool openable) {
    if (view->count >= MESH_UI_MAP_MARKERS_MAX) {
        return;
    }
    struct mesh_geo_point point;
    if (!mesh_geo_mercator_forward(latitude_i, longitude_i, &point)) {
        return;
    }

    struct mesh_ui_map_marker *marker = &view->markers[view->count];
    memset(marker, 0, sizeof *marker);
    marker->kind = (uint8_t)kind;
    marker->id = id;
    marker->latitude_i = latitude_i;
    marker->longitude_i = longitude_i;
    marker->point = point;
    marker->precision_bits = precision_bits;
    marker->received = received;
    marker->stale = stale;
    marker->openable = openable;
    if (label != NULL) {
        mesh_str_copy(marker->label, sizeof marker->label, label);
    }
    if (kind == MESH_UI_MAP_MARKER_SELF) {
        view->has_self = true;
        view->self_index = view->count;
    }
    ++view->count;
}

/*
 * The nodes the map draws, from whichever roster this handshake carries.
 *
 * `map_nodes` is the answer: every positioned node the *session* holds, which is up to twice
 * what the ranking publishes as rows. A handshake nobody published has none - a roster loaded
 * from the cache before the first publish, a hand-built fixture, a capture harness - and for
 * those the published rows are the best there is, so they stand in.
 *
 * The fallback is provably dead after a real publish and is not a second opinion: publish scans
 * every node the session holds, which is a superset of the rows it then copies, so a published
 * row with a position always has a map entry beside it. It exists so that no producer of a
 * handshake has to remember to fill a second array - the failure that would cause is a map that
 * is silently empty, which no build and no screenshot would catch.
 */
static uint32_t map_roster_count(const struct mesh_ui_handshake_state *hs) {
    if (hs->map_node_count > 0U) {
        return hs->map_node_count > MESH_UI_MAX_MAP_NODES ? MESH_UI_MAX_MAP_NODES
                                                          : hs->map_node_count;
    }
    return hs->node_count > MESH_UI_MAX_HANDSHAKE_NODES ? MESH_UI_MAX_HANDSHAKE_NODES
                                                        : hs->node_count;
}

/* One entry of the above, or false when that index is a node with nowhere to be drawn. Both
   sources are range-checked here so neither caller has to: the map is the only screen where a
   coordinate becomes a pixel, and a bad one is an off-panel marker rather than a wrong word. */
static bool map_roster_at(const struct mesh_ui_handshake_state *hs, uint32_t index,
                          struct mesh_ui_map_node *out) {
    memset(out, 0, sizeof *out);
    if (hs->map_node_count > 0U) {
        const struct mesh_ui_map_node *node = &hs->map_nodes[index];
        if (!mesh_geo_coords_valid(node->latitude_i, node->longitude_i)) {
            return false;
        }
        *out = *node;
        return true;
    }

    const struct mesh_ui_node_summary *node = &hs->nodes[index];
    if (!node->position.valid ||
        !mesh_geo_coords_valid(node->position.latitude_i, node->position.longitude_i)) {
        return false;
    }
    out->node_id = node->node_id;
    out->latitude_i = node->position.latitude_i;
    out->longitude_i = node->position.longitude_i;
    out->received = node->position.received;
    out->precision_bits = node->position.precision_bits;
    out->in_nodedb = node->in_nodedb;
    /* The fallback source *is* the published rows, so every entry it yields has one. */
    out->has_row = true;
    /* The short name, falling back to the long one - the same rule publish applies, and it is
       stated once on struct mesh_ui_map_node's `label`. */
    mesh_str_copy(out->label, sizeof out->label,
                  node->short_name[0] != '\0' ? node->short_name : node->long_name);
    return true;
}

void mesh_ui_map_build(const struct mesh_ui_store *store, struct mesh_ui_map_view *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (store == NULL) {
        return;
    }

    const struct mesh_ui_handshake_state *hs = store->handshake_valid ? &store->handshake : NULL;
    const uint32_t me = (hs != NULL && hs->has_my_info) ? hs->my_info.node_num : 0U;

    if (hs != NULL) {
        const uint32_t nodes = map_roster_count(hs);
        /*
         * What the client knows about nodes, which is the *session* roster's own total and not
         * the rows it published - the map draws from the whole of it now, so counting the rows
         * would report a smaller denominator than the map is actually working from. A handshake
         * that never carried a total falls back to its row count, which is the same max the
         * Nodes tab takes against the radio's own figure.
         */
        out->known += hs->nodes_known > nodes ? hs->nodes_known : nodes;

        /*
         * Our own radio first, so it is drawn under everything else.
         *
         * Two passes over the roster rather than one pass and a sort, because the roster is
         * already in the order this wants for the nodes and the only thing out of place is
         * ourselves. A sort would be a second ranking of a list something else has already
         * ranked, which is the thing the node detail's own rule warns about.
         */
        struct mesh_ui_map_node node;
        for (uint32_t i = 0; me != 0U && i < nodes; ++i) {
            if (!map_roster_at(hs, i, &node) || node.node_id != me) {
                continue;
            }
            map_add(out, MESH_UI_MAP_MARKER_SELF, node.node_id, node.latitude_i, node.longitude_i,
                    node.label, node.precision_bits, node.received, false, node.has_row);
            break;
        }

        for (uint32_t i = 0; i < nodes; ++i) {
            if (!map_roster_at(hs, i, &node) || (me != 0U && node.node_id == me)) {
                continue;
            }
            /* A node the radio has forgotten is still ours to remember and still had a
               position when we heard it - the roster deliberately outlives the NodeDB. Drawn
               differently rather than dropped, which is what the Nodes tab already does. */
            map_add(out, MESH_UI_MAP_MARKER_NODE, node.node_id, node.latitude_i, node.longitude_i,
                    node.label, node.precision_bits, node.received, !node.in_nodedb, node.has_row);
        }
    }

    const uint32_t places = store->waypoints.count > MESH_UI_MAX_WAYPOINTS ? MESH_UI_MAX_WAYPOINTS
                                                                           : store->waypoints.count;
    out->known += places;
    for (uint32_t i = 0; i < places; ++i) {
        const struct mesh_ui_waypoint *waypoint = &store->waypoints.entries[i];
        if (!waypoint->has_coords) {
            continue;
        }
        /* A place whose sharer left the name empty is legal on the wire and is what the phone
           apps send for a pin dropped in a hurry; the list already has a word for it. */
        const char *name =
            waypoint->name[0] != '\0' ? waypoint->name : mesh_str(MESH_STR_WAYPOINTS_UNNAMED);
        /*
         * `heard` rather than a precision: a waypoint is a point somebody chose rather than one
         * a receiver solved, so there is no rounding to report and nothing to be vague about.
         * That is the difference between a place and a fix, and it is why the two are two kinds
         * of marker rather than one with a flag.
         */
        map_add(out, MESH_UI_MAP_MARKER_WAYPOINT, waypoint->id, waypoint->latitude_i,
                waypoint->longitude_i, name, 0U, waypoint->heard, false, true);
    }
}

bool mesh_ui_map_selected(const struct mesh_ui_map_view *view,
                          const struct mesh_map_viewport *viewport, uint32_t *out_index) {
    if (view == NULL || viewport == NULL || view->count == 0U) {
        return false;
    }

    const double limit =
        (double)MESH_UI_MAP_SELECT_RADIUS_PX * (double)MESH_UI_MAP_SELECT_RADIUS_PX;
    bool found = false;
    uint32_t best_index = 0U;
    double best_distance = 0.0;

    for (uint32_t i = 0; i < view->count; ++i) {
        /*
         * Measured where the marker is *drawn* - in pixels from the middle of the view - rather
         * than across the ground between two coordinates. See the header: it is what makes the
         * ring and the press one decision, and it is the only reading that gets the poles right.
         */
        double dx = 0.0;
        double dy = 0.0;
        if (!mesh_map_viewport_offset(viewport, view->markers[i].latitude_i,
                                      view->markers[i].longitude_i, &dx, &dy)) {
            continue;
        }
        const double distance = dx * dx + dy * dy;
        if (distance > limit) {
            continue;
        }
        /*
         * Strictly nearer, so a tie goes to the marker built first - and the build order is
         * therefore what settles two markers at the same place. That puts our own radio ahead of
         * a node sharing its position and a node ahead of a waypoint dropped on top of it, which
         * is the order a reader means: the thing that moves is more interesting than the pin
         * somebody left on it.
         */
        if (!found || distance < best_distance) {
            found = true;
            best_distance = distance;
            best_index = i;
        }
    }

    if (found && out_index != NULL) {
        *out_index = best_index;
    }
    return found;
}

/*
 * How far ahead a marker has to be to count as ahead, in pixels.
 *
 * One pixel, which is the smallest difference the picture has. Below it the marker and the
 * crosshair are the same point on the panel, and a press that "moved" onto it would report a
 * change nothing could draw. What it catches now that the selected marker is skipped by
 * identity is the coincident pair - two markers at one coordinate are one place, and which of
 * them is selected there is the selection's own tie-break rather than anything a press can
 * walk between.
 */
#define MAP_STEP_MIN_PX 1.0

bool mesh_ui_map_step(const struct mesh_ui_map_view *view, const struct mesh_map_viewport *viewport,
                      enum mesh_ui_map_direction direction, uint32_t *out_index) {
    if (view == NULL || viewport == NULL || view->count == 0U) {
        return false;
    }

    /*
     * How far ahead a press will look, and how far to the side it will look while doing it: one
     * step of the pan it stands in for, per axis. A press therefore moves the view by about the
     * same amount whether it lands on a marker or pans, which is what leaves the ground between
     * two markers reachable - see the header for what a reach of the whole panel did instead.
     */
    const bool vertical = direction == MESH_UI_MAP_NORTH || direction == MESH_UI_MAP_SOUTH;
    const double reach_along =
        vertical ? (double)MESH_UI_MAP_PAN_STEP_Y : (double)MESH_UI_MAP_PAN_STEP_X;
    const double reach_cross =
        vertical ? (double)MESH_UI_MAP_PAN_STEP_X : (double)MESH_UI_MAP_PAN_STEP_Y;

    /*
     * What the reader is already aimed at, asked of the one function that answers it.
     *
     * "Under the crosshair" is a disc of MESH_UI_MAP_SELECT_RADIUS_PX, not a point, so a marker
     * can be selected while sitting some pixels ahead of centre - which a fall-back pan stopping
     * just short of one leaves behind routinely. Read as "ahead of the press", such a marker is
     * the nearest candidate and the press spends itself nudging the view onto something already
     * selected: the line under the map does not change, and a press whose whole effect is a
     * ten-pixel shift reads as a press that did nothing.
     *
     * So the selected marker is skipped, and it is skipped by *identity* rather than by
     * distance. Asking mesh_ui_map_selected() rather than re-deriving "near enough" keeps the
     * ring a backend draws, the marker A opens and the marker a direction declines to revisit as
     * one answer - the same rule the app bar's back arrow follows. Only that one marker is
     * skipped: another inside the same disc is still a destination, which is what lets a press
     * step between two markers drawn on top of each other.
     */
    uint32_t selected = 0U;
    const bool has_selected = mesh_ui_map_selected(view, viewport, &selected);

    bool found = false;
    uint32_t best_index = 0U;
    double best_distance = 0.0;

    for (uint32_t i = 0; i < view->count; ++i) {
        if (has_selected && i == selected) {
            continue;
        }
        /* The same offset the selection and the placement are derived from - so what a press
           calls "west of the crosshair" is what the reader saw drawn west of it. */
        double dx = 0.0;
        double dy = 0.0;
        if (!mesh_map_viewport_offset(viewport, view->markers[i].latitude_i,
                                      view->markers[i].longitude_i, &dx, &dy)) {
            continue;
        }

        /* Screen y grows south, so north is the negative one; resolving that here is what keeps
           it out of the comparisons below. */
        double along = 0.0;
        double cross = 0.0;
        switch (direction) {
        case MESH_UI_MAP_NORTH:
            along = -dy;
            cross = dx;
            break;
        case MESH_UI_MAP_SOUTH:
            along = dy;
            cross = dx;
            break;
        case MESH_UI_MAP_WEST:
            along = -dx;
            cross = dy;
            break;
        case MESH_UI_MAP_EAST:
        default:
            along = dx;
            cross = dy;
            break;
        }

        if (along < MAP_STEP_MIN_PX || along > reach_along) {
            continue;
        }
        const double span = cross < 0.0 ? -cross : cross;
        if (span > reach_cross) {
            /* Further to the side than one press moves the world sideways. Answering "north"
               with a marker mostly to the east is a press that did not do what it said, and the
               pan that runs instead walks the reader towards it in the direction they asked
               for. */
            continue;
        }
        if (span > along) {
            /* Outside the quadrant: nearer to another direction than to this one, and it is
               that direction's press to answer. The four of them leave nothing uncovered. */
            continue;
        }

        const double distance = along * along + cross * cross;
        if (!found || distance < best_distance) {
            found = true;
            best_distance = distance;
            best_index = i;
        }
    }

    if (found && out_index != NULL) {
        *out_index = best_index;
    }
    return found;
}

uint32_t mesh_ui_map_points(const struct mesh_ui_map_view *view, struct mesh_geo_point *out,
                            uint32_t capacity) {
    if (view == NULL || out == NULL) {
        return 0U;
    }
    const uint32_t count = view->count < capacity ? view->count : capacity;
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = view->markers[i].point;
    }
    return count;
}

uint32_t mesh_ui_map_visible(const struct mesh_ui_map_view *view,
                             const struct mesh_map_viewport *viewport) {
    if (view == NULL || viewport == NULL) {
        return 0U;
    }
    uint32_t count = 0U;
    for (uint32_t i = 0; i < view->count; ++i) {
        struct mesh_map_placement placement;
        if (mesh_map_viewport_place(viewport, view->markers[i].latitude_i,
                                    view->markers[i].longitude_i, &placement) &&
            placement.visible) {
            ++count;
        }
    }
    return count;
}

bool mesh_ui_map_find(const struct mesh_ui_map_view *view, uint8_t kind, uint32_t id,
                      uint32_t *out_index) {
    if (view == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < view->count; ++i) {
        /*
         * Both halves of the key. A node number and a waypoint id are both uint32_t and are
         * drawn from different spaces entirely, so matching on the number alone would let a
         * selection follow a waypoint whose id happened to equal a node's - which is not a
         * remote coincidence: a waypoint id is a small counter and a node number is arbitrary.
         */
        if (view->markers[i].kind == kind && view->markers[i].id == id) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return true;
        }
    }
    return false;
}

bool mesh_ui_map_has_markers(const struct mesh_ui_store *store) {
    if (store == NULL) {
        return false;
    }
    if (store->handshake_valid) {
        /* The same roster mesh_ui_map_build() walks, asked the same way - the row that offers
           the map and the map itself disagreeing about whether there is anything on it is
           exactly the bug two readings of one question produce. */
        const uint32_t nodes = map_roster_count(&store->handshake);
        struct mesh_ui_map_node node;
        for (uint32_t i = 0; i < nodes; ++i) {
            if (map_roster_at(&store->handshake, i, &node)) {
                return true;
            }
        }
    }
    const uint32_t places = store->waypoints.count > MESH_UI_MAX_WAYPOINTS ? MESH_UI_MAX_WAYPOINTS
                                                                           : store->waypoints.count;
    for (uint32_t i = 0; i < places; ++i) {
        const struct mesh_ui_waypoint *waypoint = &store->waypoints.entries[i];
        if (waypoint->has_coords &&
            mesh_geo_coords_valid(waypoint->latitude_i, waypoint->longitude_i)) {
            return true;
        }
    }
    return false;
}
