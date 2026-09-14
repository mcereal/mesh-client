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

/*
 * The caller's clock, shifted onto this history's own timeline.
 *
 * Resolves a pending resume on the way through: the first reading after a restore is the one
 * that says what the offset is, because it is the first time both clocks are in hand at once.
 * Every note_* goes through this, node readings included - a restore only ever brings the
 * radio's pair back, but one history is one timeline, and a node series stamped with the raw
 * clock while the airtime pair carried the shift would put two readings taken together minutes
 * apart on the same axis.
 */
static uint32_t mesh_ui_history_stamp(struct mesh_ui_history *history, uint32_t now_ms) {
    if (history->resume_pending) {
        history->clock_offset_ms = history->resume_target_ms - now_ms;
        history->resume_pending = false;
    }
    return now_ms + history->clock_offset_ms;
}

void mesh_ui_history_reset(struct mesh_ui_history *history) {
    if (history == NULL) {
        return;
    }
    memset(history, 0, sizeof *history);
    for (uint32_t i = 0U; i < MESH_UI_HISTORY_NODES; ++i) {
        mesh_ui_history_clear_node(&history->nodes[i]);
    }
}

void mesh_ui_history_forget(struct mesh_ui_history *history) { mesh_ui_history_reset(history); }

/* Permille off the wire, held to what sixteen bits and the domain both take. */
static int16_t airtime_permille(int32_t value) {
    if (value < 0) {
        return 0;
    }
    return (int16_t)(value > 1000 ? 1000 : value);
}

/* mesh_ui_series_push()'s rules on the airtime ring: a clock going backwards empties it, the
   oldest is what the newest costs, and a pending break is spent on the sample it lands on. */
static void airtime_push(struct mesh_ui_airtime *log, uint32_t time, int32_t utilization,
                         int32_t tx) {
    if (log->count > 0U) {
        const uint32_t newest = (log->first + log->count - 1U) % MESH_UI_HISTORY_AIRTIME_MAX;
        if (time < log->items[newest].time) {
            memset(log, 0, sizeof *log);
        }
    }
    uint32_t slot;
    if (log->count < MESH_UI_HISTORY_AIRTIME_MAX) {
        slot = (log->first + log->count) % MESH_UI_HISTORY_AIRTIME_MAX;
        ++log->count;
    } else {
        slot = log->first;
        log->first = (log->first + 1U) % MESH_UI_HISTORY_AIRTIME_MAX;
    }
    log->items[slot].time = time;
    log->items[slot].utilization = airtime_permille(utilization);
    log->items[slot].tx = airtime_permille(tx);
    log->items[slot].gap = log->pending_break;
    log->pending_break = false;
}

void mesh_ui_history_note_airtime(struct mesh_ui_history *history, uint32_t now_ms,
                                  int32_t utilization_permille, int32_t tx_permille) {
    if (history == NULL) {
        return;
    }
    const uint32_t stamp = mesh_ui_history_stamp(history, now_ms);
    /* A LocalStats beside a live DeviceMetrics stream is the same two figures a second time. The
       stream counts as live for a gap's worth after its last reading, so a radio that stops
       sending it falls back to LocalStats rather than to nothing. */
    if (history->has_metrics_airtime && stamp >= history->metrics_airtime_at &&
        stamp - history->metrics_airtime_at <= MESH_UI_HISTORY_RADIO_GAP_MS) {
        return;
    }
    airtime_push(&history->airtime, stamp, utilization_permille, tx_permille);
}

void mesh_ui_history_note_metrics_airtime(struct mesh_ui_history *history, uint32_t now_ms,
                                          int32_t utilization_permille, int32_t tx_permille) {
    if (history == NULL) {
        return;
    }
    const uint32_t stamp = mesh_ui_history_stamp(history, now_ms);
    history->metrics_airtime_at = stamp;
    history->has_metrics_airtime = true;
    airtime_push(&history->airtime, stamp, utilization_permille, tx_permille);
}

uint32_t mesh_ui_history_airtime_count(const struct mesh_ui_history *history) {
    return history != NULL ? history->airtime.count : 0U;
}

const struct mesh_ui_airtime_sample *
mesh_ui_history_airtime_at(const struct mesh_ui_history *history, uint32_t index) {
    if (history == NULL || index >= history->airtime.count) {
        return NULL;
    }
    return &history->airtime.items[(history->airtime.first + index) % MESH_UI_HISTORY_AIRTIME_MAX];
}

const struct mesh_ui_airtime_sample *
mesh_ui_history_airtime_newest(const struct mesh_ui_history *history) {
    const uint32_t count = mesh_ui_history_airtime_count(history);
    return count > 0U ? mesh_ui_history_airtime_at(history, count - 1U) : NULL;
}

void mesh_ui_history_restore_airtime(struct mesh_ui_history *history, uint32_t time_ms,
                                     int32_t utilization_permille, int32_t tx_permille, bool gap) {
    if (history == NULL) {
        return;
    }
    /* A break inside the restored window is part of what was watched, so it is put back with
       the reading that carried it rather than recomputed. */
    if (gap) {
        history->airtime.pending_break = true;
    }
    airtime_push(&history->airtime, time_ms, utilization_permille, tx_permille);
}

void mesh_ui_history_resume(struct mesh_ui_history *history, uint32_t seam_ms) {
    if (history == NULL) {
        return;
    }
    const struct mesh_ui_airtime_sample *newest = mesh_ui_history_airtime_newest(history);
    if (newest == NULL) {
        return; /* nothing came back, so there is no timeline to fit the clock to */
    }
    history->resume_target_ms = newest->time + seam_ms;
    history->resume_pending = true;
    history->airtime.pending_break = true;
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
            known->seen = mesh_ui_history_stamp(history, now_ms);
            mesh_ui_series_break(&known->battery);
        }
        return;
    }
    const uint32_t stamp = mesh_ui_history_stamp(history, now_ms);
    struct mesh_ui_history_node *slot = mesh_ui_history_slot(history, node_id);
    slot->seen = stamp;
    mesh_ui_series_push(&slot->battery, stamp, (int32_t)battery_level);
}

bool mesh_ui_history_has_airtime(const struct mesh_ui_history *history) {
    /* Two readings on two ticks: the oldest and the newest differ, so there is a window. The ring
       is in push order and a clock going backwards empties it, so the ends are the extremes. */
    const uint32_t count = mesh_ui_history_airtime_count(history);
    if (count < 2U) {
        return false;
    }
    return mesh_ui_history_airtime_newest(history)->time >
           mesh_ui_history_airtime_at(history, 0U)->time;
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
    const uint32_t stamp = mesh_ui_history_stamp(history, now_ms);
    struct mesh_ui_history_node *slot = mesh_ui_history_slot(history, node_id);
    slot->seen = stamp;
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
        mesh_ui_series_push(&slot->temperature, stamp, temperature_decidegrees);
    }
    if (has_humidity) {
        mesh_ui_series_push(&slot->humidity, stamp, humidity_permille);
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
