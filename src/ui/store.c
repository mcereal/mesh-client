#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/store.h"

#include "store_internal.h"

#include "mesh/utils/log.h"
#include "mesh/utils/text.h"

#include "mesh/core/message.h"
#include "mesh/ui/settings.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

void mesh_ui_store_mark_dirty(struct mesh_ui_store *store, mesh_ui_update_flags flags) {
    if (store == NULL || flags == MESH_UI_UPDATE_NONE) {
        return;
    }

    store->pending_flags |= flags;

    if (store->event_fd >= 0) {
        const uint64_t value = 1U;
        if (write(store->event_fd, &value, sizeof value) < 0) {
            if (errno != EAGAIN) {
                mesh_log_warn("ui", "eventfd write failed: %s", strerror(errno));
            }
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
    store->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (store->event_fd < 0) {
        const int err = -errno;
        mesh_log_error("ui", "eventfd create failed: %s", strerror(errno));
        store->event_fd = -1;
        return err;
    }

    return 0;
}

void mesh_ui_store_shutdown(struct mesh_ui_store *store) {
    if (store == NULL) {
        return;
    }

    if (store->event_fd >= 0) {
        close(store->event_fd);
        store->event_fd = -1;
    }
    store->pending_flags = MESH_UI_UPDATE_NONE;
    store->device_count = 0U;
    store->network_host[0] = '\0';
    store->handshake_valid = false;
    memset(&store->messages, 0, sizeof store->messages);
}

bool mesh_ui_store_handle_key(struct mesh_ui_store *store, enum mesh_ui_key key,
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
    return store->event_fd;
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
    mesh_str_copy(next, sizeof next, host != NULL ? host : "");
    if (strcmp(store->network_host, next) == 0) {
        return;
    }
    mesh_str_copy(store->network_host, sizeof store->network_host, next);
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
                mesh_ui_percent_permille(node->metrics.channel_utilization),
                mesh_ui_percent_permille(node->metrics.air_util_tx));
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
                mesh_ui_temperature_decidegrees(node->environment.temperature),
                node->environment.has_humidity,
                mesh_ui_percent_permille(node->environment.relative_humidity));
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
                                     mesh_ui_percent_permille(next.stats.channel_utilization),
                                     mesh_ui_percent_permille(next.stats.air_util_tx));
    }
    store->settings = next;
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_SETTINGS);
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

uint32_t mesh_ui_store_forget_conversation(struct mesh_ui_store *store, uint8_t kind, uint32_t node,
                                           uint8_t channel) {
    if (store == NULL) {
        return 0U;
    }
    const uint32_t removed = mesh_ui_message_list_forget(&store->messages, kind, node, channel);

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
    mesh_str_copy(view->network_host, sizeof view->network_host, snapshot->network_host);
    view->handshake = snapshot->handshake;
    view->handshake_valid = snapshot->handshake_valid;
    view->messages = snapshot->messages;
    view->waypoints = snapshot->waypoints;
    view->read_state = snapshot->read_state;
    /* The radio's display units, which is what a waypoint's range is stated in. */
    view->settings = snapshot->settings;
    view->event_fd = -1;
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

void mesh_ui_store_request_refresh(struct mesh_ui_store *store) {
    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_DISCOVERY | MESH_UI_UPDATE_HANDSHAKE |
                                        MESH_UI_UPDATE_TRANSPORT | MESH_UI_UPDATE_MESSAGES |
                                        MESH_UI_UPDATE_NAV);
}

bool mesh_ui_store_consume_updates(struct mesh_ui_store *store, struct mesh_ui_snapshot *snapshot) {
    if (store == NULL || snapshot == NULL) {
        return false;
    }

    if (store->event_fd >= 0) {
        uint64_t value = 0;
        ssize_t read_result = read(store->event_fd, &value, sizeof value);
        if (read_result < 0 && errno != EAGAIN) {
            mesh_log_warn("ui", "eventfd read failed: %s", strerror(errno));
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
    mesh_str_copy(snapshot->network_host, sizeof snapshot->network_host, store->network_host);
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
    snapshot->waypoints = store->waypoints;
    snapshot->read_state = store->read_state;
    snapshot->settings = store->settings;
    snapshot->traceroute = store->traceroute;
    snapshot->verification = store->verification;
    snapshot->history = store->history;

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
