#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/nodes.h"

#include "inkcell/ui/theme.h"

#include "mesh/ui/node_detail.h"
#include "mesh/ui/store_handshake.h"
#include "mesh/ui/store_node.h"

#include "mesh/geo/vector.h"

#include <string.h>
#include <strings.h> /* strcasecmp, which is here rather than in string.h */

/*
 * The table.
 *
 * src/ui/tables/status.c's shape: one row per member of the enum, in the order the chips draw, read
 * by the three files that each have an opinion about the list. What a row holds here is only the
 * word, because everything else about a filter is the predicate below - there is no condition
 * on offering one, which is the difference from the Status verbs. A filter that matched nothing
 * is still offered: "Pinned" with nothing pinned is the answer to "did I pin that node?", and a
 * chip that came and went as the roster changed would be a control the reader cannot learn.
 */
static const inkcell_str_id k_filter_labels[MESH_UI_NODE_FILTER_COUNT] = {
    [MESH_UI_NODE_FILTER_ALL] = MESH_STR_NODES_FILTER_ALL,
    [MESH_UI_NODE_FILTER_DIRECT] = MESH_STR_NODES_FILTER_DIRECT,
    [MESH_UI_NODE_FILTER_PINNED] = MESH_STR_NODES_FILTER_PINNED,
};

/* The published roster's length, clamped the way every other reader of it clamps. */
static uint32_t node_list_count(const struct mesh_ui_handshake_state *handshake) {
    if (handshake == NULL) {
        return 0U;
    }
    return handshake->node_count > MESH_UI_MAX_HANDSHAKE_NODES ? MESH_UI_MAX_HANDSHAKE_NODES
                                                               : handshake->node_count;
}

/* Whether this row is our own radio, the way every other reader of that question asks it. */
static bool node_is_self(const struct mesh_ui_handshake_state *handshake,
                         const struct mesh_ui_node_summary *node) {
    if (handshake == NULL || !handshake->has_my_info || handshake->my_info.node_num == 0U) {
        return false;
    }
    return node->node_id == handshake->my_info.node_num;
}

bool mesh_ui_node_filter_matches(const struct mesh_ui_handshake_state *handshake,
                                 const struct mesh_ui_node_summary *node,
                                 enum mesh_ui_node_filter filter) {
    if (node == NULL) {
        return false;
    }
    switch (filter) {
    case MESH_UI_NODE_FILTER_DIRECT:
        /*
         * What the list draws a staircase on: the NodeDB test the renderer makes *first*, and
         * then its own. Both halves, in that order - see the enum's note for why leaving the
         * first out put nodes labelled "off radio" under a chip that promises earshot.
         */
        return node->in_nodedb && mesh_ui_node_signal_heard(node);
    case MESH_UI_NODE_FILTER_PINNED:
        /* What the list draws a star on, which is never ourselves however the radio's own
           NodeDB entry has us flagged - see the enum's note. */
        return node->is_favorite && !node_is_self(handshake, node);
    case MESH_UI_NODE_FILTER_ALL:
    case MESH_UI_NODE_FILTER_COUNT:
    default:
        /*
         * ALL is a match for everything, and so is anything out of range.
         *
         * The default is deliberate rather than defensive. `nav.node_filter` is a uint8_t
         * restored from a preferences file, so a value this enum has never held can reach here
         * - and the failure a reader can act on is a filter row that has come back on "All",
         * not a node list that is empty for a reason nothing on the frame can say.
         */
        return true;
    }
}

uint32_t mesh_ui_node_filter_count(const struct mesh_ui_handshake_state *handshake,
                                   enum mesh_ui_node_filter filter) {
    const uint32_t count = node_list_count(handshake);
    if (filter == MESH_UI_NODE_FILTER_ALL) {
        return count;
    }
    uint32_t kept = 0U;
    for (uint32_t i = 0; i < count; ++i) {
        if (mesh_ui_node_filter_matches(handshake, &handshake->nodes[i], filter)) {
            ++kept;
        }
    }
    return kept;
}

enum mesh_ui_node_filter mesh_ui_node_filter_step(enum mesh_ui_node_filter filter, int delta) {
    const int count = (int)MESH_UI_NODE_FILTER_COUNT;
    int at = (int)filter;
    if (at < 0 || at >= count) {
        at = (int)MESH_UI_NODE_FILTER_ALL; /* the same out-of-range answer the predicate gives */
    }
    /* Modulo over a signed step, so a delta of -1 walks backwards without going negative. */
    at = ((at + delta) % count + count) % count;
    return (enum mesh_ui_node_filter)at;
}

inkcell_str_id mesh_ui_node_filter_label(enum mesh_ui_node_filter filter) {
    if ((int)filter < 0 || filter >= MESH_UI_NODE_FILTER_COUNT) {
        return k_filter_labels[MESH_UI_NODE_FILTER_ALL];
    }
    return k_filter_labels[filter];
}

/* ---- the sort ------------------------------------------------------------------------------
 *
 * The second table, in the order the row steps through. One word each, because the row that
 * shows it is a label and a value column rather than a strip of pills - see the renderer for why
 * five of these could not be chips.
 */
static const inkcell_str_id k_sort_labels[MESH_UI_NODE_SORT_COUNT] = {
    [MESH_UI_NODE_SORT_DEFAULT] = MESH_STR_NODES_SORT_DEFAULT,
    [MESH_UI_NODE_SORT_HEARD] = MESH_STR_NODES_SORT_HEARD,
    [MESH_UI_NODE_SORT_NAME] = MESH_STR_NODES_SORT_NAME,
    [MESH_UI_NODE_SORT_DISTANCE] = MESH_STR_NODES_SORT_DISTANCE,
    [MESH_UI_NODE_SORT_HOPS] = MESH_STR_NODES_SORT_HOPS,
};

/*
 * The name this sort compares, which is the one the row draws first.
 *
 * Never NULL: a node that has said neither name is placed by its `group` rather than by its
 * letters, and handing the comparison an empty string keeps the two arms of the key builder
 * from needing a null check each.
 */
static const char *node_sort_name(const struct mesh_ui_node_summary *node) {
    if (node->short_name[0] != '\0') {
        return node->short_name;
    }
    return node->long_name;
}

/*
 * One row's sort key, built once per node.
 *
 * `index` is where the node sits in the published roster, which is two facts at once: it is how
 * the key finds its node again, and it is the *default sort itself*. Every arm below falls back
 * to it, so a sort that cannot tell two rows apart leaves them in the order the app ranked them
 * - which is the one order the reader has already seen.
 *
 * `group` is "this sort has something to say about this node", and it is 0 for yes so that the
 * measurable rows come first. It is what keeps a node with no fix out of the middle of a
 * distance list and a node the firmware said nothing about off the top of a hop list.
 */
struct node_key {
    uint8_t index;
    uint8_t group;
    uint8_t hops;
    uint32_t heard;
    double distance_m;
    const char *name;
};

/* Both this and mesh_ui_node_view::order hold a roster position in a byte, which is a saving
   worth 128 bytes a view and worth nothing at all if the roster ever outgrows it. */
INKCELL_STATIC_ASSERT(MESH_UI_MAX_HANDSHAKE_NODES <= 256U,
                      "a roster position no longer fits the byte the view orders by");

/* Whether `key` belongs above `prev` under this sort. The whole of the ordering rule, written
   once so the insertion sort below is only the mechanics. */
static bool node_key_before(const struct node_key *key, const struct node_key *prev,
                            enum mesh_ui_node_sort sort) {
    if (key->group != prev->group) {
        return key->group < prev->group;
    }
    if (key->group != 0U) {
        /* Nothing to compare: the published order, which is what `index` is. */
        return false;
    }
    switch (sort) {
    case MESH_UI_NODE_SORT_HEARD:
        return key->heard > prev->heard;
    case MESH_UI_NODE_SORT_NAME:
        return strcasecmp(key->name, prev->name) < 0;
    case MESH_UI_NODE_SORT_DISTANCE:
        return key->distance_m < prev->distance_m;
    case MESH_UI_NODE_SORT_HOPS:
        return key->hops < prev->hops;
    case MESH_UI_NODE_SORT_DEFAULT:
    case MESH_UI_NODE_SORT_COUNT:
    default:
        /* The published order, and the same out-of-range answer the filter's default gives:
           `nav.node_sort` is a byte and a value this enum has never held can reach here. */
        return false;
    }
}

bool mesh_ui_node_sort_available(const struct mesh_ui_handshake_state *handshake,
                                 enum mesh_ui_node_sort sort) {
    if (sort != MESH_UI_NODE_SORT_DISTANCE) {
        /* Everything else is read off the node itself. A roster where nobody has a name or a
           last_heard is not an unavailable sort, it is a sort with nothing to reorder - which
           the list already shows by being in the order it was. */
        return true;
    }
    return mesh_ui_node_our_fix(handshake, NULL, NULL);
}

void mesh_ui_node_view_build(const struct mesh_ui_handshake_state *handshake,
                             enum mesh_ui_node_filter filter, enum mesh_ui_node_sort sort,
                             struct mesh_ui_node_view *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);

    const uint32_t held = node_list_count(handshake);
    if (held == 0U) {
        return;
    }

    /*
     * Our own fix, once for the whole list rather than once per node. False is not a failure
     * here: it makes every node unmeasurable, every key lands in group 1, and the list comes out
     * in its published order - which is what mesh_ui_node_sort_available() lets the screen say
     * out loud rather than leaving the reader to wonder why a distance sort did nothing.
     */
    int32_t self_lat = 0;
    int32_t self_lon = 0;
    const bool have_fix = (sort == MESH_UI_NODE_SORT_DISTANCE) &&
                          mesh_ui_node_our_fix(handshake, &self_lat, &self_lon);

    struct node_key keys[MESH_UI_MAX_HANDSHAKE_NODES];
    uint32_t count = 0U;
    for (uint32_t i = 0; i < held; ++i) {
        const struct mesh_ui_node_summary *node = &handshake->nodes[i];
        if (!mesh_ui_node_filter_matches(handshake, node, filter)) {
            continue;
        }
        struct node_key *key = &keys[count++];
        key->index = (uint8_t)i;
        key->group = 1U;
        key->hops = 0U;
        key->heard = node->last_heard;
        key->distance_m = 0.0;
        key->name = node_sort_name(node);
        switch (sort) {
        case MESH_UI_NODE_SORT_HEARD:
            if (node->last_heard != 0U) {
                key->group = 0U;
            }
            break;
        case MESH_UI_NODE_SORT_NAME:
            if (key->name[0] != '\0') {
                key->group = 0U;
            }
            break;
        case MESH_UI_NODE_SORT_DISTANCE:
            if (have_fix && node->position.valid) {
                struct mesh_geo_vector vector;
                if (mesh_geo_vector_between(self_lat, self_lon, node->position.latitude_i,
                                            node->position.longitude_i, &vector)) {
                    key->group = 0U;
                    key->distance_m = vector.distance_m;
                }
            }
            break;
        case MESH_UI_NODE_SORT_HOPS:
            if (node->has_hops_away) {
                key->group = 0U;
                key->hops = node->hops_away;
            }
            break;
        case MESH_UI_NODE_SORT_DEFAULT:
        case MESH_UI_NODE_SORT_COUNT:
        default:
            break;
        }
    }

    /*
     * Insertion sort, the shape src/ui/views/waypoints.c uses and for its reasons. The list is 128
     * entries at most and is rebuilt per frame, so the simplest *stable* sort is the right one -
     * and stability is the whole of the tie-break rule: the keys are built in published order,
     * so two rows this sort cannot separate come out in the order the app ranked them rather
     * than swapping under the cursor between frames.
     */
    for (uint32_t i = 1; i < count; ++i) {
        const struct node_key key = keys[i];
        uint32_t j = i;
        while (j > 0U && node_key_before(&key, &keys[j - 1U], sort)) {
            keys[j] = keys[j - 1U];
            --j;
        }
        keys[j] = key;
    }

    for (uint32_t i = 0; i < count; ++i) {
        out->order[i] = keys[i].index;
    }
    out->count = count;
}

const struct mesh_ui_node_summary *
mesh_ui_node_view_at(const struct mesh_ui_handshake_state *handshake,
                     const struct mesh_ui_node_view *view, uint32_t index) {
    if (handshake == NULL || view == NULL || index >= view->count) {
        return NULL;
    }
    const uint8_t at = view->order[index];
    if ((uint32_t)at >= node_list_count(handshake)) {
        return NULL;
    }
    return &handshake->nodes[at];
}

enum mesh_ui_node_sort mesh_ui_node_sort_step(enum mesh_ui_node_sort sort, int delta) {
    const int count = (int)MESH_UI_NODE_SORT_COUNT;
    int at = (int)sort;
    if (at < 0 || at >= count) {
        at =
            (int)MESH_UI_NODE_SORT_DEFAULT; /* the same out-of-range answer the key builder gives */
    }
    at = ((at + delta) % count + count) % count;
    return (enum mesh_ui_node_sort)at;
}

inkcell_str_id mesh_ui_node_sort_label(enum mesh_ui_node_sort sort) {
    if ((int)sort < 0 || sort >= MESH_UI_NODE_SORT_COUNT) {
        return k_sort_labels[MESH_UI_NODE_SORT_DEFAULT];
    }
    return k_sort_labels[sort];
}
