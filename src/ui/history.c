#include "mesh/ui/history.h"

#include <string.h>

/* Every series a slot holds, emptied and told the node gap. One function because a reading
   added to the struct and forgotten here is a series with a gap of 0, which never breaks - so
   the first thing it would draw is a line straight across whatever silence came before it. */
static void mesh_ui_history_clear_node(struct mesh_ui_history_node *slot) {
    mesh_ui_series_reset(&slot->battery, MESH_UI_HISTORY_NODE_GAP_MS);
    mesh_ui_series_reset(&slot->temperature, MESH_UI_HISTORY_NODE_GAP_MS);
    mesh_ui_series_reset(&slot->humidity, MESH_UI_HISTORY_NODE_GAP_MS);
}

void mesh_ui_history_reset(struct mesh_ui_history *history) {
    if (history == NULL) {
        return;
    }
    memset(history, 0, sizeof *history);
    mesh_ui_series_reset(&history->channel_utilization, MESH_UI_HISTORY_RADIO_GAP_MS);
    mesh_ui_series_reset(&history->air_util_tx, MESH_UI_HISTORY_RADIO_GAP_MS);
    for (uint32_t i = 0U; i < MESH_UI_HISTORY_NODES; ++i) {
        mesh_ui_history_clear_node(&history->nodes[i]);
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
        /* Every series, not the one the caller is about to push: the slot is being handed to a
           different node, and a temperature left behind by the node evicted out of it would be
           drawn under the arriving node's name. */
        mesh_ui_history_clear_node(oldest);
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

void mesh_ui_history_note_environment(struct mesh_ui_history *history, uint32_t now_ms,
                                      uint32_t node_id, bool has_temperature,
                                      int32_t temperature_decidegrees, bool has_humidity,
                                      int32_t humidity_permille) {
    if (history == NULL || node_id == 0U) {
        return;
    }
    /*
     * Nothing reported is not a reading, and it is not a slot either.
     *
     * EnvironmentMetrics is an optional-field message and most nodes carrying one fill in a
     * subset of it - a barometer with no thermometer is a real device. So a report arriving with
     * neither of the two readings this keeps is a node saying something about its air pressure,
     * and taking a slot for it would evict a node whose temperature somebody is watching in
     * order to remember that this one has none. The battery push next door refuses its own
     * version of this for the same reason.
     */
    if (!has_temperature && !has_humidity) {
        return;
    }
    struct mesh_ui_history_node *slot = mesh_ui_history_slot(history, node_id);
    slot->seen = now_ms;
    /*
     * Each reading on its own, because a node reports the two independently and a report that
     * dropped one of them is a silence in that series rather than in the other. Pushed under one
     * stamp, so the pair is one moment.
     *
     * There is no break to arm on the half that went missing: an absent field is exactly the
     * silence `gap_ms` was written for, and the reading coming back after three quiet reports is
     * either inside the window - in which case it really does continue - or past it, in which
     * case the series breaks it without being told. That is the difference from the battery's
     * external-power case, which arrives *punctually* and so is invisible to the clock.
     */
    if (has_temperature) {
        mesh_ui_series_push(&slot->temperature, now_ms, temperature_decidegrees);
    }
    if (has_humidity) {
        mesh_ui_series_push(&slot->humidity, now_ms, humidity_permille);
    }
}

const struct mesh_ui_series *mesh_ui_history_series(const struct mesh_ui_history *history,
                                                    uint32_t node_id,
                                                    enum mesh_ui_history_reading reading) {
    if (history == NULL || node_id == 0U) {
        return NULL;
    }
    for (uint32_t i = 0U; i < MESH_UI_HISTORY_NODES; ++i) {
        const struct mesh_ui_history_node *slot = &history->nodes[i];
        if (slot->node_id != node_id) {
            continue;
        }
        const struct mesh_ui_series *series = NULL;
        switch (reading) {
        case MESH_UI_HISTORY_BATTERY:
            series = &slot->battery;
            break;
        case MESH_UI_HISTORY_TEMPERATURE:
            series = &slot->temperature;
            break;
        case MESH_UI_HISTORY_HUMIDITY:
            series = &slot->humidity;
            break;
        case MESH_UI_HISTORY_NONE:
        case MESH_UI_HISTORY_READING_COUNT:
        default:
            return NULL;
        }
        /*
         * An empty series is the same answer as no slot at all: nothing has been kept. Whether
         * what *is* kept can be drawn is a further question and deliberately not this one - the
         * airtime pair next door is shaped the same way, with the series reachable on the struct
         * and mesh_ui_history_has_airtime() answering separately for the picture. A caller that
         * wanted the samples rather than a stroke would have nowhere else to go.
         */
        return series->count > 0U ? series : NULL;
    }
    return NULL;
}
