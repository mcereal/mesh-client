#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/store.h"

#include "mesh/geo/coords.h"
#include "mesh/utils/log.h"

#include "mesh/core/message.h"
#include "mesh/ui/settings.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static void mesh_ui_store_mark_dirty(struct mesh_ui_store *store, mesh_ui_update_flags flags) {
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

        if (node->metrics.valid && node->metrics.has_battery &&
            !(was != NULL && memcmp(&was->metrics, &node->metrics, sizeof node->metrics) == 0)) {
            mesh_ui_history_note_battery(&store->history, (uint32_t)store->now_ms, node->node_id,
                                         node->metrics.battery_level);
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
        state->stamp++; /* the app persists the read state when this moves */
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
       conversation as much as reading it is. Bumping it also moves the read state, which is
       what tells the app there is something new to persist. */
    store->read_state.stamp++;
    mark->stamp = store->read_state.stamp;

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
    store->read_state.stamp++;
    mark->stamp = store->read_state.stamp;
    if (mark->packet_id == newest) {
        return false;
    }
    mark->packet_id = newest;
    return true;
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

static void mesh_ui_store_escape_and_write(FILE *file, const char *key, const char *value) {
    fprintf(file, "%s=", key);
    if (value != NULL) {
        for (const unsigned char *ptr = (const unsigned char *)value; *ptr != '\0'; ++ptr) {
            if (*ptr < 0x20U || *ptr == '\\' || *ptr == '=') {
                fprintf(file, "\\x%02x", *ptr);
            } else {
                fputc(*ptr, file);
            }
        }
    }
    fputc('\n', file);
}

static int mesh_ui_store_save_handshake(FILE *file,
                                        const struct mesh_ui_handshake_state *handshake) {
    if (file == NULL || handshake == NULL) {
        return 0;
    }

    fprintf(file, "handshake_request=%u,%u\n", handshake->request_in_flight ? 1U : 0U,
            handshake->request_id);
    fprintf(file, "handshake_config=%u,%u,%u\n", handshake->config_complete ? 1U : 0U,
            handshake->config_complete_id, handshake->has_config ? 1U : 0U);
    fprintf(file, "handshake_mynode=%u,%u,%u,%u\n", handshake->has_my_info ? 1U : 0U,
            handshake->my_info.node_num, handshake->my_info.nodedb_entries,
            handshake->my_info.reboot_count);
    fprintf(file, "handshake_roster=%u\n", handshake->roster_owner);
    mesh_ui_store_escape_and_write(file, "handshake_channel", handshake->primary_channel);
    mesh_ui_store_escape_and_write(file, "handshake_my_short", handshake->my_short_name);
    fprintf(file, "handshake_cached=%u\n", handshake->cached ? 1U : 0U);
    fprintf(file, "handshake_channels=%u\n", handshake->channel_count);
    for (uint32_t i = 0; i < handshake->channel_count && i < MESH_UI_MAX_CHANNELS; ++i) {
        const struct mesh_ui_channel *channel = &handshake->channels[i];
        fprintf(file, "channel[%u]=%u,%u\n", i, (unsigned)channel->index, (unsigned)channel->role);
        char key[32];
        snprintf(key, sizeof key, "channel_name[%u]", i);
        mesh_ui_store_escape_and_write(file, key, channel->name);
    }
    fprintf(file, "handshake_nodes=%u\n", handshake->node_count);
    for (uint32_t i = 0; i < handshake->node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
        const struct mesh_ui_node_summary *node = &handshake->nodes[i];
        fprintf(file, "node[%u]=%u,%u,%u,%f,%u,%u\n", i, node->node_id, node->last_heard,
                node->has_hops_away ? 1U : 0U, (double)node->snr, node->via_mqtt ? 1U : 0U,
                node->hops_away);
        char key_long[32];
        char key_short[32];
        snprintf(key_long, sizeof key_long, "node_long[%u]", i);
        snprintf(key_short, sizeof key_short, "node_short[%u]", i);
        mesh_ui_store_escape_and_write(file, key_long, node->long_name);
        mesh_ui_store_escape_and_write(file, key_short, node->short_name);

        /* The detail the Nodes tab drills into. Written as separate keys rather than widened
           onto node[] so an older build reading a newer cache skips what it does not know and
           a newer build reading an older one simply finds nothing to fill in. */
        char key[40];
        snprintf(key, sizeof key, "node_user[%u]", i);
        mesh_ui_store_escape_and_write(file, key, node->user_id);
        fprintf(file, "node_ident[%u]=%u,%u,%u,%u,%u,%u,%u\n", i, node->hw_model, node->role,
                node->is_licensed ? 1U : 0U, node->is_unmessagable ? 1U : 0U,
                node->is_favorite ? 1U : 0U, node->is_ignored ? 1U : 0U, (unsigned)node->channel);
        /* What the roster knows that the radio did not tell us this run: whether the name is
           real and whether the radio still carried the node. Both are why a restored roster is
           worth more than a re-sync. */
        fprintf(file, "node_state[%u]=%u,%u\n", i, node->has_user ? 1U : 0U,
                node->in_nodedb ? 1U : 0U);
        if (node->public_key_len > 0U) {
            char pubkey[2U * sizeof node->public_key + 1U];
            mesh_ui_settings_key_hex(node->public_key, node->public_key_len, pubkey, sizeof pubkey);
            snprintf(key, sizeof key, "node_key[%u]", i);
            mesh_ui_store_escape_and_write(file, key, pubkey);
        }
        /* Its own key rather than a widened node[] line: the loader matches node[] on an exact
           field count, so a build that predates this one would drop the whole node rather than
           the one value it does not know. */
        if (node->has_rssi) {
            fprintf(file, "node_rssi[%u]=%d\n", i, (int)node->rx_rssi);
        }
        if (node->position.valid) {
            /* `received` is appended last so a cache written by an older build still loads:
               the reader takes seven fields or eight, and a line with seven leaves it 0. */
            fprintf(file, "node_pos[%u]=%d,%d,%u,%d,%u,%u,%u,%u\n", i, node->position.latitude_i,
                    node->position.longitude_i, node->position.has_altitude ? 1U : 0U,
                    node->position.altitude, node->position.time,
                    (unsigned)node->position.sats_in_view, (unsigned)node->position.precision_bits,
                    node->position.received);
        }
        if (node->metrics.valid) {
            fprintf(file, "node_metrics[%u]=%u,%u,%u,%u,%f,%u,%f,%u,%f,%u,%u\n", i,
                    node->metrics.time, node->metrics.has_battery ? 1U : 0U,
                    (unsigned)node->metrics.battery_level, node->metrics.has_voltage ? 1U : 0U,
                    (double)node->metrics.voltage, node->metrics.has_channel_utilization ? 1U : 0U,
                    (double)node->metrics.channel_utilization,
                    node->metrics.has_air_util_tx ? 1U : 0U, (double)node->metrics.air_util_tx,
                    node->metrics.has_uptime ? 1U : 0U, node->metrics.uptime_seconds);
        }
        if (node->environment.valid) {
            fprintf(file, "node_env[%u]=%u,%u,%f,%u,%f,%u,%f,%u,%u,%u,%f,%u,%f,%u,%f\n", i,
                    node->environment.time, node->environment.has_temperature ? 1U : 0U,
                    (double)node->environment.temperature, node->environment.has_humidity ? 1U : 0U,
                    (double)node->environment.relative_humidity,
                    node->environment.has_pressure ? 1U : 0U,
                    (double)node->environment.barometric_pressure,
                    node->environment.has_iaq ? 1U : 0U, (unsigned)node->environment.iaq,
                    node->environment.has_lux ? 1U : 0U, (double)node->environment.lux,
                    node->environment.has_voltage ? 1U : 0U, (double)node->environment.voltage,
                    node->environment.has_current ? 1U : 0U, (double)node->environment.current);
        }
        /* The four groups beyond device metrics and environment. Each gets its own key for the
           reason the three above do: a cache written by a build that had them is read by one
           that does not simply by skipping a line it does not recognise. */
        /* One line per neighbour rather than one line for the list: a cache line is parsed
           with a fixed-field sscanf, and a variable-length list in one would have to be
           re-parsed by hand for a count that upstream can change. */
        if (node->neighbors.valid) {
            fprintf(file, "node_nbrs[%u]=%u,%u,%u\n", i, node->neighbors.time,
                    node->neighbors.broadcast_interval_secs, (unsigned)node->neighbors.count);
            for (uint8_t n = 0; n < node->neighbors.count && n < MESH_UI_MAX_NEIGHBORS; ++n) {
                fprintf(file, "node_nbr[%u.%u]=%u,%f\n", i, (unsigned)n,
                        node->neighbors.entries[n].node_id, (double)node->neighbors.entries[n].snr);
            }
        }
        if (node->power.valid) {
            fprintf(file, "node_power[%u]=%u,%u,%f,%u,%f,%u,%f,%u,%f,%u,%f,%u,%f\n", i,
                    node->power.time, node->power.channel[0].has_voltage ? 1U : 0U,
                    (double)node->power.channel[0].voltage,
                    node->power.channel[0].has_current ? 1U : 0U,
                    (double)node->power.channel[0].current,
                    node->power.channel[1].has_voltage ? 1U : 0U,
                    (double)node->power.channel[1].voltage,
                    node->power.channel[1].has_current ? 1U : 0U,
                    (double)node->power.channel[1].current,
                    node->power.channel[2].has_voltage ? 1U : 0U,
                    (double)node->power.channel[2].voltage,
                    node->power.channel[2].has_current ? 1U : 0U,
                    (double)node->power.channel[2].current);
        }
        if (node->air_quality.valid) {
            fprintf(file, "node_air[%u]=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%f,%u,%f\n", i,
                    node->air_quality.time, node->air_quality.has_pm10 ? 1U : 0U,
                    (unsigned)node->air_quality.pm10_standard, node->air_quality.has_pm25 ? 1U : 0U,
                    (unsigned)node->air_quality.pm25_standard,
                    node->air_quality.has_pm100 ? 1U : 0U,
                    (unsigned)node->air_quality.pm100_standard, node->air_quality.has_co2 ? 1U : 0U,
                    (unsigned)node->air_quality.co2, node->air_quality.has_voc_index ? 1U : 0U,
                    (double)node->air_quality.voc_index, node->air_quality.has_nox_index ? 1U : 0U,
                    (double)node->air_quality.nox_index);
        }
        if (node->health.valid) {
            fprintf(file, "node_health[%u]=%u,%u,%u,%u,%u,%u,%f\n", i, node->health.time,
                    node->health.has_heart_bpm ? 1U : 0U, (unsigned)node->health.heart_bpm,
                    node->health.has_spo2 ? 1U : 0U, (unsigned)node->health.spo2,
                    node->health.has_temperature ? 1U : 0U, (double)node->health.temperature);
        }
        if (node->host.valid) {
            fprintf(file, "node_host[%u]=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", i, node->host.time,
                    node->host.has_uptime ? 1U : 0U, node->host.uptime_seconds,
                    node->host.has_freemem ? 1U : 0U, node->host.freemem_kib,
                    node->host.has_diskfree ? 1U : 0U, node->host.diskfree_mib,
                    node->host.has_load ? 1U : 0U, node->host.load1, node->host.load5,
                    node->host.load15);
        }
    }

    return 0;
}

static void mesh_ui_store_unescape_value(char *value) {
    if (value == NULL) {
        return;
    }

    char *write_ptr = value;
    for (char *read_ptr = value; *read_ptr != '\0'; ++read_ptr) {
        if (*read_ptr == '\\') {
            if (read_ptr[1] == 'x' && read_ptr[2] != '\0' && read_ptr[3] != '\0') {
                char hex[3] = {read_ptr[2], read_ptr[3], '\0'};
                *write_ptr++ = (char)strtol(hex, NULL, 16);
                read_ptr += 3;
            }
        } else {
            *write_ptr++ = *read_ptr;
        }
    }
    *write_ptr = '\0';
}

static void mesh_ui_store_save_messages(FILE *file, const struct mesh_ui_message_list *messages) {
    if (file == NULL || messages == NULL) {
        return;
    }

    fprintf(file, "messages=%u,%u\n", messages->count, messages->dropped);
    for (uint32_t i = 0; i < messages->count && i < MESH_UI_MAX_MESSAGES; ++i) {
        const struct mesh_ui_message *message = &messages->entries[i];
        fprintf(file, "msg[%u]=%u,%u,%u,%u,%u,%u,%u\n", i, message->packet_id, message->peer,
                message->rx_time, (unsigned)message->channel, (unsigned)message->direction,
                (unsigned)message->ack, message->broadcast ? 1U : 0U);

        /* What the message *is*, as opposed to where it came from. On its own key rather than
           widened onto msg[] for the reason the node detail's groups are: the loader matches
           msg[] on an exact field count, so a build that predates this would drop the whole
           message rather than the part it does not know.

           Losing this line is not cosmetic. A reaction reloaded without `is_reaction` is a
           bubble containing a bare emoji that also bumps the unread count - which is precisely
           the behaviour reading Data.emoji was meant to end, returning at every restart. */
        char key_meta[32];
        snprintf(key_meta, sizeof key_meta, "msg_meta[%u]", i);
        fprintf(file, "%s=%u,%u,%u,%u\n", key_meta, (unsigned)message->kind,
                message->pki_encrypted ? 1U : 0U, message->reply_id,
                message->is_reaction ? 1U : 0U);

        char key_name[32];
        char key_text[32];
        snprintf(key_name, sizeof key_name, "msg_name[%u]", i);
        snprintf(key_text, sizeof key_text, "msg_text[%u]", i);
        mesh_ui_store_escape_and_write(file, key_name, message->peer_name);
        mesh_ui_store_escape_and_write(file, key_text, message->text);
    }
}

/* One line per conversation the client remembers anything about. The stamp is not written: load
   order stands in for it, which is all the eviction ordering needs.

   The mute is a fifth field rather than a line of its own, and it is appended rather than
   inserted, so a file written before it existed still parses - see the loader, which takes four
   fields or five. */
static void mesh_ui_store_save_read_state(FILE *file, const struct mesh_ui_read_state *state) {
    if (file == NULL || state == NULL) {
        return;
    }
    fprintf(file, "read_marks=%u\n", state->count);
    for (uint32_t i = 0; i < state->count && i < MESH_UI_READ_MARKS_MAX; ++i) {
        const struct mesh_ui_read_mark *mark = &state->marks[i];
        fprintf(file, "read[%u]=%u,%u,%u,%u,%u\n", i, (unsigned)mark->kind, (unsigned)mark->channel,
                mark->node, mark->packet_id, mark->muted ? 1U : 0U);
    }
}

int mesh_ui_store_save(const struct mesh_ui_store *store, const char *path) {
    if (store == NULL || path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return -errno;
    }

    fprintf(file, "handshake_valid=%u\n", store->handshake_valid ? 1U : 0U);
    if (store->handshake_valid) {
        mesh_ui_store_save_handshake(file, &store->handshake);
    }
    mesh_ui_store_save_messages(file, &store->messages);
    mesh_ui_store_save_read_state(file, &store->read_state);

    int result = ferror(file) ? -EIO : 0;
    if (fclose(file) != 0) {
        result = -errno;
    }
    return result;
}

int mesh_ui_store_load(struct mesh_ui_store *store, const char *path) {
    if (store == NULL || path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -errno;
    }

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof(handshake));
    bool handshake_valid = false;
    uint32_t nodes_expected = 0U;
    bool nodes_expected_set = false;
    uint32_t nodes_loaded = 0U;

    struct mesh_ui_message_list messages;
    memset(&messages, 0, sizeof(messages));
    uint32_t messages_expected = 0U;
    bool messages_expected_set = false;
    uint32_t messages_loaded = 0U;

    struct mesh_ui_read_state read_state;
    memset(&read_state, 0, sizeof(read_state));

    char line[1280];
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        char *equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }

        *equals = '\0';
        char *key = line;
        char *value = equals + 1;
        mesh_ui_store_unescape_value(value);

        if (strcmp(key, "handshake_valid") == 0) {
            handshake_valid = (strtoul(value, NULL, 10) != 0U);
        } else if (strcmp(key, "handshake_request") == 0) {
            unsigned int inflight = 0U;
            unsigned int request_id = 0U;
            if (sscanf(value, "%u,%u", &inflight, &request_id) == 2) {
                handshake.request_in_flight = (inflight != 0U);
                handshake.request_id = request_id;
            }
        } else if (strcmp(key, "handshake_config") == 0) {
            unsigned int complete = 0U;
            unsigned int config_id = 0U;
            unsigned int has_config = 0U;
            if (sscanf(value, "%u,%u,%u", &complete, &config_id, &has_config) == 3) {
                handshake.config_complete = (complete != 0U);
                handshake.config_complete_id = config_id;
                handshake.has_config = (has_config != 0U);
            }
        } else if (strcmp(key, "handshake_mynode") == 0) {
            unsigned int has_my_info = 0U;
            unsigned int node_num = 0U;
            unsigned int nodedb = 0U;
            unsigned int reboot_count = 0U;
            if (sscanf(value, "%u,%u,%u,%u", &has_my_info, &node_num, &nodedb, &reboot_count) ==
                4) {
                handshake.has_my_info = (has_my_info != 0U);
                handshake.my_info.node_num = node_num;
                handshake.my_info.nodedb_entries = nodedb;
                handshake.my_info.reboot_count = reboot_count;
            }
        } else if (strcmp(key, "handshake_channel") == 0) {
            snprintf(handshake.primary_channel, sizeof(handshake.primary_channel), "%s", value);
        } else if (strcmp(key, "handshake_my_short") == 0) {
            snprintf(handshake.my_short_name, sizeof(handshake.my_short_name), "%s", value);
        } else if (strcmp(key, "handshake_nodes") == 0) {
            nodes_expected = (uint32_t)strtoul(value, NULL, 10);
            nodes_expected_set = true;
        } else if (strcmp(key, "handshake_roster") == 0) {
            handshake.roster_owner = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "handshake_cached") == 0) {
            handshake.cached = (strtoul(value, NULL, 10) != 0U);
        } else if (strcmp(key, "handshake_channels") == 0) {
            uint32_t count = (uint32_t)strtoul(value, NULL, 10);
            handshake.channel_count = count > MESH_UI_MAX_CHANNELS ? MESH_UI_MAX_CHANNELS : count;
        } else if (strncmp(key, "channel[", 8) == 0) {
            unsigned int index = 0U;
            unsigned int slot = 0U;
            unsigned int role = 0U;
            if (sscanf(key, "channel[%u]", &index) == 1 && index < MESH_UI_MAX_CHANNELS &&
                sscanf(value, "%u,%u", &slot, &role) == 2) {
                handshake.channels[index].index = (uint8_t)slot;
                handshake.channels[index].role = (uint8_t)role;
            }
        } else if (strncmp(key, "channel_name[", 13) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "channel_name[%u]", &index) == 1 && index < MESH_UI_MAX_CHANNELS) {
                snprintf(handshake.channels[index].name, sizeof(handshake.channels[index].name),
                         "%s", value);
            }
        } else if (strncmp(key, "node[", 5) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "node[%u]", &index) == 1) {
                uint32_t node_id = 0U;
                uint32_t last_heard = 0U;
                unsigned int has_hops = 0U;
                double snr = 0.0;
                unsigned int via_mqtt = 0U;
                unsigned int hops = 0U;
                if (sscanf(value, "%u,%u,%u,%lf,%u,%u", &node_id, &last_heard, &has_hops, &snr,
                           &via_mqtt, &hops) == 6) {
                    if (index < MESH_UI_MAX_HANDSHAKE_NODES) {
                        struct mesh_ui_node_summary *node = &handshake.nodes[index];
                        node->node_id = node_id;
                        node->last_heard = last_heard;
                        node->has_hops_away = (has_hops != 0U);
                        node->snr = (float)snr;
                        node->via_mqtt = (via_mqtt != 0U);
                        node->hops_away = (uint8_t)hops;
                        if ((uint32_t)(index + 1U) > nodes_loaded) {
                            nodes_loaded = index + 1U;
                        }
                    }
                }
            }
        } else if (strncmp(key, "node_long[", 10) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "node_long[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES) {
                snprintf(handshake.nodes[index].long_name, sizeof(handshake.nodes[index].long_name),
                         "%s", value);
            }
        } else if (strncmp(key, "node_short[", 11) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "node_short[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES) {
                snprintf(handshake.nodes[index].short_name,
                         sizeof(handshake.nodes[index].short_name), "%s", value);
            }
        } else if (strncmp(key, "node_user[", 10) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "node_user[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES) {
                snprintf(handshake.nodes[index].user_id, sizeof(handshake.nodes[index].user_id),
                         "%s", value);
            }
        } else if (strncmp(key, "node_ident[", 11) == 0) {
            unsigned int index = 0U;
            unsigned int hw = 0U;
            unsigned int role = 0U;
            unsigned int licensed = 0U;
            unsigned int unmessagable = 0U;
            unsigned int favorite = 0U;
            unsigned int ignored = 0U;
            unsigned int channel = 0U;
            if (sscanf(key, "node_ident[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES &&
                sscanf(value, "%u,%u,%u,%u,%u,%u,%u", &hw, &role, &licensed, &unmessagable,
                       &favorite, &ignored, &channel) == 7) {
                struct mesh_ui_node_summary *node = &handshake.nodes[index];
                node->hw_model = hw;
                node->role = role;
                node->is_licensed = (licensed != 0U);
                node->is_unmessagable = (unmessagable != 0U);
                node->is_favorite = (favorite != 0U);
                node->is_ignored = (ignored != 0U);
                node->channel = (uint8_t)channel;
            }
        } else if (strncmp(key, "node_state[", 11) == 0) {
            unsigned int index = 0U;
            unsigned int has_user = 0U;
            unsigned int in_nodedb = 0U;
            if (sscanf(key, "node_state[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES &&
                sscanf(value, "%u,%u", &has_user, &in_nodedb) == 2) {
                handshake.nodes[index].has_user = (has_user != 0U);
                handshake.nodes[index].in_nodedb = (in_nodedb != 0U);
            }
        } else if (strncmp(key, "node_key[", 9) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "node_key[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES) {
                struct mesh_ui_node_summary *node = &handshake.nodes[index];
                size_t len = 0U;
                if (mesh_ui_settings_key_parse(value, node->public_key, sizeof node->public_key,
                                               &len)) {
                    node->public_key_len = (uint8_t)len;
                }
            }
        } else if (strncmp(key, "node_nbrs[", 10) == 0) {
            unsigned int index = 0U;
            unsigned int stamp = 0U;
            unsigned int interval = 0U;
            unsigned int count = 0U;
            if (sscanf(key, "node_nbrs[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES &&
                sscanf(value, "%u,%u,%u", &stamp, &interval, &count) == 3) {
                struct mesh_ui_node_neighbors *nbrs = &handshake.nodes[index].neighbors;
                nbrs->valid = true;
                nbrs->time = stamp;
                nbrs->broadcast_interval_secs = interval;
                /* The count is re-derived from the entries that actually load, so a truncated
                   or hand-edited file cannot leave the list claiming rows that are not there. */
                nbrs->count = 0U;
            }
        } else if (strncmp(key, "node_nbr[", 9) == 0) {
            unsigned int index = 0U;
            unsigned int slot = 0U;
            unsigned int node_id = 0U;
            double snr = 0.0;
            if (sscanf(key, "node_nbr[%u.%u]", &index, &slot) == 2 &&
                index < MESH_UI_MAX_HANDSHAKE_NODES && slot < MESH_UI_MAX_NEIGHBORS &&
                sscanf(value, "%u,%lf", &node_id, &snr) == 2 && node_id != 0U) {
                struct mesh_ui_node_neighbors *nbrs = &handshake.nodes[index].neighbors;
                /* Only ever appended, and only for a list the node_nbrs line already opened:
                   a stray entry for a node with no header is not half a neighbour list. */
                if (nbrs->valid && nbrs->count < MESH_UI_MAX_NEIGHBORS) {
                    nbrs->entries[nbrs->count].node_id = node_id;
                    nbrs->entries[nbrs->count].snr = (float)snr;
                    nbrs->count++;
                }
            }
        } else if (strncmp(key, "node_rssi[", 10) == 0) {
            unsigned int index = 0U;
            int rssi = 0;
            unsigned int stamp = 0U;
            /* The stamp joined this line after the reading did, so `>= 1` rather than `== 2`:
               a cache written without it still loads, and an unstamped reading reads as
               current - which is exactly what it was before the stamp existed. */
            if (sscanf(key, "node_rssi[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES &&
                sscanf(value, "%d,%u", &rssi, &stamp) >= 1) {
                handshake.nodes[index].has_rssi = true;
                handshake.nodes[index].rx_rssi = (int16_t)rssi;
                handshake.nodes[index].rssi_time = stamp;
            }
        } else if (strncmp(key, "node_pos[", 9) == 0) {
            unsigned int index = 0U;
            /*
             * The coordinates are read wide, and that is not a style choice. `%d` on text that
             * overflows an `int` is undefined, and glibc's answer for "4294967296" is 0 - so
             * scanned narrowly, an absurd coordinate would arrive at the range check already
             * wearing a valid one's clothes and be stored as a fix in the Gulf of Guinea. The
             * conversion has to be provably lossless before the point is asked whether it is a
             * place, which is two questions and therefore two guards.
             */
            long long latitude = 0;
            long long longitude = 0;
            unsigned int has_altitude = 0U;
            int altitude = 0;
            unsigned int stamp = 0U;
            unsigned int sats = 0U;
            unsigned int precision = 0U;
            unsigned int received = 0U;
            /* Seven fields or eight: a cache written before `received` existed still loads,
               and its fixes simply have no arrival time to fall back on. */
            if (sscanf(key, "node_pos[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES &&
                sscanf(value, "%lld,%lld,%u,%d,%u,%u,%u,%u", &latitude, &longitude, &has_altitude,
                       &altitude, &stamp, &sats, &precision, &received) >= 7 &&
                latitude >= INT32_MIN && latitude <= INT32_MAX && longitude >= INT32_MIN &&
                longitude <= INT32_MAX &&
                /* The cache is a text file on a card the user can edit, so it is an ingress
                   like the air is, and it is held to the same test. A line naming an
                   impossible point leaves the node with no fix rather than an absurd one. */
                mesh_geo_coords_valid((int32_t)latitude, (int32_t)longitude)) {
                struct mesh_ui_node_position *position = &handshake.nodes[index].position;
                position->valid = true;
                position->latitude_i = (int32_t)latitude;
                position->longitude_i = (int32_t)longitude;
                position->has_altitude = (has_altitude != 0U);
                position->altitude = (int32_t)altitude;
                position->time = stamp;
                position->received = received;
                position->sats_in_view = (uint8_t)sats;
                position->precision_bits = (uint8_t)precision;
            }
        } else if (strncmp(key, "node_metrics[", 13) == 0) {
            unsigned int index = 0U;
            unsigned int stamp = 0U;
            unsigned int has_battery = 0U;
            unsigned int battery = 0U;
            unsigned int has_voltage = 0U;
            double voltage = 0.0;
            unsigned int has_channel = 0U;
            double channel_util = 0.0;
            unsigned int has_air = 0U;
            double air_util = 0.0;
            unsigned int has_uptime = 0U;
            unsigned int uptime = 0U;
            if (sscanf(key, "node_metrics[%u]", &index) == 1 &&
                index < MESH_UI_MAX_HANDSHAKE_NODES &&
                sscanf(value, "%u,%u,%u,%u,%lf,%u,%lf,%u,%lf,%u,%u", &stamp, &has_battery, &battery,
                       &has_voltage, &voltage, &has_channel, &channel_util, &has_air, &air_util,
                       &has_uptime, &uptime) == 11) {
                struct mesh_ui_node_metrics *metrics = &handshake.nodes[index].metrics;
                metrics->valid = true;
                metrics->time = stamp;
                metrics->has_battery = (has_battery != 0U);
                metrics->battery_level = (uint8_t)battery;
                metrics->has_voltage = (has_voltage != 0U);
                metrics->voltage = (float)voltage;
                metrics->has_channel_utilization = (has_channel != 0U);
                metrics->channel_utilization = (float)channel_util;
                metrics->has_air_util_tx = (has_air != 0U);
                metrics->air_util_tx = (float)air_util;
                metrics->has_uptime = (has_uptime != 0U);
                metrics->uptime_seconds = uptime;
            }
        } else if (strncmp(key, "node_env[", 9) == 0) {
            unsigned int index = 0U;
            unsigned int stamp = 0U;
            unsigned int has_temperature = 0U;
            double temperature = 0.0;
            unsigned int has_humidity = 0U;
            double humidity = 0.0;
            unsigned int has_pressure = 0U;
            double pressure = 0.0;
            unsigned int has_iaq = 0U;
            unsigned int iaq = 0U;
            unsigned int has_lux = 0U;
            double lux = 0.0;
            unsigned int has_voltage = 0U;
            double voltage = 0.0;
            unsigned int has_current = 0U;
            double current = 0.0;
            if (sscanf(key, "node_env[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES &&
                sscanf(value, "%u,%u,%lf,%u,%lf,%u,%lf,%u,%u,%u,%lf,%u,%lf,%u,%lf", &stamp,
                       &has_temperature, &temperature, &has_humidity, &humidity, &has_pressure,
                       &pressure, &has_iaq, &iaq, &has_lux, &lux, &has_voltage, &voltage,
                       &has_current, &current) == 15) {
                struct mesh_ui_node_environment *env = &handshake.nodes[index].environment;
                env->valid = true;
                env->time = stamp;
                env->has_temperature = (has_temperature != 0U);
                env->temperature = (float)temperature;
                env->has_humidity = (has_humidity != 0U);
                env->relative_humidity = (float)humidity;
                env->has_pressure = (has_pressure != 0U);
                env->barometric_pressure = (float)pressure;
                env->has_iaq = (has_iaq != 0U);
                env->iaq = (uint16_t)iaq;
                env->has_lux = (has_lux != 0U);
                env->lux = (float)lux;
                env->has_voltage = (has_voltage != 0U);
                env->voltage = (float)voltage;
                env->has_current = (has_current != 0U);
                env->current = (float)current;
            }
        } else if (strncmp(key, "node_power[", 11) == 0) {
            unsigned int index = 0U;
            unsigned int stamp = 0U;
            unsigned int has_v[3] = {0U, 0U, 0U};
            double v[3] = {0.0, 0.0, 0.0};
            unsigned int has_i[3] = {0U, 0U, 0U};
            double amps[3] = {0.0, 0.0, 0.0};
            if (sscanf(key, "node_power[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES &&
                sscanf(value, "%u,%u,%lf,%u,%lf,%u,%lf,%u,%lf,%u,%lf,%u,%lf", &stamp, &has_v[0],
                       &v[0], &has_i[0], &amps[0], &has_v[1], &v[1], &has_i[1], &amps[1], &has_v[2],
                       &v[2], &has_i[2], &amps[2]) == 13) {
                struct mesh_ui_node_power *power = &handshake.nodes[index].power;
                power->valid = true;
                power->time = stamp;
                for (size_t ch = 0; ch < 3U; ++ch) {
                    power->channel[ch].has_voltage = (has_v[ch] != 0U);
                    power->channel[ch].voltage = (float)v[ch];
                    power->channel[ch].has_current = (has_i[ch] != 0U);
                    power->channel[ch].current = (float)amps[ch];
                }
            }
        } else if (strncmp(key, "node_air[", 9) == 0) {
            unsigned int index = 0U;
            unsigned int stamp = 0U;
            unsigned int has_pm10 = 0U;
            unsigned int pm10 = 0U;
            unsigned int has_pm25 = 0U;
            unsigned int pm25 = 0U;
            unsigned int has_pm100 = 0U;
            unsigned int pm100 = 0U;
            unsigned int has_co2 = 0U;
            unsigned int co2 = 0U;
            unsigned int has_voc = 0U;
            double voc = 0.0;
            unsigned int has_nox = 0U;
            double nox = 0.0;
            if (sscanf(key, "node_air[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES &&
                sscanf(value, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%lf,%u,%lf", &stamp, &has_pm10, &pm10,
                       &has_pm25, &pm25, &has_pm100, &pm100, &has_co2, &co2, &has_voc, &voc,
                       &has_nox, &nox) == 13) {
                struct mesh_ui_node_air_quality *air = &handshake.nodes[index].air_quality;
                air->valid = true;
                air->time = stamp;
                air->has_pm10 = (has_pm10 != 0U);
                air->pm10_standard = (uint16_t)pm10;
                air->has_pm25 = (has_pm25 != 0U);
                air->pm25_standard = (uint16_t)pm25;
                air->has_pm100 = (has_pm100 != 0U);
                air->pm100_standard = (uint16_t)pm100;
                air->has_co2 = (has_co2 != 0U);
                air->co2 = (uint16_t)co2;
                air->has_voc_index = (has_voc != 0U);
                air->voc_index = (float)voc;
                air->has_nox_index = (has_nox != 0U);
                air->nox_index = (float)nox;
            }
        } else if (strncmp(key, "node_health[", 12) == 0) {
            unsigned int index = 0U;
            unsigned int stamp = 0U;
            unsigned int has_bpm = 0U;
            unsigned int bpm = 0U;
            unsigned int has_spo2 = 0U;
            unsigned int spo2 = 0U;
            unsigned int has_temperature = 0U;
            double temperature = 0.0;
            if (sscanf(key, "node_health[%u]", &index) == 1 &&
                index < MESH_UI_MAX_HANDSHAKE_NODES &&
                sscanf(value, "%u,%u,%u,%u,%u,%u,%lf", &stamp, &has_bpm, &bpm, &has_spo2, &spo2,
                       &has_temperature, &temperature) == 7) {
                struct mesh_ui_node_health *health = &handshake.nodes[index].health;
                health->valid = true;
                health->time = stamp;
                health->has_heart_bpm = (has_bpm != 0U);
                health->heart_bpm = (uint8_t)bpm;
                health->has_spo2 = (has_spo2 != 0U);
                health->spo2 = (uint8_t)spo2;
                health->has_temperature = (has_temperature != 0U);
                health->temperature = (float)temperature;
            }
        } else if (strncmp(key, "node_host[", 10) == 0) {
            unsigned int index = 0U;
            unsigned int stamp = 0U;
            unsigned int has_uptime = 0U;
            unsigned int uptime = 0U;
            unsigned int has_freemem = 0U;
            unsigned int freemem = 0U;
            unsigned int has_diskfree = 0U;
            unsigned int diskfree = 0U;
            unsigned int has_load = 0U;
            unsigned int load1 = 0U;
            unsigned int load5 = 0U;
            unsigned int load15 = 0U;
            if (sscanf(key, "node_host[%u]", &index) == 1 && index < MESH_UI_MAX_HANDSHAKE_NODES &&
                sscanf(value, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", &stamp, &has_uptime, &uptime,
                       &has_freemem, &freemem, &has_diskfree, &diskfree, &has_load, &load1, &load5,
                       &load15) == 11) {
                struct mesh_ui_node_host *host = &handshake.nodes[index].host;
                host->valid = true;
                host->time = stamp;
                host->has_uptime = (has_uptime != 0U);
                host->uptime_seconds = uptime;
                host->has_freemem = (has_freemem != 0U);
                host->freemem_kib = freemem;
                host->has_diskfree = (has_diskfree != 0U);
                host->diskfree_mib = diskfree;
                host->has_load = (has_load != 0U);
                host->load1 = load1;
                host->load5 = load5;
                host->load15 = load15;
            }
        } else if (strcmp(key, "messages") == 0) {
            unsigned int count = 0U;
            unsigned int dropped = 0U;
            if (sscanf(value, "%u,%u", &count, &dropped) == 2) {
                messages_expected = count;
                messages_expected_set = true;
                messages.dropped = dropped;
            }
        } else if (strncmp(key, "msg[", 4) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "msg[%u]", &index) == 1 && index < MESH_UI_MAX_MESSAGES) {
                unsigned int packet_id = 0U;
                unsigned int peer = 0U;
                unsigned int rx_time = 0U;
                unsigned int channel = 0U;
                unsigned int direction = 0U;
                unsigned int ack = 0U;
                unsigned int broadcast = 0U;
                if (sscanf(value, "%u,%u,%u,%u,%u,%u,%u", &packet_id, &peer, &rx_time, &channel,
                           &direction, &ack, &broadcast) == 7) {
                    struct mesh_ui_message *message = &messages.entries[index];
                    message->packet_id = packet_id;
                    message->peer = peer;
                    message->rx_time = rx_time;
                    message->channel = (uint8_t)channel;
                    message->direction = (uint8_t)direction;
                    message->ack = (uint8_t)ack;
                    message->broadcast = (broadcast != 0U);
                    if ((uint32_t)(index + 1U) > messages_loaded) {
                        messages_loaded = index + 1U;
                    }
                }
            }
        } else if (strncmp(key, "msg_meta[", 9) == 0) {
            unsigned int index = 0U;
            unsigned int kind = 0U;
            unsigned int pki = 0U;
            unsigned int reply_id = 0U;
            unsigned int is_reaction = 0U;
            if (sscanf(key, "msg_meta[%u]", &index) == 1 && index < MESH_UI_MAX_MESSAGES &&
                sscanf(value, "%u,%u,%u,%u", &kind, &pki, &reply_id, &is_reaction) == 4) {
                struct mesh_ui_message *message = &messages.entries[index];
                message->kind = (uint8_t)kind;
                message->pki_encrypted = (pki != 0U);
                message->reply_id = reply_id;
                message->is_reaction = (is_reaction != 0U);
            }
        } else if (strncmp(key, "msg_name[", 9) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "msg_name[%u]", &index) == 1 && index < MESH_UI_MAX_MESSAGES) {
                snprintf(messages.entries[index].peer_name,
                         sizeof(messages.entries[index].peer_name), "%s", value);
            }
        } else if (strncmp(key, "msg_text[", 9) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "msg_text[%u]", &index) == 1 && index < MESH_UI_MAX_MESSAGES) {
                snprintf(messages.entries[index].text, sizeof(messages.entries[index].text), "%s",
                         value);
            }
        } else if (strncmp(key, "read[", 5) == 0) {
            unsigned int index = 0U;
            if (sscanf(key, "read[%u]", &index) == 1 && index < MESH_UI_READ_MARKS_MAX) {
                unsigned int kind = 0U;
                unsigned int channel = 0U;
                unsigned int node = 0U;
                unsigned int packet_id = 0U;
                unsigned int muted = 0U;
                /* Four fields is a file written before mutes existed, five is one written
                   since; `muted` keeps its 0 either way. And a mark is worth keeping when it
                   carries *either* half - a conversation muted before it was ever read has no
                   packet id to name, and dropping it would unmute it on the next launch. */
                const int fields =
                    sscanf(value, "%u,%u,%u,%u,%u", &kind, &channel, &node, &packet_id, &muted);
                if (fields >= 4 && (packet_id != 0U || muted != 0U)) {
                    struct mesh_ui_read_mark *mark = &read_state.marks[index];
                    mark->kind = (uint8_t)kind;
                    mark->channel = (uint8_t)channel;
                    mark->node = node;
                    mark->packet_id = packet_id;
                    mark->muted = (muted != 0U);
                    mark->stamp = index + 1U;
                    if ((uint32_t)(index + 1U) > read_state.count) {
                        read_state.count = index + 1U;
                    }
                }
            }
        }
    }

    fclose(file);

    uint32_t final_count = nodes_expected_set ? nodes_expected : nodes_loaded;
    if (final_count > MESH_UI_MAX_HANDSHAKE_NODES) {
        final_count = MESH_UI_MAX_HANDSHAKE_NODES;
    }
    handshake.node_count = final_count;
    /* The cache holds only the 128 that were published, so for as long as it is all we have,
       what we know and what we show are the same number. The first publish after the session
       is seeded replaces it with the roster's own total. */
    handshake.nodes_known = final_count;
    /* The two forget counts are deliberately left at zero: they describe what the *session's*
       roster would lose, and the session is seeded from this cache a moment later, so the
       first publish fills them from the roster itself rather than from the 128 rows here. */

    if (handshake_valid) {
        if (!handshake.cached) {
            handshake.cached = true;
        }
        store->handshake = handshake;
        store->handshake_valid = true;
    } else {
        memset(&store->handshake, 0, sizeof store->handshake);
        store->handshake_valid = false;
    }

    uint32_t message_count = messages_expected_set ? messages_expected : messages_loaded;
    if (message_count > MESH_UI_MAX_MESSAGES) {
        message_count = MESH_UI_MAX_MESSAGES;
    }
    messages.count = message_count;
    store->messages = messages;
    read_state.stamp = read_state.count;
    store->read_state = read_state;

    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_HANDSHAKE | MESH_UI_UPDATE_MESSAGES);
    return 0;
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
