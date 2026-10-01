#define _POSIX_C_SOURCE 200809L

/*
 * The Status board's Reach card, as numbers - see include/mesh/ui/reach.h.
 */

#include "mesh/ui/reach.h"

#include <string.h>

void mesh_ui_reach_of(const struct mesh_ui_handshake_state *hs, struct mesh_ui_reach *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->strongest = -1;
    out->weakest = -1;
    if (hs == NULL) {
        return;
    }
    const uint32_t self = hs->has_my_info ? hs->my_info.node_num : 0U;
    const uint32_t count =
        hs->node_count < MESH_UI_MAX_HANDSHAKE_NODES ? hs->node_count : MESH_UI_MAX_HANDSHAKE_NODES;
    for (uint32_t i = 0U; i < count; ++i) {
        const struct mesh_ui_node_summary *node = &hs->nodes[i];
        if (node->node_id == 0U || node->node_id == self) {
            continue;
        }
        if (node->via_mqtt) {
            out->via_broker += 1U;
            continue;
        }
        if (!node->has_hops_away) {
            continue;
        }
        const uint32_t bucket =
            node->hops_away >= MESH_UI_REACH_MORE ? MESH_UI_REACH_MORE : node->hops_away;
        out->hops[bucket] += 1U;
        out->counted += 1U;
        /* An SNR of exactly zero can be a reading - a link at the noise floor - so the stamp
           says there is one as well as the value. Either will do: a node restored from the card
           keeps its figure and loses its stamp, and is still the neighbour it was. */
        if (bucket != MESH_UI_REACH_DIRECT || (node->snr_time == 0U && node->snr == 0.0f)) {
            continue;
        }
        if (out->strongest < 0 || node->snr > hs->nodes[out->strongest].snr) {
            out->strongest = (int)i;
        }
        if (out->weakest < 0 || node->snr < hs->nodes[out->weakest].snr) {
            out->weakest = (int)i;
        }
    }
}
