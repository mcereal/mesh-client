#include "mesh/ui/history.h"

#include <string.h>

void mesh_ui_history_reset(struct mesh_ui_history *history) {
    if (history == NULL) {
        return;
    }
    memset(history, 0, sizeof *history);
    mesh_ui_series_reset(&history->channel_utilization, MESH_UI_HISTORY_RADIO_GAP_MS);
    mesh_ui_series_reset(&history->air_util_tx, MESH_UI_HISTORY_RADIO_GAP_MS);
    for (uint32_t i = 0U; i < MESH_UI_HISTORY_NODES; ++i) {
        mesh_ui_series_reset(&history->nodes[i].battery, MESH_UI_HISTORY_NODE_GAP_MS);
    }
}

void mesh_ui_history_forget(struct mesh_ui_history *history) { mesh_ui_history_reset(history); }

void mesh_ui_history_note_airtime(struct mesh_ui_history *history, uint32_t now_ms,
                                  int32_t utilization_permille, int32_t tx_permille) {
    if (history == NULL) {
        return;
    }
    mesh_ui_series_push(&history->channel_utilization, now_ms, utilization_permille);
    mesh_ui_series_push(&history->air_util_tx, now_ms, tx_permille);
}

/* The slot this node already has, or NULL. Separate from the one below because two callers
   want different answers to "no slot": a reading takes one, and a discontinuity in a trend
   nothing has been watching is not a thing to start watching. */
static struct mesh_ui_history_node *mesh_ui_history_find(struct mesh_ui_history *history,
                                                         uint32_t node_id) {
    for (uint32_t i = 0U; i < MESH_UI_HISTORY_NODES; ++i) {
        if (history->nodes[i].node_id == node_id) {
            return &history->nodes[i];
        }
    }
    return NULL;
}

/*
 * The slot this node has, or the one it takes.
 *
 * Eviction is by `seen` rather than by how full a series is: a node that stopped reporting an
 * hour ago has a complete trend and nothing more to add to it, and one arriving now is the one
 * somebody is looking at. A free slot always wins over an occupied one, so a mesh with fewer
 * nodes than slots never evicts at all.
 */
static struct mesh_ui_history_node *mesh_ui_history_slot(struct mesh_ui_history *history,
                                                         uint32_t node_id) {
    struct mesh_ui_history_node *oldest = &history->nodes[0];
    for (uint32_t i = 0U; i < MESH_UI_HISTORY_NODES; ++i) {
        struct mesh_ui_history_node *slot = &history->nodes[i];
        if (slot->node_id == node_id) {
            return slot;
        }
        if (oldest->node_id != 0U && (slot->node_id == 0U || slot->seen < oldest->seen)) {
            oldest = slot;
        }
    }
    if (oldest->node_id != node_id) {
        oldest->node_id = node_id;
        mesh_ui_series_reset(&oldest->battery, MESH_UI_HISTORY_NODE_GAP_MS);
    }
    return oldest;
}

void mesh_ui_history_note_battery(struct mesh_ui_history *history, uint32_t now_ms,
                                  uint32_t node_id, uint8_t battery_level) {
    if (history == NULL || node_id == 0U) {
        return;
    }
    /*
     * 101 is the firmware's "running off external power", which every phone app draws as a plug
     * rather than as a level. It is not a hundred and one percent of anything, so it is not a
     * point on a line: a node that spends the night on mains and the morning on its battery
     * would otherwise show a cliff at dawn that is the plug being pulled, not the charge
     * falling.
     *
     * Refusing it is not enough on its own, and this is the one place the elapsed-time gap
     * cannot do the work. A node on mains is still reporting, punctually, well inside its gap
     * window - so the two real readings either side of an hour of external power are half an
     * hour apart on the clock with an hour of unknown battery between them, and a line drawn
     * straight through would claim a charge nobody measured. The series is told, and the next
     * reading starts a segment of its own.
     *
     * Only for a node that already has a trend. A node we have never had a level from has
     * nothing to discontinue, and claiming a slot to record that it is plugged in would evict a
     * node whose battery we are actually watching.
     */
    if (battery_level > 100U) {
        struct mesh_ui_history_node *known = mesh_ui_history_find(history, node_id);
        if (known != NULL && known->battery.count > 0U) {
            known->seen = now_ms;
            mesh_ui_series_break(&known->battery);
        }
        return;
    }
    struct mesh_ui_history_node *slot = mesh_ui_history_slot(history, node_id);
    slot->seen = now_ms;
    mesh_ui_series_push(&slot->battery, now_ms, (int32_t)battery_level);
}

bool mesh_ui_history_has_airtime(const struct mesh_ui_history *history) {
    /* The channel's own series rather than both: the two are pushed together by
       mesh_ui_history_note_airtime(), so they break in the same places, and asking about one of
       a pair that arrives in lockstep is asking about the pair.

       A segment rather than a count, because what the verb offers is a *picture*. Two reports
       either side of a link that was down for a quarter of an hour are two samples the ring
       holds and no line at all - the second one starts a segment rather than continuing the
       first - so a count would offer a chart with axes, a legend and nothing between them. */
    return history != NULL && mesh_ui_series_has_segment(&history->channel_utilization);
}

const struct mesh_ui_series *mesh_ui_history_battery(const struct mesh_ui_history *history,
                                                     uint32_t node_id) {
    if (history == NULL || node_id == 0U) {
        return NULL;
    }
    for (uint32_t i = 0U; i < MESH_UI_HISTORY_NODES; ++i) {
        const struct mesh_ui_history_node *slot = &history->nodes[i];
        if (slot->node_id == node_id && slot->battery.count > 0U) {
            return &slot->battery;
        }
    }
    return NULL;
}
