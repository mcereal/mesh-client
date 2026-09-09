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
                    uint8_t precision_bits, uint32_t received, bool stale) {
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
 * What to write beside a node.
 *
 * The short name, because a map is mostly empty space with four characters in it and a long
 * name would be the label rather than the marker. It is the same abbreviation the node list
 * already shows in its disc, so a reader who learned a node by its initials on one screen
 * recognises it on the other. A node that has never introduced itself has neither, and falls
 * back to the long name the session derived from its number.
 */
static const char *map_node_label(const struct mesh_ui_node_summary *node) {
    if (node->short_name[0] != '\0') {
        return node->short_name;
    }
    return node->long_name;
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
        const uint32_t nodes = hs->node_count > MESH_UI_MAX_HANDSHAKE_NODES
                                   ? MESH_UI_MAX_HANDSHAKE_NODES
                                   : hs->node_count;
        out->known += nodes;

        /*
         * Our own radio first, so it is drawn under everything else.
         *
         * Two passes over the roster rather than one pass and a sort, because the roster is
         * already in the order this wants for the nodes and the only thing out of place is
         * ourselves. A sort would be a second ranking of a list something else has already
         * ranked, which is the thing the node detail's own rule warns about.
         */
        for (uint32_t i = 0; i < nodes; ++i) {
            const struct mesh_ui_node_summary *node = &hs->nodes[i];
            if (node->node_id != me || me == 0U || !node->position.valid) {
                continue;
            }
            map_add(out, MESH_UI_MAP_MARKER_SELF, node->node_id, node->position.latitude_i,
                    node->position.longitude_i, map_node_label(node), node->position.precision_bits,
                    node->position.received, false);
            break;
        }

        for (uint32_t i = 0; i < nodes; ++i) {
            const struct mesh_ui_node_summary *node = &hs->nodes[i];
            if (!node->position.valid || (me != 0U && node->node_id == me)) {
                continue;
            }
            /* A node the radio has forgotten is still ours to remember and still had a
               position when we heard it - the roster deliberately outlives the NodeDB. Drawn
               differently rather than dropped, which is what the Nodes tab already does. */
            map_add(out, MESH_UI_MAP_MARKER_NODE, node->node_id, node->position.latitude_i,
                    node->position.longitude_i, map_node_label(node), node->position.precision_bits,
                    node->position.received, !node->in_nodedb);
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
                waypoint->longitude_i, name, 0U, waypoint->heard, false);
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
        const uint32_t nodes = store->handshake.node_count > MESH_UI_MAX_HANDSHAKE_NODES
                                   ? MESH_UI_MAX_HANDSHAKE_NODES
                                   : store->handshake.node_count;
        for (uint32_t i = 0; i < nodes; ++i) {
            const struct mesh_ui_node_position *position = &store->handshake.nodes[i].position;
            if (position->valid &&
                mesh_geo_coords_valid(position->latitude_i, position->longitude_i)) {
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
