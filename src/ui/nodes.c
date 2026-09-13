#include "mesh/ui/nodes.h"

#include "mesh/ui/node_detail.h"
#include "mesh/ui/store.h"

/*
 * The table.
 *
 * src/ui/status.c's shape: one row per member of the enum, in the order the chips draw, read by
 * the three files that each have an opinion about the list. What a row holds here is only the
 * word, because everything else about a filter is the predicate below - there is no condition
 * on offering one, which is the difference from the Status verbs. A filter that matched nothing
 * is still offered: "Pinned" with nothing pinned is the answer to "did I pin that node?", and a
 * chip that came and went as the roster changed would be a control the reader cannot learn.
 */
static const enum mesh_str_id k_filter_labels[MESH_UI_NODE_FILTER_COUNT] = {
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

bool mesh_ui_node_filter_matches(const struct mesh_ui_node_summary *node,
                                 enum mesh_ui_node_filter filter) {
    if (node == NULL) {
        return false;
    }
    switch (filter) {
    case MESH_UI_NODE_FILTER_DIRECT:
        /* The list's own test, asked rather than restated - see the enum's note. */
        return mesh_ui_node_signal_heard(node);
    case MESH_UI_NODE_FILTER_PINNED:
        return node->is_favorite;
    case MESH_UI_NODE_FILTER_ALL:
    case MESH_UI_NODE_FILTER_COUNT:
    default:
        /*
         * ALL is a match for everything, and so is anything out of range.
         *
         * The default is deliberate rather than defensive. `nav.node_filter` is a uint8_t
         * restored from a preferences file, so a value this enum has never held can reach here
         * - and the failure a reader can act on is a chip strip that has come back on "All",
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
        if (mesh_ui_node_filter_matches(&handshake->nodes[i], filter)) {
            ++kept;
        }
    }
    return kept;
}

const struct mesh_ui_node_summary *mesh_ui_node_filter_at(
    const struct mesh_ui_handshake_state *handshake, enum mesh_ui_node_filter filter,
    uint32_t index) {
    if (filter == MESH_UI_NODE_FILTER_ALL) {
        /* The unfiltered list is the one every other reader already has an answer for. */
        return mesh_ui_node_detail_at(handshake, index);
    }
    const uint32_t count = node_list_count(handshake);
    uint32_t seen = 0U;
    for (uint32_t i = 0; i < count; ++i) {
        if (!mesh_ui_node_filter_matches(&handshake->nodes[i], filter)) {
            continue;
        }
        if (seen == index) {
            return &handshake->nodes[i];
        }
        ++seen;
    }
    return NULL;
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

enum mesh_str_id mesh_ui_node_filter_label(enum mesh_ui_node_filter filter) {
    if ((int)filter < 0 || filter >= MESH_UI_NODE_FILTER_COUNT) {
        return k_filter_labels[MESH_UI_NODE_FILTER_ALL];
    }
    return k_filter_labels[filter];
}
