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
     * falling. The reading is refused and the trend simply has a gap there, which is true.
     */
    if (battery_level > 100U) {
        return;
    }
    struct mesh_ui_history_node *slot = mesh_ui_history_slot(history, node_id);
    slot->seen = now_ms;
    mesh_ui_series_push(&slot->battery, now_ms, (int32_t)battery_level);
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
