#ifndef MESH_UI_REACH_H
#define MESH_UI_REACH_H

#include "mesh/ui/store_handshake.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How far away the roster is: the Status board's Reach card, as numbers.
 *
 * The Mesh card counts what the radio *heard* - packets, airtime, the database's size - and none
 * of it says what shape the mesh is from here. Two meshes of forty nodes can be forty nodes in
 * earshot or three neighbours relaying for thirty-seven, and they fail differently: the first
 * stops working when the channel fills, the second when one of the three goes quiet. The hop
 * counts the radio already keeps per node are that shape, and this is them summed.
 *
 * Counted off the published roster rather than anything the radio reports as a total, so every
 * figure on the card is one a reader could check by scrolling the Nodes tab. Our own node is left
 * out - it is zero hops from itself, and counting it would put one "direct" neighbour on every
 * card that has none.
 *
 * Four buckets and not eight, because past two hops a mesh is "far" and the difference between
 * four and five is not one anybody acts on; and because the bar the buckets become is a
 * composition, which reads at four parts and turns to stripes at eight.
 */
enum {
    MESH_UI_REACH_DIRECT = 0, /* heard straight off the air */
    MESH_UI_REACH_ONE,
    MESH_UI_REACH_TWO,
    MESH_UI_REACH_MORE, /* three hops or further */
    MESH_UI_REACH_BUCKETS
};

struct mesh_ui_reach {
    /* Nodes with a hop count, by bucket. A node with none - heard only before the radio said, or
       only over the broker - is in none of them, so the four are disjoint and the bar is honest. */
    uint32_t hops[MESH_UI_REACH_BUCKETS];
    uint32_t counted; /* the four summed */
    /* Nodes the roster knows only through the broker. Not a bucket: a broker is not a distance. */
    uint32_t via_broker;
    /*
     * The best and the worst link among the direct neighbours, by SNR - roster indices, or -1 for
     * none. Direct only, because an SNR is measured on the last hop and a three-hop node's figure
     * is the relay's link rather than its own; and not over the broker, which has no SNR at all.
     */
    int strongest;
    int weakest;
};

/* Sums the roster in `hs` into `out`. Never fails: an empty roster is a reach of nothing. */
void mesh_ui_reach_of(const struct mesh_ui_handshake_state *hs, struct mesh_ui_reach *out);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_REACH_H */
