#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/store.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"

#include "store_internal.h"

#include "mesh/core/message.h"
/* For enum mesh_traceroute_state, which the UI's traceroute carries as a byte: telling a trace
   in flight from a route already measured is the one question this file asks of it. */
#include "mesh/core/session.h"
/* For mesh_ui_node_signal_heard(): whether a node's SNR is a measurement of its own link. A
   question about a node summary rather than about the screen the header is named for, and the
   one answer to it - the trend kept here and the bar drawn there must not decide it apart. */
#include "mesh/ui/node_detail.h"
#include "mesh/ui/settings.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void mesh_ui_store_mark_dirty(struct mesh_ui_store *store, mesh_ui_update_flags flags) {
    if (store == NULL || flags == MESH_UI_UPDATE_NONE) {
        return;
    }

    store->pending_flags |= flags;

    if (store->event_wake.fd >= 0) {
        const int signalled = inkwell_wake_signal(&store->event_wake);
        if (signalled < 0) {
            inkwell_log_warn("ui", "store wake write failed: %s", strerror(-signalled));
        }
    }
}

int mesh_ui_store_init(struct mesh_ui_store *store) {
    if (store == NULL) {
        return -EINVAL;
    }

    memset(store, 0, sizeof *store);
    mesh_ui_nav_init(&store->nav);
    mesh_ui_history_reset(&store->history);
    const int opened = inkwell_wake_open(&store->event_wake);
    if (opened < 0) {
        inkwell_log_error("ui", "creating the store wake failed: %s", strerror(-opened));
        return opened;
    }

    return 0;
}

void mesh_ui_store_shutdown(struct mesh_ui_store *store) {
    if (store == NULL) {
        return;
    }

    inkwell_wake_close(&store->event_wake);
    store->pending_flags = MESH_UI_UPDATE_NONE;
    store->device_count = 0U;
    store->network_host[0] = '\0';
    store->handshake_valid = false;
    memset(&store->messages, 0, sizeof store->messages);
    memset(&store->thread, 0, sizeof store->thread);
}

bool mesh_ui_store_handle_key(struct mesh_ui_store *store, enum inkcell_key key,
                              struct mesh_ui_action *out_action) {
    if (store == NULL) {
        if (out_action != NULL) {
            memset(out_action, 0, sizeof *out_action);
        }
        return false;
    }

    /* Lists may have changed since the last frame; a stale cursor would act on the wrong row. */
    mesh_ui_nav_clamp(&store->nav, store);
    const bool changed = mesh_ui_nav_handle_key(&store->nav, store, key, out_action);
    /* A press that raised a notice could not date it - see mesh_ui_nav_raise_toast(). The clock
       the last tick carried is the one the frames are driven by, which on the device is
       CLOCK_MONOTONIC and in a capture is the scene's own, and it is right here. */
    mesh_ui_nav_date_toast(&store->nav, store->now_ms);
    if (changed) {
        mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
    }
    return changed;
}

void mesh_ui_store_set_toast(struct mesh_ui_store *store, uint64_t now_ms, const char *text) {
    if (store == NULL) {
        return;
    }
    mesh_ui_nav_set_toast(&store->nav, now_ms, text);
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
}

void mesh_ui_store_post_toast(struct mesh_ui_store *store, uint64_t now_ms, const char *text) {
    if (store == NULL) {
        return;
    }
    mesh_ui_nav_post_toast(&store->nav, now_ms, text);
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
}

/* The pairing agent's question, pushed in from the app rather than raised by a key press:
   BlueZ blocks the bond until it is answered, so it takes over the screen wherever the user
   happens to be. */
void mesh_ui_store_open_passkey_prompt(struct mesh_ui_store *store, const char *label,
                                       uint32_t passkey, bool confirm) {
    if (store == NULL) {
        return;
    }
    if (mesh_ui_nav_open_passkey(&store->nav, label, passkey, confirm)) {
        mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
    }
}

void mesh_ui_store_close_passkey_prompt(struct mesh_ui_store *store) {
    if (store == NULL) {
        return;
    }
    if (mesh_ui_nav_close_passkey(&store->nav)) {
        mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
    }
}

/* The verification sheet and its keyboard, on exactly the same terms as the prompt above: the
   app opens them from the ceremony's stage, because what raises them is a ClientNotification. */
bool mesh_ui_store_open_verify_sheet(struct mesh_ui_store *store) {
    if (store == NULL) {
        return false;
    }
    if (mesh_ui_nav_open_verify(&store->nav)) {
        mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
    }
    /* Whether it is up, not whether this call raised it: a sheet already standing is the answer
       the caller wants, and a pairing prompt refusing it is not. */
    return store->nav.verify_open;
}

void mesh_ui_store_close_verify_sheet(struct mesh_ui_store *store) {
    if (store == NULL) {
        return;
    }
    if (mesh_ui_nav_close_verify(&store->nav)) {
        mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
    }
}

bool mesh_ui_store_open_verify_number(struct mesh_ui_store *store) {
    if (store == NULL) {
        return false;
    }
    if (mesh_ui_nav_open_verify_number(&store->nav)) {
        mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
    }
    return store->nav.keyboard_verify;
}

void mesh_ui_store_close_verify_number(struct mesh_ui_store *store) {
    if (store == NULL) {
        return;
    }
    if (mesh_ui_nav_close_verify_number(&store->nav)) {
        mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
    }
}

void mesh_ui_store_settings_edits_clear(struct mesh_ui_store *store) {
    if (store == NULL) {
        return;
    }
    if (store->nav.settings_edit_count == 0U && !store->nav.settings_discard_armed &&
        !store->nav.confirm_open) {
        return;
    }
    memset(store->nav.settings_edits, 0, sizeof store->nav.settings_edits);
    store->nav.settings_edit_count = 0U;
    store->nav.settings_discard_armed = false;
    store->nav.confirm_open = false;
    store->nav.confirm_action = (uint8_t)MESH_UI_SETTINGS_ACTION_NONE;
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
}

void mesh_ui_store_settings_edits_consumed(struct mesh_ui_store *store,
                                           enum mesh_ui_setting_consumer consumer) {
    if (store == NULL) {
        return;
    }
    struct mesh_ui_nav *nav = &store->nav;
    uint8_t kept = 0U;
    for (uint8_t i = 0; i < nav->settings_edit_count && i < MESH_UI_SETTINGS_EDITS_MAX; ++i) {
        if (mesh_ui_settings_field_consumer(
                (enum mesh_ui_setting_field)nav->settings_edits[i].field) == consumer) {
            continue; /* written by the press that just fired */
        }
        if (kept != i) {
            nav->settings_edits[kept] = nav->settings_edits[i];
        }
        ++kept;
    }
    if (kept == nav->settings_edit_count && !nav->settings_discard_armed) {
        return;
    }
    for (uint8_t i = kept; i < MESH_UI_SETTINGS_EDITS_MAX; ++i) {
        memset(&nav->settings_edits[i], 0, sizeof nav->settings_edits[i]);
    }
    nav->settings_edit_count = kept;
    /* The question B asked was about the edits that have just gone; whatever is left is work
       the user has not been asked about yet. */
    nav->settings_discard_armed = false;
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
}

void mesh_ui_store_tick(struct mesh_ui_store *store, uint64_t now_ms) {
    if (store == NULL) {
        return;
    }
    /* The clock a history sample is stamped with. Kept here rather than passed to the setters
       because a publish reaches the store from wherever the reading arrived - see the field. */
    store->now_ms = now_ms;
    if (mesh_ui_nav_tick(&store->nav, now_ms)) {
        mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_NAV);
    }
}

int mesh_ui_store_event_fd(const struct mesh_ui_store *store) {
    if (store == NULL) {
        return -1;
    }
    return store->event_wake.fd;
}

void mesh_ui_store_set_discovery(struct mesh_ui_store *store, const struct mesh_ui_device *devices,
                                 size_t count) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_device next[MESH_UI_MAX_DEVICES];
    memset(next, 0, sizeof(next));

    const size_t capped = (count > MESH_UI_MAX_DEVICES) ? MESH_UI_MAX_DEVICES : count;
    if (devices != NULL && capped > 0U) {
        memcpy(next, devices, capped * sizeof(struct mesh_ui_device));
    }

    const bool count_changed = (store->device_count != capped);
    const bool payload_changed = (memcmp(store->devices, next, sizeof(next)) != 0);

    if (!count_changed && !payload_changed) {
        return;
    }

    memcpy(store->devices, next, sizeof(next));
    store->device_count = capped;

    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_DISCOVERY);
}

void mesh_ui_store_set_network_host(struct mesh_ui_store *store, const char *host) {
    if (store == NULL) {
        return;
    }
    char next[MESH_UI_NETWORK_HOST_MAX];
    inkwell_str_copy(next, sizeof next, host != NULL ? host : "");
    if (strcmp(store->network_host, next) == 0) {
        return;
    }
    inkwell_str_copy(store->network_host, sizeof store->network_host, next);
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_DISCOVERY);
}

/* The node with that id in a published roster, or NULL. A linear walk because the roster is
   ranked by last_heard and re-ranked on every publish, so a row index means a different node
   from one frame to the next - the same reason the open node detail is remembered by id. */
static const struct mesh_ui_node_summary *
mesh_ui_store_find_node(const struct mesh_ui_handshake_state *handshake, uint32_t node_id) {
    if (handshake == NULL || node_id == 0U) {
        return NULL;
    }
    const uint32_t count = handshake->node_count < MESH_UI_MAX_HANDSHAKE_NODES
                               ? handshake->node_count
                               : MESH_UI_MAX_HANDSHAKE_NODES;
    for (uint32_t i = 0U; i < count; ++i) {
        if (handshake->nodes[i].node_id == node_id) {
            return &handshake->nodes[i];
        }
    }
    return NULL;
}

/*
 * What the incoming roster adds to the client's memory of the mesh.
 *
 * Called before the roster is replaced, because the question each node asks is whether *this*
 * report is one we already have - and the answer is in the copy about to be overwritten. A
 * node's device metrics carry no usable stamp for the reason the radio's own report does not
 * (see mesh_ui_store_set_settings), so the test is again the reading having changed: the
 * telemetry group as a whole, which carries an uptime that moves even when the battery has not.
 *
 * A different radio drops everything first. Node numbers are the mesh's, not the radio's, but a
 * different radio is a different mesh - and a trend stitched across the swap would draw one
 * node's battery falling into another node's.
 */
static void mesh_ui_store_note_roster(struct mesh_ui_store *store,
                                      const struct mesh_ui_handshake_state *next) {
    /* A swap makes the roster about to be replaced no evidence at all: the readings it holds
       belong to the mesh we have just left, so every node in the arriving one is a first
       report rather than a repeat of one. */
    const bool swapped =
        store->handshake.roster_owner != 0U && next->roster_owner != store->handshake.roster_owner;
    if (swapped) {
        mesh_ui_history_forget(&store->history);
        /* Every path starts at *us*, so a route belongs to the radio that measured it as
           squarely as a trend does - the slot the attempt is in as much as the log, since a
           trace that was in flight was in flight over the link we have just put down. */
        memset(&store->traceroute, 0, sizeof store->traceroute);
        memset(&store->traceroutes, 0, sizeof store->traceroutes);
    }
    const uint32_t count = next->node_count < MESH_UI_MAX_HANDSHAKE_NODES
                               ? next->node_count
                               : MESH_UI_MAX_HANDSHAKE_NODES;
    for (uint32_t i = 0U; i < count; ++i) {
        const struct mesh_ui_node_summary *node = &next->nodes[i];
        const struct mesh_ui_node_summary *was =
            swapped ? NULL : mesh_ui_store_find_node(&store->handshake, node->node_id);

        const bool metrics_new =
            node->metrics.valid &&
            !(was != NULL && memcmp(&was->metrics, &node->metrics, sizeof node->metrics) == 0);
        if (metrics_new && node->metrics.has_battery) {
            mesh_ui_history_note_battery(&store->history, (uint32_t)store->now_ms, node->node_id,
                                         node->metrics.battery_level);
        }

        /* Our own node's DeviceMetrics are the radio's airtime once a minute - see
           MESH_UI_HISTORY_RADIO_REPORT_MS. The uptime in them moves every report, so a changed
           struct is a new report even on a mesh too quiet to move either figure. */
        if (metrics_new && next->has_my_info && node->node_id == next->my_info.node_num &&
            node->metrics.has_channel_utilization && node->metrics.has_air_util_tx) {
            mesh_ui_history_note_metrics_airtime(
                &store->history, (uint32_t)store->now_ms,
                inkcell_percent_permille(node->metrics.channel_utilization),
                inkcell_percent_permille(node->metrics.air_util_tx));
        }

        /*
         * The same shape for the node's air, and it is a *separate* test rather than a second
         * reading taken off the first.
         *
         * DeviceMetrics and EnvironmentMetrics are two Telemetry variants that arrive in two
         * packets on two schedules - a node can report its battery for an hour without its
         * thermometer saying anything, and a weather station with no battery reports air and
         * nothing else. Keyed on the metrics struct the way the battery is, a sensor reading
         * would be pushed once per battery report and dropped whenever the battery held still,
         * which is a temperature series sampled by the wrong clock.
         *
         * Compared whole for the reason the LocalStats push is: no field of it is a stamp we can
         * trust. `environment.time` is our own clock when the packet landed, which on a Brick
         * with no RTC is 0 on every report, so the struct having changed is the only honest test
         * that a node has said something new.
         */
        if (node->environment.valid &&
            !(was != NULL &&
              memcmp(&was->environment, &node->environment, sizeof node->environment) == 0)) {
            mesh_ui_history_note_environment(
                &store->history, (uint32_t)store->now_ms, node->node_id,
                node->environment.has_temperature,
                inkcell_temperature_decidegrees(node->environment.temperature),
                node->environment.has_humidity,
                inkcell_percent_permille(node->environment.relative_humidity));
        }

        /*
         * And how the packet itself arrived, which is the one thing here that is not telemetry.
         *
         * Keyed on the *measurement* rather than on the arrival, which is the distinction this
         * push got wrong first time round. `last_heard` says a packet landed, and a packet
         * landing is not the same event as a ratio being taken - they come apart in both
         * directions, and each direction costs a different kind of wrong:
         *
         *   - An `rx_snr` of exactly 0.0 is the firmware's "no measurement", so the session
         *     declines to store it (mesh_session_apply_packet()) while still advancing
         *     `last_heard`. Keyed on the arrival, this appended the *previous* packet's ratio as
         *     a fresh reading - a number about a different packet, drawn as evidence, which is
         *     the one thing this whole screen is arranged to prevent.
         *   - Two packets inside one epoch second leave `last_heard` where it was while the
         *     ratio moves, so the second measurement was dropped.
         *
         * `snr_time` is the stamp that tells them apart, set where the reading is stored. The
         * value is tested beside it because the stamp has only second resolution: a second
         * reading inside one second is a new measurement, and a stamp alone cannot see it.
         *
         * A stamp of 0 is a node this run has measured nothing from - every node restored from
         * the card is one, its reading having come back without the moment it was taken - and it
         * pushes nothing rather than dating a cached figure to now.
         *
         * Gated on mesh_ui_node_signal_heard(), which is the rest of what makes this honest: an
         * SNR from a relayed packet describes the relay and a node reached over MQTT crossed no
         * air at all. Both are true numbers about something else, and the node detail refuses to
         * draw a bar on either for the same reason - so the row and the trend behind it are one
         * claim rather than two that agree until one of them is changed.
         *
         * The RSSI rides the same measurement rather than carrying a staleness test of its own.
         * Both stamps are the packet's arrival where the session stored them, so `rssi_time ==
         * snr_time` is exactly "these two came off one packet" - which is the pair this history
         * keeps under one stamp. An RSSI that arrived with no ratio beside it is not recorded,
         * and that is the right way round: the alternative is a lone reading stamped with a
         * moment nothing else on the row shares.
         */
        const bool measured =
            node->snr_time != 0U &&
            (was == NULL || was->snr_time != node->snr_time || was->snr != node->snr);
        if (measured && mesh_ui_node_signal_heard(node)) {
            const bool rssi_now = node->has_rssi && node->rssi_time == node->snr_time;
            mesh_ui_history_note_signal(&store->history, (uint32_t)store->now_ms, node->node_id,
                                        inkcell_snr_db(node->snr), rssi_now,
                                        (int32_t)node->rx_rssi);
        }
    }
}

void mesh_ui_store_set_handshake(struct mesh_ui_store *store,
                                 const struct mesh_ui_handshake_state *handshake) {
    if (store == NULL) {
        return;
    }

    bool next_valid = (handshake != NULL);
    struct mesh_ui_handshake_state next_state;
    memset(&next_state, 0, sizeof(next_state));
    if (next_valid) {
        next_state = *handshake;
    }

    const bool validity_changed = (store->handshake_valid != next_valid);
    const bool payload_changed =
        next_valid && (memcmp(&store->handshake, &next_state, sizeof(next_state)) != 0);

    if (!validity_changed && !payload_changed) {
        return;
    }

    if (next_valid) {
        mesh_ui_store_note_roster(store, &next_state);
        store->handshake = next_state;
    } else {
        memset(&store->handshake, 0, sizeof store->handshake);
    }
    store->handshake_valid = next_valid;

    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_HANDSHAKE);
}

void mesh_ui_store_set_transport_status(struct mesh_ui_store *store, const char *status) {
    if (store == NULL) {
        return;
    }

    char next[MESH_UI_TRANSPORT_STATUS_MAX];
    memset(next, 0, sizeof next);
    if (status != NULL) {
        snprintf(next, sizeof next, "%s", status);
    }

    if (memcmp(store->transport_status, next, sizeof next) == 0) {
        return;
    }

    memcpy(store->transport_status, next, sizeof store->transport_status);
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_TRANSPORT);
}

void mesh_ui_store_set_mqtt(struct mesh_ui_store *store, const struct mesh_ui_mqtt_state *mqtt) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_mqtt_state next;
    memset(&next, 0, sizeof next);
    if (mqtt != NULL) {
        next = *mqtt;
    }

    /*
     * memcmp over the whole record, which is safe here in a way it usually is not: both sides
     * were memset before they were filled - this one above, the caller's in the publish - so the
     * padding is zero in both. The alternative is a field-by-field comparison of thirteen
     * fields that exists only to answer "did anything move", and the field somebody forgets to
     * add to it is a card that stops repainting.
     */
    if (memcmp(&store->mqtt, &next, sizeof next) == 0) {
        return;
    }

    store->mqtt = next;
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_MQTT);
}

void mesh_ui_store_set_settings(struct mesh_ui_store *store,
                                const struct mesh_ui_settings *settings) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_settings next;
    memset(&next, 0, sizeof next);
    if (settings != NULL) {
        next = *settings;
    }
    if (memcmp(&store->settings, &next, sizeof next) == 0) {
        return;
    }
    /*
     * A new LocalStats report is a reading to remember, and the test for one is the report
     * itself having changed rather than any field of it in particular.
     *
     * Its own stamp would be the obvious key and is not usable: `time` is our clock when it
     * arrived, and on a Brick with no wall clock that is 0 on every report - so a series keyed
     * on it would hold exactly one sample for the life of the session. The whole struct is the
     * honest test, because a LocalStats always carries the packet counters and those move
     * whether or not the airtime figures did. A radio that genuinely repeated a report byte for
     * byte contributes no sample, which is the right way round: nothing new was said.
     */
    if (next.stats.valid && memcmp(&next.stats, &store->settings.stats, sizeof next.stats) != 0) {
        mesh_ui_history_note_airtime(&store->history, (uint32_t)store->now_ms,
                                     inkcell_percent_permille(next.stats.channel_utilization),
                                     inkcell_percent_permille(next.stats.air_util_tx));
    }
    store->settings = next;
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_SETTINGS);
}

/*
 * One measured route into the log, newest first.
 *
 * A second trace of a node replaces that node's entry rather than adding one, because what the
 * log answers is "what is the route to that node" and there is only ever one current answer.
 * The move to the front is what makes the cap mean the eight most recently *traced* nodes
 * rather than the first eight ever traced.
 */
static void mesh_ui_traceroute_log_record(struct mesh_ui_traceroute_log *log,
                                          const struct mesh_ui_traceroute *trace) {
    uint8_t at = log->count;
    for (uint8_t i = 0U; i < log->count; ++i) {
        if (log->entries[i].target == trace->target) {
            at = i;
            break;
        }
    }
    /* Everything above the slot being reused - or above the oldest entry, when the log is full
       and nothing here is this node - shuffles down to leave room at the front. */
    if (at >= MESH_UI_TRACEROUTE_LOG_MAX) {
        at = MESH_UI_TRACEROUTE_LOG_MAX - 1U;
    } else if (at == log->count && log->count < MESH_UI_TRACEROUTE_LOG_MAX) {
        log->count++;
    }
    for (uint8_t i = at; i > 0U; --i) {
        log->entries[i] = log->entries[i - 1U];
    }
    log->entries[0] = *trace;
    log->revision++;
}

static const struct mesh_ui_traceroute *
mesh_ui_traceroute_log_find(const struct mesh_ui_traceroute_log *log, uint32_t node_id) {
    for (uint8_t i = 0U; i < log->count && i < MESH_UI_TRACEROUTE_LOG_MAX; ++i) {
        if (log->entries[i].target == node_id) {
            return &log->entries[i];
        }
    }
    return NULL;
}

static const struct mesh_ui_traceroute *
mesh_ui_pick_traceroute(const struct mesh_ui_traceroute *live,
                        const struct mesh_ui_traceroute_log *log, uint32_t node_id) {
    if (node_id == 0U) {
        return NULL;
    }
    /* The attempt wins while it is this node's, whatever it has to say: a trace that is running
       or that timed out is news, and the row that reports it is the same row the route hangs
       under. When it lands it is recorded below and the two agree. */
    if (live->state != MESH_TRACEROUTE_IDLE && live->target == node_id) {
        return live;
    }
    return mesh_ui_traceroute_log_find(log, node_id);
}

const struct mesh_ui_traceroute *mesh_ui_store_traceroute_view(const struct mesh_ui_store *store,
                                                               uint32_t node_id) {
    if (store == NULL) {
        return NULL;
    }
    return mesh_ui_pick_traceroute(&store->traceroute, &store->traceroutes, node_id);
}

const struct mesh_ui_traceroute *
mesh_ui_snapshot_traceroute_view(const struct mesh_ui_snapshot *snapshot, uint32_t node_id) {
    if (snapshot == NULL) {
        return NULL;
    }
    return mesh_ui_pick_traceroute(&snapshot->traceroute, &snapshot->traceroutes, node_id);
}

void mesh_ui_store_set_traceroute(struct mesh_ui_store *store,
                                  const struct mesh_ui_traceroute *traceroute) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_traceroute next;
    memset(&next, 0, sizeof next);
    if (traceroute != NULL) {
        next = *traceroute;
    }
    if (memcmp(&store->traceroute, &next, sizeof next) == 0) {
        return;
    }
    /*
     * A finished trace is the one thing here worth keeping, so it is written down before the
     * slot moves on. The guard is the record rather than the state alone: the session keeps its
     * result until the next trace is sent, so this call arrives with the same DONE record on
     * every publish, and the memcmp above is what makes recording it once mean once.
     */
    if (next.state == MESH_TRACEROUTE_DONE && next.target != 0U && next.forward_count > 0U) {
        mesh_ui_traceroute_log_record(&store->traceroutes, &next);
    }
    store->traceroute = next;
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_TRACEROUTE);
}

void mesh_ui_store_set_verification(struct mesh_ui_store *store,
                                    const struct mesh_ui_verification *verification) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_verification next;
    memset(&next, 0, sizeof next);
    if (verification != NULL) {
        next = *verification;
    }
    if (memcmp(&store->verification, &next, sizeof next) == 0) {
        return;
    }
    store->verification = next;
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_VERIFY);
}

void mesh_ui_store_set_messages(struct mesh_ui_store *store,
                                const struct mesh_ui_message_list *messages) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_message_list next;
    memset(&next, 0, sizeof(next));
    if (messages != NULL) {
        next = *messages;
        if (next.count > MESH_UI_MAX_MESSAGES) {
            next.count = MESH_UI_MAX_MESSAGES;
        }
        /* Zero the unused tail so the memcmp below compares like with like. */
        for (uint32_t i = next.count; i < MESH_UI_MAX_MESSAGES; ++i) {
            memset(&next.entries[i], 0, sizeof(next.entries[i]));
        }
    }

    if (memcmp(&store->messages, &next, sizeof(next)) == 0) {
        return;
    }

    store->messages = next;
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_MESSAGES);
}

void mesh_ui_store_set_waypoints(struct mesh_ui_store *store,
                                 const struct mesh_ui_waypoint_list *waypoints) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_waypoint_list next;
    memset(&next, 0, sizeof(next));
    if (waypoints != NULL) {
        next = *waypoints;
        if (next.count > MESH_UI_MAX_WAYPOINTS) {
            next.count = MESH_UI_MAX_WAYPOINTS;
        }
        /* Zero the unused tail so the memcmp below compares like with like. */
        for (uint32_t i = next.count; i < MESH_UI_MAX_WAYPOINTS; ++i) {
            memset(&next.entries[i], 0, sizeof(next.entries[i]));
        }
    }

    if (memcmp(&store->waypoints, &next, sizeof(next)) == 0) {
        return;
    }

    store->waypoints = next;
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_WAYPOINTS);
}

static bool mesh_ui_message_list_contains(const struct mesh_ui_message_list *list,
                                          uint32_t packet_id) {
    /* Packet id 0 means "no id" in the Meshtastic protocol, so it never identifies anything. */
    if (list == NULL || packet_id == 0U) {
        return false;
    }
    for (uint32_t i = 0; i < list->count && i < MESH_UI_MAX_MESSAGES; ++i) {
        if (list->entries[i].packet_id == packet_id) {
            return true;
        }
    }
    return false;
}

uint32_t mesh_ui_handshake_off_radio(const struct mesh_ui_handshake_state *handshake) {
    if (handshake == NULL) {
        return 0U;
    }
    const uint32_t count = handshake->node_count > MESH_UI_MAX_HANDSHAKE_NODES
                               ? MESH_UI_MAX_HANDSHAKE_NODES
                               : handshake->node_count;
    uint32_t off = 0U;
    for (uint32_t i = 0; i < count; ++i) {
        if (!handshake->nodes[i].in_nodedb) {
            ++off;
        }
    }
    return off;
}

void mesh_ui_message_list_merge(const struct mesh_ui_message_list *cached,
                                const struct mesh_ui_message_list *live,
                                struct mesh_ui_message_list *out) {
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    const uint32_t live_count =
        (live == NULL) ? 0U
                       : (live->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : live->count);

    /* Live messages are the newest and always keep their slots; history fills what is left. */
    const uint32_t room = MESH_UI_MAX_MESSAGES - live_count;

    uint32_t eligible[MESH_UI_MAX_MESSAGES];
    uint32_t eligible_count = 0U;
    if (cached != NULL) {
        for (uint32_t i = 0; i < cached->count && i < MESH_UI_MAX_MESSAGES; ++i) {
            if (!mesh_ui_message_list_contains(live, cached->entries[i].packet_id)) {
                eligible[eligible_count++] = i;
            }
        }
    }

    /* Too much history for the room left: drop the oldest of it. */
    const uint32_t skipped = (eligible_count > room) ? eligible_count - room : 0U;

    for (uint32_t i = skipped; i < eligible_count; ++i) {
        out->entries[out->count++] = cached->entries[eligible[i]];
    }
    for (uint32_t i = 0; i < live_count; ++i) {
        out->entries[out->count++] = live->entries[i];
    }

    out->dropped =
        ((cached != NULL) ? cached->dropped : 0U) + ((live != NULL) ? live->dropped : 0U) + skipped;
}

/* Whether a UI message belongs to the conversation named by (kind, node, channel). The store's
   copy already carries the resolved peer and a broadcast flag, so this is the same question
   mesh_message_in_conversation() answers on the transport's ring, asked of the other shape. */
static bool mesh_ui_message_belongs(const struct mesh_ui_message *message, uint8_t kind,
                                    uint32_t node, uint8_t channel) {
    switch ((enum mesh_ui_conversation_kind)kind) {
    case MESH_UI_CONVERSATION_CHANNEL:
        return message->broadcast && message->channel == channel;
    case MESH_UI_CONVERSATION_DIRECT:
        return !message->broadcast && message->peer == node;
    case MESH_UI_CONVERSATION_ALL:
    case MESH_UI_CONVERSATION_NEW:
    default:
        return false;
    }
}

uint32_t mesh_ui_message_list_forget(struct mesh_ui_message_list *list, uint8_t kind, uint32_t node,
                                     uint8_t channel) {
    if (list == NULL) {
        return 0U;
    }
    const uint32_t count = list->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : list->count;
    uint32_t kept = 0U;
    uint32_t removed = 0U;
    for (uint32_t i = 0; i < count; ++i) {
        if (mesh_ui_message_belongs(&list->entries[i], kind, node, channel)) {
            removed++;
            continue;
        }
        if (kept != i) {
            list->entries[kept] = list->entries[i];
        }
        kept++;
    }
    if (removed > 0U) {
        memset(&list->entries[kept], 0, (count - kept) * sizeof list->entries[0]);
        list->count = kept;
    }
    return removed;
}

/* Whether this entry is the message `packet_id` names, or a reaction drawn on it. Says nothing
   about which conversation it is in - that is mesh_ui_message_belongs()'s question, and both
   have to be true before anything is thrown away. */
static bool mesh_ui_message_names(const struct mesh_ui_message *entry, uint32_t packet_id) {
    return (entry->packet_id == packet_id) || (entry->is_reaction && entry->reply_id == packet_id);
}

uint32_t mesh_ui_message_list_forget_message(struct mesh_ui_message_list *list, uint8_t kind,
                                             uint32_t node, uint8_t channel, uint32_t packet_id) {
    if (list == NULL || packet_id == 0U) {
        return 0U;
    }
    const uint32_t count = list->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : list->count;
    uint32_t kept = 0U;
    uint32_t removed = 0U;
    for (uint32_t i = 0; i < count; ++i) {
        const struct mesh_ui_message *entry = &list->entries[i];
        /* The bubble itself, and the reactions that were drawn on it, in this conversation and
           no other. See the header for both halves. */
        const bool drop = mesh_ui_message_names(entry, packet_id) &&
                          mesh_ui_message_belongs(entry, kind, node, channel);
        if (drop) {
            removed++;
            continue;
        }
        if (kept != i) {
            list->entries[kept] = list->entries[i];
        }
        kept++;
    }
    if (removed > 0U) {
        memset(&list->entries[kept], 0, (count - kept) * sizeof list->entries[0]);
        list->count = kept;
    }
    return removed;
}

void mesh_ui_thread_merge(struct mesh_ui_thread *thread, const struct mesh_ui_message_list *live) {
    if (thread == NULL || live == NULL) {
        return;
    }
    const uint32_t count = live->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : live->count;
    for (uint32_t i = 0; i < count; ++i) {
        const struct mesh_ui_message *message = &live->entries[i];
        if (!mesh_ui_message_belongs(message, thread->kind, thread->node, thread->channel)) {
            continue;
        }

        /* Already in the window: the same message, further along. Replaced where it sits, so a
           bubble does not jump to the bottom of the transcript when its ack arrives. */
        /* Matched on the sender as well as the id, for the archive's reason: an id is unique
           per sender, so two nodes on one channel can share one and folding them together
           would drop a message the reader has every right to see. */
        bool held = false;
        if (message->packet_id != 0U) {
            for (uint32_t j = 0; j < thread->count; ++j) {
                const struct mesh_ui_message *seen = &thread->entries[j];
                if (seen->packet_id == message->packet_id && seen->peer == message->peer &&
                    seen->direction == message->direction) {
                    thread->entries[j] = *message;
                    held = true;
                    break;
                }
            }
        }
        if (held) {
            continue;
        }

        if (thread->count >= MESH_UI_MAX_THREAD_MESSAGES) {
            /* Full: the oldest goes, and is counted as gone, exactly as the archive reader
               counts what its own ring overwrote. */
            memmove(&thread->entries[0], &thread->entries[1],
                    (MESH_UI_MAX_THREAD_MESSAGES - 1U) * sizeof thread->entries[0]);
            thread->count = MESH_UI_MAX_THREAD_MESSAGES - 1U;
            thread->dropped++;
        }
        thread->entries[thread->count] = *message;
        thread->count++;
    }
}

struct mesh_ui_message_view mesh_ui_message_list_view(const struct mesh_ui_message_list *list) {
    if (list == NULL) {
        return (struct mesh_ui_message_view){NULL, 0U, 0U};
    }
    const uint32_t count = list->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : list->count;
    return (struct mesh_ui_message_view){list->entries, count, list->dropped};
}

struct mesh_ui_message_view mesh_ui_thread_view(const struct mesh_ui_thread *thread) {
    if (thread == NULL || !thread->valid) {
        return (struct mesh_ui_message_view){NULL, 0U, 0U};
    }
    const uint32_t count =
        thread->count > MESH_UI_MAX_THREAD_MESSAGES ? MESH_UI_MAX_THREAD_MESSAGES : thread->count;
    return (struct mesh_ui_message_view){thread->entries, count, thread->dropped};
}

/*
 * Whether the window in hand is a window over the conversation the nav has open.
 *
 * Asked before every draw rather than trusted, because the two move independently: the nav
 * turns on a press and the window is refilled a publish later, so for one frame after opening a
 * thread the window still holds the conversation the reader just left. Drawing it would put
 * somebody else's messages under the right title, which is the one failure mode a transcript
 * must not have.
 */
static bool mesh_ui_thread_matches(const struct mesh_ui_thread *thread,
                                   const struct mesh_ui_nav *nav) {
    if (thread == NULL || nav == NULL || !thread->valid || !nav->thread_open || nav->inbox) {
        return false;
    }
    if (nav->target_node == MESH_MESSAGE_BROADCAST_ADDR) {
        return thread->kind == (uint8_t)MESH_UI_CONVERSATION_CHANNEL &&
               thread->channel == nav->target_channel;
    }
    return thread->kind == (uint8_t)MESH_UI_CONVERSATION_DIRECT && thread->node == nav->target_node;
}

/*
 * The deep window when it is the right one, the flat list otherwise - and the fallback is not a
 * degraded mode. The all-traffic view is several conversations at once and has no window by
 * definition; the conversation list is derived from the flat list; and a thread opened a moment
 * ago is drawn from the flat list for exactly one frame, which shows the newest messages in the
 * right order and is simply shallower than the frame after it.
 *
 * Three arguments rather than a store, because both sides of the publish seam ask it of the
 * record they hold - the nav and the backends of a snapshot, the press handler of the store -
 * and a second copy of this decision is a second opinion about which messages are on screen.
 */
static struct mesh_ui_message_view
mesh_ui_pick_message_view(const struct mesh_ui_thread *thread, const struct mesh_ui_nav *nav,
                          const struct mesh_ui_message_list *messages) {
    if (mesh_ui_thread_matches(thread, nav)) {
        return mesh_ui_thread_view(thread);
    }
    return mesh_ui_message_list_view(messages);
}

struct mesh_ui_message_view mesh_ui_store_message_view(const struct mesh_ui_store *store,
                                                       const struct mesh_ui_nav *nav) {
    if (store == NULL) {
        return (struct mesh_ui_message_view){NULL, 0U, 0U};
    }
    /* `nav` rather than `store->nav`, because the two are not always the same one: a store built
       by mesh_ui_store_view() has no nav at all and its caller carries the real one separately.
       Reading the store's own would be this file quietly answering a different question from the
       one it was asked. */
    return mesh_ui_pick_message_view(&store->thread, nav != NULL ? nav : &store->nav,
                                     &store->messages);
}

struct mesh_ui_message_view mesh_ui_snapshot_message_view(const struct mesh_ui_snapshot *snapshot) {
    if (snapshot == NULL) {
        return (struct mesh_ui_message_view){NULL, 0U, 0U};
    }
    return mesh_ui_pick_message_view(&snapshot->thread, &snapshot->nav, &snapshot->messages);
}

void mesh_ui_store_set_thread(struct mesh_ui_store *store, const struct mesh_ui_thread *thread) {
    if (store == NULL) {
        return;
    }
    struct mesh_ui_thread next;
    memset(&next, 0, sizeof next);
    if (thread != NULL) {
        next = *thread;
        if (next.count > MESH_UI_MAX_THREAD_MESSAGES) {
            next.count = MESH_UI_MAX_THREAD_MESSAGES;
        }
    }
    if (memcmp(&store->thread, &next, sizeof next) == 0) {
        return;
    }
    store->thread = next;
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_MESSAGES);
}

uint32_t mesh_ui_store_forget_message(struct mesh_ui_store *store, uint8_t kind, uint32_t node,
                                      uint8_t channel, uint32_t packet_id) {
    if (store == NULL || packet_id == 0U) {
        return 0U;
    }
    const uint32_t removed =
        mesh_ui_message_list_forget_message(&store->messages, kind, node, channel, packet_id);

    /*
     * And out of the window under the reader's eyes, which is a separate list holding a
     * separate copy - the bubble being deleted is almost always one the deep window is what
     * put on screen, so a delete that reached only the flat list would appear to do nothing.
     *
     * The read mark is deliberately left alone, unlike a conversation delete. It names a packet
     * id as "everything up to here has been seen", and one message going does not un-see the
     * rest; a mark whose own message has been deleted reads as "everything still in view
     * arrived after it", which is the ring's rule and the right one here.
     */
    struct mesh_ui_thread *thread = &store->thread;
    uint32_t kept = 0U;
    const uint32_t count =
        thread->count > MESH_UI_MAX_THREAD_MESSAGES ? MESH_UI_MAX_THREAD_MESSAGES : thread->count;
    for (uint32_t i = 0; i < count; ++i) {
        const struct mesh_ui_message *entry = &thread->entries[i];
        /* The window is a window over one conversation already, so the second half of this is
           always true here - asked anyway, through the same predicate, so the two lists cannot
           come to disagree about what a delete reaches. */
        if (mesh_ui_message_names(entry, packet_id) &&
            mesh_ui_message_belongs(entry, kind, node, channel)) {
            continue;
        }
        if (kept != i) {
            thread->entries[kept] = thread->entries[i];
        }
        kept++;
    }
    if (kept != count) {
        memset(&thread->entries[kept], 0, (count - kept) * sizeof thread->entries[0]);
        thread->count = kept;
    }

    const uint32_t from_window = count - kept;
    if (removed > 0U || from_window > 0U) {
        mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_MESSAGES);
    }
    /* The larger of the two, because they are two copies of one transcript rather than two
       transcripts: the window is the deeper of them, so a bubble the flat list had already let
       go still counts as a message the reader has just deleted. */
    return removed > from_window ? removed : from_window;
}

uint32_t mesh_ui_store_forget_conversation(struct mesh_ui_store *store, uint8_t kind, uint32_t node,
                                           uint8_t channel) {
    if (store == NULL) {
        return 0U;
    }
    const uint32_t removed = mesh_ui_message_list_forget(&store->messages, kind, node, channel);

    /* The window goes with the conversation it was a window over. It is the deeper of the two
       copies, so leaving it would put the deleted messages straight back on screen the moment
       the reader opened the thread again. */
    if (store->thread.valid && store->thread.kind == kind &&
        ((kind == (uint8_t)MESH_UI_CONVERSATION_CHANNEL && store->thread.channel == channel) ||
         (kind == (uint8_t)MESH_UI_CONVERSATION_DIRECT && store->thread.node == node))) {
        memset(&store->thread, 0, sizeof store->thread);
    }

    /* The mark goes with the messages it marked. Leaving it would badge the conversation's
       whole next exchange as read, because a mark whose message is gone reads as "everything
       still in view arrived before it" - the rule that makes the ring's evictions safe is
       exactly the wrong one here. */
    struct mesh_ui_read_state *state = &store->read_state;
    for (uint32_t i = 0; i < state->count && i < MESH_UI_READ_MARKS_MAX; ++i) {
        if (state->marks[i].kind != kind) {
            continue;
        }
        const bool match = (kind == MESH_UI_CONVERSATION_CHANNEL)
                               ? state->marks[i].channel == channel
                               : state->marks[i].node == node;
        if (!match) {
            continue;
        }
        state->marks[i] = state->marks[state->count - 1U];
        memset(&state->marks[state->count - 1U], 0, sizeof state->marks[0]);
        state->count--;
        /* One mark fewer is one line fewer in the file, so both counters move. */
        state->stamp++;
        state->revision++; /* the app persists the read state when this moves */
        break;
    }

    if (removed > 0U) {
        mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_MESSAGES);
    }
    return removed;
}

/* The slot holding this conversation's mark, taking a free one or evicting the least recently
   read when they are all in use. Never fails. */
static struct mesh_ui_read_mark *mesh_ui_store_read_mark_slot(struct mesh_ui_read_state *state,
                                                              uint8_t kind, uint32_t node,
                                                              uint8_t channel) {
    for (uint32_t i = 0; i < state->count && i < MESH_UI_READ_MARKS_MAX; ++i) {
        struct mesh_ui_read_mark *mark = &state->marks[i];
        if (mark->kind != kind) {
            continue;
        }
        if (kind == MESH_UI_CONVERSATION_CHANNEL && mark->channel == channel) {
            return mark;
        }
        if (kind == MESH_UI_CONVERSATION_DIRECT && mark->node == node) {
            return mark;
        }
    }

    struct mesh_ui_read_mark *slot = NULL;
    if (state->count < MESH_UI_READ_MARKS_MAX) {
        slot = &state->marks[state->count++];
    } else {
        /*
         * Least recently read, but an unmuted mark first.
         *
         * A read mark is bookkeeping the client can rebuild by being read again; a mute is a
         * choice the user made, and one that vanishes because they opened thirty-two other
         * conversations is a setting that silently undoes itself. Preferring an unmuted victim
         * costs one extra pass and makes the mute as durable as the table can make it. When
         * every slot is muted there is nothing to prefer and the plain LRU stands.
         */
        for (uint32_t i = 0; i < MESH_UI_READ_MARKS_MAX; ++i) {
            if (state->marks[i].muted) {
                continue;
            }
            if (slot == NULL || state->marks[i].stamp < slot->stamp) {
                slot = &state->marks[i];
            }
        }
        if (slot == NULL) {
            slot = &state->marks[0];
            for (uint32_t i = 1; i < MESH_UI_READ_MARKS_MAX; ++i) {
                if (state->marks[i].stamp < slot->stamp) {
                    slot = &state->marks[i];
                }
            }
        }
    }
    memset(slot, 0, sizeof *slot);
    slot->kind = kind;
    slot->node = node;
    slot->channel = channel;
    return slot;
}

/* The mark for one conversation, or NULL when the client holds none. Const twin of the slot
   chooser above, which allocates; a reader must not. */
static const struct mesh_ui_read_mark *
mesh_ui_store_find_read_mark(const struct mesh_ui_read_state *state, uint8_t kind, uint32_t node,
                             uint8_t channel) {
    for (uint32_t i = 0; i < state->count && i < MESH_UI_READ_MARKS_MAX; ++i) {
        const struct mesh_ui_read_mark *mark = &state->marks[i];
        if (mark->kind != kind) {
            continue;
        }
        if (kind == MESH_UI_CONVERSATION_CHANNEL && mark->channel == channel) {
            return mark;
        }
        if (kind == MESH_UI_CONVERSATION_DIRECT && mark->node == node) {
            return mark;
        }
    }
    return NULL;
}

void mesh_ui_store_view(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_store *view) {
    if (snapshot == NULL || view == NULL) {
        return;
    }
    memset(view, 0, sizeof *view);
    memcpy(view->devices, snapshot->devices, sizeof view->devices);
    view->device_count = snapshot->device_count;
    inkwell_str_copy(view->network_host, sizeof view->network_host, snapshot->network_host);
    view->handshake = snapshot->handshake;
    view->handshake_valid = snapshot->handshake_valid;
    view->messages = snapshot->messages;
    /* `thread` is left zeroed for `nav`'s reason, and the two go together: the window is only
       ever read through the nav that says which conversation is open, and this view has none.
       It is also the largest record in a snapshot, and this one is built on the stack. */
    view->waypoints = snapshot->waypoints;
    view->read_state = snapshot->read_state;
    /* The radio's display units, which is what a waypoint's range is stated in. */
    view->settings = snapshot->settings;
    view->event_wake.fd = -1;
    view->event_wake.write_fd = -1;
}

uint32_t mesh_ui_store_conversation_read_mark(const struct mesh_ui_store *store, uint8_t kind,
                                              uint32_t node, uint8_t channel) {
    if (store == NULL) {
        return 0U;
    }
    const struct mesh_ui_read_mark *mark =
        mesh_ui_store_find_read_mark(&store->read_state, kind, node, channel);
    return mark != NULL ? mark->packet_id : 0U;
}

bool mesh_ui_store_conversation_muted_locally(const struct mesh_ui_store *store, uint8_t kind,
                                              uint32_t node, uint8_t channel) {
    if (store == NULL) {
        return false;
    }
    const struct mesh_ui_read_mark *mark =
        mesh_ui_store_find_read_mark(&store->read_state, kind, node, channel);
    return mark != NULL && mark->muted;
}

bool mesh_ui_store_conversation_muted(const struct mesh_ui_store *store, uint8_t kind,
                                      uint32_t node, uint8_t channel) {
    if (store == NULL) {
        return false;
    }
    if (mesh_ui_store_conversation_muted_locally(store, kind, node, channel)) {
        return true;
    }
    /* The radio's half, and only for a direct conversation - see the contract in store.h. The
       roster is walked here rather than through mesh_ui_node_detail_find() because that lives
       one layer up, in the screen that draws a node: a store reaching into it would be the
       seam pointing the wrong way for the sake of a six-line loop. */
    if (kind != (uint8_t)MESH_UI_CONVERSATION_DIRECT || !store->handshake_valid) {
        return false;
    }
    const uint32_t count = store->handshake.node_count > MESH_UI_MAX_HANDSHAKE_NODES
                               ? MESH_UI_MAX_HANDSHAKE_NODES
                               : store->handshake.node_count;
    for (uint32_t i = 0; i < count; ++i) {
        if (store->handshake.nodes[i].node_id == node) {
            return store->handshake.nodes[i].is_muted;
        }
    }
    return false;
}

bool mesh_ui_store_set_conversation_mute(struct mesh_ui_store *store, uint8_t kind, uint32_t node,
                                         uint8_t channel, bool muted) {
    if (store == NULL || (kind != (uint8_t)MESH_UI_CONVERSATION_CHANNEL &&
                          kind != (uint8_t)MESH_UI_CONVERSATION_DIRECT)) {
        return false;
    }
    /* Unmuting something the client holds no mark for is already true, and allocating a slot to
       record it would evict a mark that means something to say nothing. */
    if (!muted && !mesh_ui_store_conversation_muted_locally(store, kind, node, channel)) {
        return false;
    }

    struct mesh_ui_read_mark *mark =
        mesh_ui_store_read_mark_slot(&store->read_state, kind, node, channel);
    if (mark->muted == muted) {
        return false;
    }
    mark->muted = muted;
    /* The stamp is what the eviction above orders by, and a mute is the user touching this
       conversation as much as reading it is. The revision moves with it because a mute is a
       field of the saved line: unlike a re-read that found the mark already in place, this one
       really does leave the file out of date. */
    store->read_state.stamp++;
    mark->stamp = store->read_state.stamp;
    store->read_state.revision++;

    /*
     * An unmute that leaves the mark saying nothing takes the mark with it.
     *
     * A conversation muted before it was ever opened has `packet_id` 0, so once the mute is off
     * the record holds no read position and no mute - it is an empty slot in a table of 32 that
     * still costs a slot, and worse, it has just had its stamp refreshed. The eviction above
     * prefers an unmuted victim and orders by stamp, so this one would be the *last* unmuted
     * mark to go and a genuine read position would be thrown away ahead of it - which reads, on
     * the device, as a conversation the user had read coming back unread.
     */
    if (!mark->muted && mark->packet_id == 0U) {
        struct mesh_ui_read_state *state = &store->read_state;
        const uint32_t index = (uint32_t)(mark - state->marks);
        if (index < state->count) {
            state->marks[index] = state->marks[state->count - 1U];
            memset(&state->marks[state->count - 1U], 0, sizeof state->marks[0]);
            state->count--;
        }
    }

    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_MESSAGES | MESH_UI_UPDATE_NAV);
    return true;
}

bool mesh_ui_store_mark_open_conversation_read(struct mesh_ui_store *store) {
    if (store == NULL || !store->nav.thread_open || store->nav.inbox) {
        return false;
    }

    const bool is_channel = (store->nav.target_node == MESH_MESSAGE_BROADCAST_ADDR);
    const uint8_t kind =
        is_channel ? (uint8_t)MESH_UI_CONVERSATION_CHANNEL : (uint8_t)MESH_UI_CONVERSATION_DIRECT;

    /* The newest message in the conversation that carries an id. Packet id 0 means "no id" in
       the Meshtastic protocol, so it can never be a mark; fall back to the newest one that can.
     *
     * Reactions are skipped, and that is not a nicety - it is the same rule
     * mesh_ui_nav_conversation_summarise() counts by, and the two have to agree or the badge
     * cannot clear. The count walks the log ignoring reactions and looks for the marked packet
     * to know where "read" stops; a mark left on a reaction is a packet that walk never meets,
     * so `mark_seen` stays false and every inbound message in view goes on being counted as
     * unread. A conversation whose newest entry was a tapback stayed badged however many times
     * it was opened.
     *
     * It is also what lets the transcript find its own "new from here" line, which looks for
     * the bubble whose predecessor is the marked one - and a reaction never gets a bubble. */
    uint32_t newest = 0U;
    const uint32_t count =
        store->messages.count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : store->messages.count;
    for (uint32_t i = 0; i < count; ++i) {
        const struct mesh_ui_message *message = &store->messages.entries[i];
        if (message->is_reaction) {
            continue;
        }
        const bool belongs =
            is_channel ? (message->broadcast && message->channel == store->nav.target_channel)
                       : (!message->broadcast && message->peer == store->nav.target_node);
        if (belongs && message->packet_id != 0U) {
            newest = message->packet_id;
        }
    }
    if (newest == 0U) {
        return false; /* nothing here to have read */
    }

    struct mesh_ui_read_mark *mark = mesh_ui_store_read_mark_slot(
        &store->read_state, kind, store->nav.target_node, store->nav.target_channel);
    /* The ordering moves whatever comes of it: this ran because the reader is in here, and
       "least recently read" has to count a conversation they came back to with nothing new in
       it. Claiming the slot is also what can evict another mark, and that is a change to the
       file - but it cannot happen without the position below moving too, because a slot this
       call just claimed holds packet id 0 and `newest` is never 0. */
    store->read_state.stamp++;
    mark->stamp = store->read_state.stamp;
    if (mark->packet_id == newest) {
        /*
         * The mark is already where it belongs, which is the ordinary case rather than the
         * corner: this runs from consume_updates() on every update while a thread is open - a
         * node reporting, a position arriving, a press - and the read position moves only when
         * a message does. Moving the revision here dirtied the cache each time and rewrote the
         * whole snapshot a batching window later, for as long as the thread stayed open.
         */
        return false;
    }
    mark->packet_id = newest;
    store->read_state.revision++;
    return true;
}

void mesh_ui_store_set_page_rows(struct mesh_ui_store *store, uint32_t rows) {
    if (store != NULL) {
        store->page_rows = rows;
    }
}

void mesh_ui_store_set_focus_map(struct mesh_ui_store *store, const struct inkcell_focus_map *map) {
    if (store != NULL) {
        store->focus = map;
    }
}

void mesh_ui_store_request_refresh(struct mesh_ui_store *store) {
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_DISCOVERY | MESH_UI_UPDATE_HANDSHAKE |
                                        MESH_UI_UPDATE_TRANSPORT | MESH_UI_UPDATE_MESSAGES |
                                        MESH_UI_UPDATE_NAV);
}

bool mesh_ui_store_consume_updates(struct mesh_ui_store *store, struct mesh_ui_snapshot *snapshot) {
    if (store == NULL || snapshot == NULL) {
        return false;
    }

    if (store->event_wake.fd >= 0) {
        const int drained = inkwell_wake_drain(&store->event_wake);
        if (drained < 0) {
            inkwell_log_warn("ui", "store wake read failed: %s", strerror(-drained));
        }
    }

    if (store->pending_flags == MESH_UI_UPDATE_NONE) {
        return false;
    }

    /* Whatever is on screen has been seen. Doing this here covers both halves of the rule: a
       key that opens a thread marks NAV dirty, and a message arriving into the open thread
       marks MESSAGES dirty, so either way we get here before the frame is built. */
    mesh_ui_store_mark_open_conversation_read(store);

    snapshot->update_flags = store->pending_flags;
    snapshot->device_count = store->device_count;
    inkwell_str_copy(snapshot->network_host, sizeof snapshot->network_host, store->network_host);
    if (store->device_count > 0U) {
        memcpy(snapshot->devices, store->devices,
               store->device_count * sizeof(struct mesh_ui_device));
    }
    if (store->device_count < MESH_UI_MAX_DEVICES) {
        memset(&snapshot->devices[store->device_count], 0,
               (MESH_UI_MAX_DEVICES - store->device_count) * sizeof(struct mesh_ui_device));
    }

    snapshot->handshake_valid = store->handshake_valid;
    if (store->handshake_valid) {
        snapshot->handshake = store->handshake;
    } else {
        memset(&snapshot->handshake, 0, sizeof snapshot->handshake);
    }

    snapshot->messages = store->messages;
    snapshot->thread = store->thread;
    snapshot->waypoints = store->waypoints;
    snapshot->read_state = store->read_state;
    snapshot->settings = store->settings;
    snapshot->traceroute = store->traceroute;
    snapshot->traceroutes = store->traceroutes;
    snapshot->verification = store->verification;
    snapshot->history = store->history;
    /* What the backend last said its body holds, for the one thing on the other side of the seam
       that needs it: the action bar deciding whether a list of readings can be scrolled. */
    snapshot->page_rows = store->page_rows;
    snapshot->mqtt = store->mqtt;

    memcpy(snapshot->transport_status, store->transport_status, sizeof snapshot->transport_status);

    /* Data changes (a node dropping out, a message arriving) move or invalidate cursors; fix
       them up here so every backend draws a cursor that points at a real row. */
    if (mesh_ui_nav_clamp(&store->nav, store)) {
        store->pending_flags |= MESH_UI_UPDATE_NAV;
        snapshot->update_flags = store->pending_flags;
    }
    snapshot->nav = store->nav;

    store->pending_flags = MESH_UI_UPDATE_NONE;
    return true;
}

const struct mesh_ui_device *
mesh_ui_snapshot_connected_device(const struct mesh_ui_snapshot *snapshot) {
    if (snapshot == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < snapshot->device_count; ++i) {
        if (snapshot->devices[i].connected) {
            return &snapshot->devices[i];
        }
    }
    return NULL;
}

bool mesh_ui_device_connectable(const struct mesh_ui_device *device) {
    return device != NULL && device->identifier[0] != '\0' && !device->connected &&
           !device->bootloader;
}

bool mesh_ui_device_forgettable(const struct mesh_ui_device *device) {
    return device != NULL && device->kind == (uint8_t)MESH_UI_DEVICE_BLE;
}
