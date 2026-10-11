#include "mesh/core/tern.h"

#include "inkwell/base/array.h"
#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"
#include "inkwell/base/wipe.h"
#include "inkwell/codec/sha256.h"

#include "mesh/core/message.h"
#include "mesh/core/session.h"
#include "mesh/proto/ble_profile.h"
#include "mesh/proto/stream_framing.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The conversation, as draft/companion.md lays it out: HELLO, SET_TIME when the clock is worth
 * giving, SYNC, then whatever the user asks, one request at a time with a PING whenever IDLE
 * passes without one. What the node says goes into the session model, so every screen reads it
 * as it reads a Meshtastic or a MeshCore radio.
 *
 * A node is known on the roster by its routing id (mesh_tern_routing_id()), because that is the
 * one number all three of a contact, a message and a neighbour lead to: the first two carry an
 * address, which hashes to it, and the third carries it bare.
 *
 * Groups, group messages and invites (version 2) are kept in the conversation's own lists and go
 * no further: the model has no shape for a group that a screen would not take for a channel or a
 * direct message. What they do reach is the sync, which has to know which of their ids may still
 * change. So with a group member's position and with sharing (version 5): kept here. A contact's
 * position and a presence card (version 6) are the node's own, and go on its roster entry too.
 */

/* Meshtastic's role numbers, which the roster's `role` is read in: a relay forwards like a
   CLIENT, a leaf keeps to itself like a CLIENT_MUTE. */
enum {
    TERN_ROLE_AS_CLIENT = 0,
    TERN_ROLE_AS_CLIENT_MUTE = 1,
};

/* --------------------------------------------------------------------------------- requests */

/* The version both ends speak, or this client's own before the node has said its. */
static uint8_t mesh_tern_spoken(const struct mesh_tern *tern) {
    return tern->has_agreed ? tern->agreed : tern->version;
}

static void mesh_tern_pop(struct mesh_tern *tern);
static void mesh_tern_note_queued(struct mesh_tern *tern, uint32_t ticket, uint32_t id);

static void mesh_tern_pump(struct mesh_tern *tern) {
    if (tern->send == NULL || tern->awaiting) {
        return;
    }
    /* A request queued before INFO said the node's version may be one that version lacks. It is
       not sent: the node could not tell this client what it changed. */
    while (tern->queue_count > 0U &&
           mesh_tern_frame_since(tern->queue[tern->queue_head].frame,
                                 tern->queue[tern->queue_head].len) > mesh_tern_spoken(tern)) {
        const struct mesh_tern_request *dropped = &tern->queue[tern->queue_head];
        inkwell_log_warn("tern", "Request 0x%02x not sent: version %u lacks it",
                         (unsigned)dropped->frame[0], (unsigned)mesh_tern_spoken(tern));
        if (dropped->ticket != 0U) {
            mesh_tern_note_queued(tern, dropped->ticket, 0U);
        }
        mesh_tern_pop(tern);
    }
    if (tern->queue_count == 0U) {
        return;
    }
    struct mesh_tern_request *request = &tern->queue[tern->queue_head];
    /* The sequence byte is chosen when the request goes, so an answer to one given up on can
       never be taken for the next's. */
    tern->next_seq = (uint8_t)(tern->next_seq + 1U);
    request->frame[1] = tern->next_seq;
    tern->awaiting = true;
    tern->awaiting_since_ms = tern->now_ms;
    const int result = tern->send(tern->send_ctx, request->frame, request->len, 0U);
    if (result < 0) {
        inkwell_log_warn("tern", "Request 0x%02x not sent (%d)", (unsigned)request->frame[0],
                         result);
        tern->gone = true;
    }
}

static void mesh_tern_pop(struct mesh_tern *tern) {
    if (tern->queue_count == 0U) {
        return;
    }
    /* A JOIN_LINK carried a group's secret: nothing of it outlives its answer. */
    inkwell_wipe(&tern->queue[tern->queue_head], sizeof tern->queue[tern->queue_head]);
    tern->queue_head = (tern->queue_head + 1U) % MESH_TERN_QUEUE_LEN;
    tern->queue_count -= 1U;
    tern->awaiting = false;
}

static uint8_t mesh_tern_head_type(const struct mesh_tern *tern) {
    return tern->queue_count > 0U ? tern->queue[tern->queue_head].frame[0] : 0U;
}

static int mesh_tern_enqueue_ticket(struct mesh_tern *tern, const struct mesh_tern_frame *frame,
                                    uint32_t ticket) {
    if (tern->send == NULL) {
        return -ENOTCONN;
    }
    if (mesh_tern_since(frame->type) > mesh_tern_spoken(tern) ||
        (frame->type == MESH_TERN_SET &&
         mesh_tern_setting_since(frame->setting) > mesh_tern_spoken(tern))) {
        return -EOPNOTSUPP;
    }
    if (tern->queue_count >= MESH_TERN_QUEUE_LEN) {
        return -ENOBUFS;
    }
    struct mesh_tern_request *slot =
        &tern->queue[(tern->queue_head + tern->queue_count) % MESH_TERN_QUEUE_LEN];
    const int len =
        mesh_tern_encode(frame, mesh_tern_spoken(tern), slot->frame, sizeof slot->frame);
    if (len < 0) {
        return len;
    }
    slot->len = (uint8_t)len;
    slot->ticket = ticket;
    tern->queue_count += 1U;
    mesh_tern_pump(tern);
    return 0;
}

static int mesh_tern_enqueue(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    return mesh_tern_enqueue_ticket(tern, frame, 0U);
}

static void mesh_tern_note_queued(struct mesh_tern *tern, uint32_t ticket, uint32_t id) {
    if (tern->queued_count == MESH_TERN_QUEUED_IDS) {
        memmove(tern->queued, tern->queued + 1,
                (MESH_TERN_QUEUED_IDS - 1U) * sizeof tern->queued[0]);
        tern->queued_count -= 1U;
    }
    tern->queued[tern->queued_count++] = (struct mesh_tern_queued){.ticket = ticket, .id = id};
}

static int mesh_tern_request(struct mesh_tern *tern, uint8_t type) {
    const struct mesh_tern_frame frame = {.type = type};
    return mesh_tern_enqueue(tern, &frame);
}

static bool mesh_tern_queued(const struct mesh_tern *tern, uint8_t type) {
    for (size_t i = 0; i < tern->queue_count; ++i) {
        if (tern->queue[(tern->queue_head + i) % MESH_TERN_QUEUE_LEN].frame[0] == type) {
            return true;
        }
    }
    return false;
}

/* Every message after `after`, and with it every contact, every neighbour and the rest. */
static void mesh_tern_request_sync(struct mesh_tern *tern, uint32_t after) {
    if (mesh_tern_queued(tern, MESH_TERN_SYNC)) {
        return;
    }
    const struct mesh_tern_frame frame = {.type = MESH_TERN_SYNC, .after = after};
    if (mesh_tern_enqueue(tern, &frame) < 0) {
        inkwell_log_warn("tern", "Sync not queued");
    }
}

/* ------------------------------------------------------------------------------ the roster */

static void mesh_tern_hex_id(const uint8_t *address, char *out, size_t out_len) {
    /* The address's first six bytes: there is no short code for an address yet (the draft's
       "Not yet specified"), and this is what a person can compare against the node's console. */
    snprintf(out, out_len, "%02x%02x%02x%02x%02x%02x", address[0], address[1], address[2],
             address[3], address[4], address[5]);
}

static void mesh_tern_short_name(const char *name, char *out, size_t out_len) {
    size_t used = 0U;
    while (*name == ' ') {
        ++name;
    }
    while (*name != '\0' && *name != ' ') {
        const size_t step =
            inkwell_text_utf8_sequence_len((const uint8_t *)name, strnlen(name, 4U));
        if (step == 0U || used + step + 1U > out_len) {
            break;
        }
        memcpy(out + used, name, step);
        used += step;
        name += step;
    }
    out[used] = '\0';
}

/* The roster entry for the node at `address`, with what the address alone says about it. */
static struct mesh_node_summary *mesh_tern_node_at(struct mesh_tern *tern, const uint8_t *address,
                                                   bool synced) {
    const uint32_t id = mesh_tern_routing_id(address);
    struct mesh_node_summary *node = mesh_session_model_node(tern->model, id, synced);
    if (node == NULL) {
        return NULL;
    }
    memcpy(node->public_key, address, MESH_TERN_ADDRESS_LEN);
    node->public_key_len = (uint8_t)MESH_TERN_ADDRESS_LEN;
    mesh_tern_hex_id(address, node->user_id, sizeof node->user_id);
    return node;
}

static struct mesh_node_summary *mesh_tern_roster_node(const struct mesh_tern *tern,
                                                       uint32_t node_id) {
    const struct mesh_handshake_status *status = &tern->model->handshake;
    for (size_t i = 0; i < status->node_count && i < MESH_SESSION_MAX_NODES; ++i) {
        if (status->nodes[i].node_id == node_id) {
            return (struct mesh_node_summary *)&status->nodes[i];
        }
    }
    return NULL;
}

static size_t mesh_tern_contact_find(const struct mesh_tern *tern, const uint8_t *address) {
    for (size_t i = 0; i < tern->contact_count; ++i) {
        if (memcmp(tern->contacts[i].address, address, MESH_TERN_ADDRESS_LEN) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t mesh_tern_contact_by_node(const struct mesh_tern *tern, uint32_t node_id) {
    for (size_t i = 0; i < tern->contact_count; ++i) {
        if (tern->contacts[i].node_id == node_id) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t mesh_tern_neighbour_find(const struct mesh_tern *tern, uint32_t routing_id) {
    for (size_t i = 0; i < tern->neighbour_count; ++i) {
        if (tern->neighbours[i].routing_id == routing_id) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* Whether the node still holds `node_id` in either list - which is what `in_nodedb` says. */
static bool mesh_tern_holds(const struct mesh_tern *tern, uint32_t node_id) {
    return mesh_tern_contact_by_node(tern, node_id) != SIZE_MAX ||
           mesh_tern_neighbour_find(tern, node_id) != SIZE_MAX;
}

static void mesh_tern_settle_node(struct mesh_tern *tern, uint32_t node_id) {
    struct mesh_node_summary *node = mesh_tern_roster_node(tern, node_id);
    if (node != NULL && node_id != tern->self_node) {
        node->in_nodedb = mesh_tern_holds(tern, node_id);
    }
}

static void mesh_tern_store_contact(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    const bool syncing = tern->phase == MESH_TERN_SYNCING;
    size_t at = mesh_tern_contact_find(tern, frame->address);
    if (at == SIZE_MAX) {
        if (tern->contact_count >= MESH_TERN_MAX_CONTACTS) {
            inkwell_log_warn("tern", "Contact list full; one not kept");
            return;
        }
        at = tern->contact_count++;
    }
    struct mesh_tern_contact *contact = &tern->contacts[at];
    memcpy(contact->address, frame->address, MESH_TERN_ADDRESS_LEN);
    contact->node_id = mesh_tern_routing_id(frame->address);
    contact->session = frame->session;
    contact->synced = contact->synced || syncing;
    inkwell_str_copy(contact->name, sizeof contact->name, frame->text);

    /* A contact is the node's own list, as a Meshtastic replay is: listed, and never news. */
    struct mesh_node_summary *node =
        syncing ? mesh_session_model_listed(tern->model, contact->node_id, 0U)
                : mesh_session_model_node(tern->model, contact->node_id, false);
    if (node == NULL) {
        return;
    }
    memcpy(node->public_key, frame->address, MESH_TERN_ADDRESS_LEN);
    node->public_key_len = (uint8_t)MESH_TERN_ADDRESS_LEN;
    mesh_tern_hex_id(frame->address, node->user_id, sizeof node->user_id);
    /* An empty name is a name, the draft says; on a screen it is the address instead. */
    if (frame->text_len > 0U) {
        inkwell_text_sanitise_str(frame->text, node->long_name, sizeof node->long_name);
        mesh_tern_short_name(node->long_name, node->short_name, sizeof node->short_name);
        node->has_user = true;
    } else {
        node->long_name[0] = '\0';
        node->short_name[0] = '\0';
        node->has_user = false;
    }
    node->in_nodedb = true;
}

static void mesh_tern_drop_contact(struct mesh_tern *tern, const uint8_t *address) {
    const size_t at = mesh_tern_contact_find(tern, address);
    if (at == SIZE_MAX) {
        return;
    }
    const uint32_t node_id = tern->contacts[at].node_id;
    tern->contacts[at] = tern->contacts[--tern->contact_count];
    /* The name was the contact's, and went with it; the node stays on the roster, which
       outlives what the radio holds. */
    struct mesh_node_summary *node = mesh_tern_roster_node(tern, node_id);
    if (node != NULL) {
        node->long_name[0] = '\0';
        node->short_name[0] = '\0';
        node->has_user = false;
    }
    mesh_tern_settle_node(tern, node_id);
}

static void mesh_tern_store_neighbour(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    if (frame->routing_id == 0U || frame->routing_id == 0xFFFFFFFFU) {
        return;
    }
    const bool syncing = tern->phase == MESH_TERN_SYNCING;
    size_t at = mesh_tern_neighbour_find(tern, frame->routing_id);
    if (at == SIZE_MAX) {
        if (tern->neighbour_count >= MESH_TERN_MAX_NEIGHBOURS) {
            return;
        }
        at = tern->neighbour_count++;
    }
    struct mesh_tern_neighbour *neighbour = &tern->neighbours[at];
    neighbour->routing_id = frame->routing_id;
    neighbour->role = frame->role;
    neighbour->synced = neighbour->synced || syncing;

    struct mesh_node_summary *node =
        mesh_session_model_node(tern->model, frame->routing_id, syncing);
    if (node == NULL) {
        return;
    }
    if (node->user_id[0] == '\0') {
        snprintf(node->user_id, sizeof node->user_id, "!%08x", (unsigned)frame->routing_id);
    }
    node->in_nodedb = true;
    node->role =
        frame->role == MESH_TERN_ROLE_RELAY ? TERN_ROLE_AS_CLIENT : TERN_ROLE_AS_CLIENT_MUTE;
    /* A neighbour is one the node hears directly. */
    node->has_hops_away = true;
    node->hops_away = 0U;
    const float snr = (float)frame->snr / 4.0f;
    const uint32_t now = inkwell_time_wall_credible_s();
    if (now != 0U) {
        const uint32_t heard = now > frame->heard ? now - frame->heard : 0U;
        if (heard > node->last_heard) {
            node->last_heard = heard;
        }
        node->snr = snr;
        node->snr_time = heard;
    } else {
        node->snr = snr;
    }
    const struct mesh_session_event event = {.kind = MESH_SESSION_EVENT_NODE_HEARD,
                                             .node = node,
                                             .has_hops = true,
                                             .hops = 0U,
                                             .has_snr = true,
                                             .snr = snr};
    mesh_session_model_note_node(tern->model, &event);
}

static void mesh_tern_drop_neighbour(struct mesh_tern *tern, uint32_t routing_id) {
    const size_t at = mesh_tern_neighbour_find(tern, routing_id);
    if (at == SIZE_MAX) {
        return;
    }
    tern->neighbours[at] = tern->neighbours[--tern->neighbour_count];
    mesh_tern_settle_node(tern, routing_id);
}

static void mesh_tern_forget_books(struct mesh_tern *tern) {
    tern->contact_count = 0U;
    tern->neighbour_count = 0U;
    tern->group_count = 0U;
    tern->group_item_count = 0U;
    tern->open_count = 0U;
    tern->position_count = 0U;
    tern->group_position_count = 0U;
    tern->sharing_count = 0U;
    tern->card_count = 0U;
    tern->newest_id = 0U;
    tern->has_synced_version = false;
    tern->has_missed_since = false;
    tern->has_asked = false;
}

static void mesh_tern_store_self(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    /* Another node on this link than the last: what was learned of that one is not this one's,
       and its message ids are its own count. */
    if (tern->has_self && memcmp(tern->self.address, frame->address, MESH_TERN_ADDRESS_LEN) != 0) {
        mesh_tern_forget_books(tern);
        /* The sync under way asked after the other node's ids: ask this one again, from 0. */
        tern->missed_news = true;
    }
    tern->has_self = true;
    memcpy(tern->self.address, frame->address, MESH_TERN_ADDRESS_LEN);
    tern->self.role = frame->role;
    tern->self.power = frame->power;
    tern->self.time = frame->time;
    inkwell_str_copy(tern->self.region, sizeof tern->self.region, frame->text);
    tern->self.cards = frame->cards;
    inkwell_text_sanitise_str(frame->card_name, tern->self.card_name, sizeof tern->self.card_name);

    tern->self_node = mesh_tern_routing_id(frame->address);
    mesh_session_model_adopt_radio(tern->model, tern->self_node);
    struct mesh_node_summary *node = mesh_tern_node_at(tern, frame->address, true);
    if (node == NULL) {
        return;
    }
    node->in_nodedb = true;
    node->role =
        frame->role == MESH_TERN_ROLE_RELAY ? TERN_ROLE_AS_CLIENT : TERN_ROLE_AS_CLIENT_MUTE;
    const uint32_t now = inkwell_time_wall_credible_s();
    if (now > node->last_heard) {
        node->last_heard = now;
    }
}

static void mesh_tern_store_power(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    struct mesh_node_summary *self =
        tern->has_self ? mesh_tern_roster_node(tern, tern->self_node) : NULL;
    if (self == NULL) {
        return;
    }
    struct mesh_node_metrics *metrics = &self->metrics;
    metrics->valid = true;
    metrics->time = inkwell_time_wall_credible_s();
    metrics->has_voltage = frame->millivolts != 0U;
    metrics->voltage = (float)frame->millivolts / 1000.0f;
    /* 101 is upstream's "plugged in", which the node says outright with bit 1. */
    if ((frame->flags & MESH_TERN_POWER_EXTERNAL) != 0U) {
        metrics->has_battery = true;
        metrics->battery_level = 101U;
    } else {
        metrics->has_battery = frame->percent <= 100U;
        metrics->battery_level = frame->percent <= 100U ? frame->percent : 0U;
    }
}

/*
 * The region's limit as the node spends it. The record is kept whole for a screen that wants
 * the node's own numbers; the model's share of the air is what this radio sent over the
 * period, which is how a Meshtastic radio's air_util_tx reads too. A profile with no limit
 * names no period to take a share of, so that leaves the share as it was.
 */
static void mesh_tern_store_airtime(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    tern->has_airtime = true;
    tern->airtime = (struct mesh_tern_airtime){.period_s = frame->period,
                                               .allowed_ms = frame->allowed,
                                               .used_ms = frame->used,
                                               .wait_ms = frame->wait};
    if (frame->period == 0U) {
        return;
    }
    struct mesh_radio_stats *stats = &tern->model->stats;
    float share = (float)frame->used * 100.0f / ((float)frame->period * 1000.0f);
    if (share > 100.0f) {
        share = 100.0f;
    }
    stats->valid = true;
    stats->time = inkwell_time_wall_credible_s();
    stats->air_util_tx = share;
}

/* ------------------------------------------------------------------------------ messages */

static uint8_t mesh_tern_ack_for(uint8_t state) {
    switch ((enum mesh_tern_state)state) {
    case MESH_TERN_STATE_WAITING:
        return MESH_MESSAGE_ACK_WAITING;
    case MESH_TERN_STATE_SENT:
        return MESH_MESSAGE_ACK_PENDING;
    case MESH_TERN_STATE_DELIVERED:
        return MESH_MESSAGE_ACK_DELIVERED;
    case MESH_TERN_STATE_NOT_DELIVERED:
        return MESH_MESSAGE_ACK_FAILED;
    case MESH_TERN_STATE_RECEIVED:
        break;
    }
    return MESH_MESSAGE_ACK_NONE;
}

/*
 * The radio's message ids, spread over the log's: an odd multiple of its routing id, XORed in.
 * Every node counts its messages from 1, so two nodes on one run - or one run's cache and the
 * next's - would otherwise give their first messages one packet id between them. The XOR keeps
 * each node's ids apart from each other's and lets a packet id be read back to the node's.
 */
static uint32_t mesh_tern_mix(const struct mesh_tern *tern) {
    return tern->self_node * 0x9E3779B1U;
}

/*
 * Whether an item's record may yet change without this client seeing it, should news be missed:
 * a message or an invite waiting or sent may be delivered, and anything received and unread may
 * be read on another client. A group message that is sent stays sent - nothing answers a flood.
 */
static bool mesh_tern_may_change(uint8_t kind, uint8_t state, uint8_t flags) {
    switch ((enum mesh_tern_state)state) {
    case MESH_TERN_STATE_WAITING:
        return true;
    case MESH_TERN_STATE_SENT:
        return kind != MESH_TERN_GROUP_MESSAGE;
    case MESH_TERN_STATE_RECEIVED:
        return (flags & MESH_TERN_MESSAGE_READ) == 0U;
    case MESH_TERN_STATE_DELIVERED:
    case MESH_TERN_STATE_NOT_DELIVERED:
        break;
    }
    return false;
}

static size_t mesh_tern_open_find(const struct mesh_tern *tern, uint32_t id) {
    for (size_t i = 0; i < tern->open_count; ++i) {
        if (tern->open[i].id == id) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* Keeps `open` to the ids whose record may yet change. Full, it keeps the least: a sync after
   missed news asks after one less than the least, and the rest are above it. */
static void mesh_tern_track(struct mesh_tern *tern, uint8_t kind, uint32_t id, uint8_t state,
                            uint8_t flags) {
    const size_t at = mesh_tern_open_find(tern, id);
    if (!mesh_tern_may_change(kind, state, flags)) {
        if (at != SIZE_MAX) {
            tern->open[at] = tern->open[--tern->open_count];
        }
        return;
    }
    const struct mesh_tern_open entry = {.id = id, .kind = kind, .state = state, .flags = flags};
    if (at != SIZE_MAX) {
        tern->open[at] = entry;
    } else if (tern->open_count < MESH_TERN_MAX_OPEN) {
        tern->open[tern->open_count++] = entry;
    } else {
        size_t greatest = 0U;
        for (size_t i = 1; i < tern->open_count; ++i) {
            if (tern->open[i].id > tern->open[greatest].id) {
                greatest = i;
            }
        }
        if (tern->open[greatest].id > id) {
            tern->open[greatest] = entry;
        }
    }
}

/* Every record of the node's count - a message, a group message, an invite - is an id held. */
static void mesh_tern_note_item(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    if (frame->id > tern->newest_id) {
        tern->newest_id = frame->id;
    }
    mesh_tern_track(tern, frame->type, frame->id, frame->state, frame->flags);
}

static bool mesh_tern_logged(const struct mesh_tern *tern, uint32_t message_id) {
    return mesh_message_log_find(&tern->model->messages, mesh_tern_packet_id(tern, message_id)) !=
           NULL;
}

/*
 * One MESSAGE record. Its id is the node's, and the message's packet id in the log is that id
 * scoped to the node (mesh_tern_packet_id()): unique on the node, the same on every client and
 * after every restart, so a record seen again - a sync after a reconnect, a received message
 * marked read - lands on the entry it already has rather than making a second.
 */
static void mesh_tern_store_message(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    if (frame->id == 0U) {
        return;
    }
    mesh_tern_note_item(tern, frame);
    /* SELF comes first in every sync, and names the node the ids are scoped to. */
    if (!tern->has_self || mesh_tern_packet_id(tern, frame->id) == 0U) {
        return;
    }
    const bool inbound = frame->state == MESH_TERN_STATE_RECEIVED;
    if (mesh_tern_logged(tern, frame->id)) {
        if (!inbound) {
            (void)mesh_session_model_mark_ack(
                tern->model, mesh_tern_packet_id(tern, frame->id),
                (enum mesh_message_ack)mesh_tern_ack_for(frame->state), frame->reason);
        }
        return;
    }
    struct mesh_node_summary *peer = mesh_tern_node_at(tern, frame->address, false);
    const uint32_t peer_id = mesh_tern_routing_id(frame->address);

    struct mesh_message message;
    memset(&message, 0, sizeof message);
    message.packet_id = mesh_tern_packet_id(tern, frame->id);
    message.kind = MESH_MESSAGE_KIND_TEXT;
    message.rx_time = frame->time;
    /* Secured unicast is all the radio protocol has: every message is to one node alone. */
    message.pki_encrypted = true;
    /* A sync hands back what the node held while this client was away. The thread wants it;
       a toast about each one would be the answer to "what did I miss" shouted back. */
    message.replayed = tern->phase == MESH_TERN_SYNCING;
    if (inbound) {
        message.direction = MESH_MESSAGE_INBOUND;
        message.from = peer_id;
        message.to = tern->self_node;
        if (peer != NULL && !message.replayed) {
            const uint32_t now = inkwell_time_wall_credible_s();
            if (now > peer->last_heard) {
                peer->last_heard = now;
            }
        }
    } else {
        message.direction = MESH_MESSAGE_OUTBOUND;
        message.from = tern->self_node;
        message.to = peer_id;
        message.ack = mesh_tern_ack_for(frame->state);
        message.ack_error = frame->reason;
    }
    inkwell_text_sanitise((const uint8_t *)frame->text, frame->text_len, message.text,
                          sizeof message.text);
    (void)mesh_session_model_log_message(tern->model, &message);
}

static size_t mesh_tern_group_item_find(const struct mesh_tern *tern, uint32_t id) {
    for (size_t i = 0; i < tern->group_item_count; ++i) {
        if (tern->group_items[i].id == id) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* STATE: where a message, group message or invite is now - those three fields of it, no more. */
static void mesh_tern_store_state(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    if (frame->id == 0U) {
        return;
    }
    const size_t open = mesh_tern_open_find(tern, frame->id);
    const size_t item = mesh_tern_group_item_find(tern, frame->id);
    uint8_t kind = MESH_TERN_MESSAGE;
    uint8_t flags = 0U;
    if (item != SIZE_MAX) {
        struct mesh_tern_group_item *held = &tern->group_items[item];
        held->state = frame->state;
        held->reason = frame->reason;
        kind = held->kind;
        flags = held->flags;
    } else if (open != SIZE_MAX) {
        kind = tern->open[open].kind;
        flags = tern->open[open].flags;
    }
    mesh_tern_track(tern, kind, frame->id, frame->state, flags);
    if (kind != MESH_TERN_MESSAGE || frame->state == MESH_TERN_STATE_RECEIVED || !tern->has_self) {
        return;
    }
    (void)mesh_session_model_mark_ack(tern->model, mesh_tern_packet_id(tern, frame->id),
                                      (enum mesh_message_ack)mesh_tern_ack_for(frame->state),
                                      frame->reason);
}

/* -------------------------------------------------------------------------------- groups */

static size_t mesh_tern_group_find(const struct mesh_tern *tern, const uint8_t *id) {
    for (size_t i = 0; i < tern->group_count; ++i) {
        if (memcmp(tern->groups[i].id, id, MESH_TERN_GROUP_LEN) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static void mesh_tern_store_group(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    size_t at = mesh_tern_group_find(tern, frame->group);
    if (at == SIZE_MAX) {
        if (tern->group_count >= MESH_TERN_MAX_GROUPS) {
            inkwell_log_warn("tern", "Group list full; one not kept");
            return;
        }
        at = tern->group_count++;
    }
    struct mesh_tern_group *group = &tern->groups[at];
    memcpy(group->id, frame->group, MESH_TERN_GROUP_LEN);
    group->synced = group->synced || tern->phase == MESH_TERN_SYNCING;
    inkwell_text_sanitise_str(frame->text, group->name, sizeof group->name);
}

static void mesh_tern_drop_group(struct mesh_tern *tern, const uint8_t *id) {
    const size_t at = mesh_tern_group_find(tern, id);
    if (at != SIZE_MAX) {
        tern->groups[at] = tern->groups[--tern->group_count];
    }
}

/* A GROUP_MESSAGE or INVITE record. Full, the oldest by id makes room: the newest are what a
   screen would show first. */
static void mesh_tern_store_group_item(struct mesh_tern *tern,
                                       const struct mesh_tern_frame *frame) {
    if (frame->id == 0U) {
        return;
    }
    mesh_tern_note_item(tern, frame);
    size_t at = mesh_tern_group_item_find(tern, frame->id);
    if (at == SIZE_MAX && tern->group_item_count < MESH_TERN_MAX_GROUP_ITEMS) {
        at = tern->group_item_count++;
    } else if (at == SIZE_MAX) {
        at = 0U;
        for (size_t i = 1; i < tern->group_item_count; ++i) {
            if (tern->group_items[i].id < tern->group_items[at].id) {
                at = i;
            }
        }
        if (tern->group_items[at].id > frame->id) {
            return;
        }
    }
    struct mesh_tern_group_item *item = &tern->group_items[at];
    memset(item, 0, sizeof *item);
    item->id = frame->id;
    item->kind = frame->type;
    item->flags = frame->flags;
    item->state = frame->state;
    item->reason = frame->reason;
    item->time = frame->time;
    memcpy(item->group, frame->group, MESH_TERN_GROUP_LEN);
    if (frame->type == MESH_TERN_INVITE) {
        memcpy(item->contact, frame->address, MESH_TERN_ADDRESS_LEN);
    } else {
        item->from = frame->from;
    }
    inkwell_text_sanitise((const uint8_t *)frame->text, frame->text_len, item->text,
                          sizeof item->text);
}

/* ASKED: the node refused first contact from an address that proved itself. It is not a record
   - a sync does not send it again - so the last one is all that is kept. */
static void mesh_tern_store_asked(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    char id[16];
    mesh_tern_hex_id(frame->address, id, sizeof id);
    inkwell_log_info("tern", "Node refused first contact from %s (why %u)", id,
                     (unsigned)frame->why);
    tern->has_asked = true;
    memcpy(tern->asked_address, frame->address, MESH_TERN_ADDRESS_LEN);
    tern->asked_why = frame->why;
}

/* ----------------------------------------------------------------- positions, sharing, cards */

/* When something `ago` seconds old as the record was sent happened, by this client's clock, or 0
   with none credible. */
static uint32_t mesh_tern_ago(uint32_t ago) {
    const uint32_t now = inkwell_time_wall_credible_s();
    return now == 0U ? 0U : (now > ago ? now - ago : 1U);
}

/* A contact's position onto its roster entry, or off it (`held` NULL). */
static void mesh_tern_place_node(struct mesh_tern *tern, const uint8_t *address,
                                 const struct mesh_tern_position *held) {
    struct mesh_node_summary *node = mesh_tern_node_at(tern, address, false);
    if (node == NULL) {
        return;
    }
    memset(&node->position, 0, sizeof node->position);
    if (held == NULL) {
        return;
    }
    node->position.valid = true;
    node->position.latitude_i = held->lat;
    node->position.longitude_i = held->lon;
    node->position.has_altitude = held->altitude != MESH_TERN_NO_ALTITUDE;
    node->position.altitude = node->position.has_altitude ? held->altitude : 0;
    node->position.time = held->fixed;
    /* A sync hands back what the node held; live news is a fix arriving now. */
    node->position.received =
        tern->phase == MESH_TERN_SYNCING ? 0U : inkwell_time_wall_credible_s();
    /* A Tern cell is 360 / 2^p degrees and a Meshtastic one 2^(32-b) * 10^-7, so the same number
       is a cell about a sixth smaller here than the footprint a screen draws for it: never larger.
     */
    node->position.precision_bits = held->precision;
}

static size_t mesh_tern_position_find(const struct mesh_tern_position *list, size_t count,
                                      const struct mesh_tern_frame *frame) {
    for (size_t i = 0; i < count; ++i) {
        const bool same = frame->type == MESH_TERN_POSITION
                              ? memcmp(list[i].address, frame->address, MESH_TERN_ADDRESS_LEN) == 0
                              : memcmp(list[i].group, frame->group, MESH_TERN_GROUP_LEN) == 0 &&
                                    list[i].from == frame->from;
        if (same) {
            return i;
        }
    }
    return SIZE_MAX;
}

static void mesh_tern_drop_position(struct mesh_tern *tern, bool group_kind, size_t at) {
    if (group_kind) {
        tern->group_positions[at] = tern->group_positions[--tern->group_position_count];
        return;
    }
    mesh_tern_place_node(tern, tern->positions[at].address, NULL);
    tern->positions[at] = tern->positions[--tern->position_count];
}

/* POSITION or GROUP_POSITION: the position the node holds from a sender, or with precision 0 that
   it holds none. */
static void mesh_tern_store_position(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    const bool group_kind = frame->type == MESH_TERN_GROUP_POSITION;
    struct mesh_tern_position *list = group_kind ? tern->group_positions : tern->positions;
    size_t *count = group_kind ? &tern->group_position_count : &tern->position_count;
    const size_t capacity = group_kind ? MESH_TERN_MAX_GROUP_POSITIONS : MESH_TERN_MAX_CONTACTS;
    size_t at = mesh_tern_position_find(list, *count, frame);
    if (frame->precision == 0U || frame->precision > MESH_TERN_PRECISION_MAX) {
        if (at != SIZE_MAX) {
            mesh_tern_drop_position(tern, group_kind, at);
        }
        return;
    }
    if (at == SIZE_MAX) {
        if (*count >= capacity) {
            inkwell_log_warn("tern", "Position list full; one not kept");
            return;
        }
        at = (*count)++;
    }
    struct mesh_tern_position *held = &list[at];
    memset(held, 0, sizeof *held);
    memcpy(held->address, frame->address, MESH_TERN_ADDRESS_LEN);
    memcpy(held->group, frame->group, MESH_TERN_GROUP_LEN);
    held->from = frame->from;
    held->precision = frame->precision;
    held->lat = frame->lat;
    held->lon = frame->lon;
    held->altitude = frame->altitude;
    held->accuracy = (uint8_t)(frame->accuracy > 0xFFU ? 0xFFU : frame->accuracy);
    held->fixed = mesh_tern_ago(frame->age);
    held->synced = tern->phase == MESH_TERN_SYNCING;
    if (!group_kind) {
        mesh_tern_place_node(tern, frame->address, held);
    }
}

static size_t mesh_tern_sharing_find(const struct mesh_tern *tern, bool group_kind,
                                     const struct mesh_tern_frame *frame) {
    for (size_t i = 0; i < tern->sharing_count; ++i) {
        const struct mesh_tern_sharing *held = &tern->sharing[i];
        if (held->group_kind == group_kind &&
            (group_kind ? memcmp(held->group, frame->group, MESH_TERN_GROUP_LEN) == 0
                        : memcmp(held->address, frame->address, MESH_TERN_ADDRESS_LEN) == 0)) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* SHARING or GROUP_SHARING: whether and how the node shares its position there. */
static void mesh_tern_store_sharing(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    const bool group_kind = frame->type == MESH_TERN_GROUP_SHARING;
    size_t at = mesh_tern_sharing_find(tern, group_kind, frame);
    if (frame->precision == 0U) {
        if (at != SIZE_MAX) {
            tern->sharing[at] = tern->sharing[--tern->sharing_count];
        }
        return;
    }
    if (at == SIZE_MAX) {
        if (tern->sharing_count >= INKWELL_ARRAY_LEN(tern->sharing)) {
            return;
        }
        at = tern->sharing_count++;
    }
    struct mesh_tern_sharing *held = &tern->sharing[at];
    memset(held, 0, sizeof *held);
    held->group_kind = group_kind;
    memcpy(held->address, frame->address, MESH_TERN_ADDRESS_LEN);
    memcpy(held->group, frame->group, MESH_TERN_GROUP_LEN);
    held->share = (struct mesh_tern_share){.precision = frame->precision,
                                           .fields = frame->fields,
                                           .interval = frame->interval,
                                           .minutes = frame->minutes};
    const uint32_t now = inkwell_time_wall_credible_s();
    held->ends = frame->minutes != 0U && now != 0U ? now + (uint32_t)frame->minutes * 60U : 0U;
    held->synced = tern->phase == MESH_TERN_SYNCING;
}

static size_t mesh_tern_card_find(const struct mesh_tern *tern, const uint8_t *address) {
    for (size_t i = 0; i < tern->card_count; ++i) {
        if (memcmp(tern->cards[i].address, address, MESH_TERN_ADDRESS_LEN) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* A card the node holds, onto the sender's roster entry beside - never in the place of - any name
   the user gave it: a card's name is its sender's claim. */
static void mesh_tern_store_card(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    size_t at = mesh_tern_card_find(tern, frame->address);
    if (at == SIZE_MAX) {
        if (tern->card_count >= MESH_TERN_MAX_CARDS) {
            inkwell_log_warn("tern", "Card list full; one not kept");
            return;
        }
        at = tern->card_count++;
    }
    struct mesh_tern_card *card = &tern->cards[at];
    memcpy(card->address, frame->address, MESH_TERN_ADDRESS_LEN);
    card->heard = mesh_tern_ago(frame->heard);
    card->synced = card->synced || tern->phase == MESH_TERN_SYNCING;
    inkwell_text_sanitise((const uint8_t *)frame->text, frame->text_len, card->name,
                          sizeof card->name);
    struct mesh_node_summary *node = mesh_tern_node_at(tern, frame->address, false);
    if (node == NULL) {
        return;
    }
    node->has_card = true;
    node->card_heard = card->heard;
    inkwell_str_copy(node->card_name, sizeof node->card_name, card->name);
    if (card->heard > node->last_heard) {
        node->last_heard = card->heard;
    }
}

static void mesh_tern_drop_card(struct mesh_tern *tern, const uint8_t *address) {
    const size_t at = mesh_tern_card_find(tern, address);
    if (at == SIZE_MAX) {
        return;
    }
    tern->cards[at] = tern->cards[--tern->card_count];
    struct mesh_node_summary *node = mesh_tern_roster_node(tern, mesh_tern_routing_id(address));
    if (node != NULL) {
        node->has_card = false;
        node->card_heard = 0U;
        node->card_name[0] = '\0';
    }
}

/* ---------------------------------------------------------------------------------- news */

static uint32_t mesh_tern_sync_after(const struct mesh_tern *tern);
static void mesh_tern_begin_sync(struct mesh_tern *tern, uint32_t after);
static void mesh_tern_apply_news(struct mesh_tern *tern, const struct mesh_tern_frame *frame);

/* News was missed. The sync under way, if one is, no longer proves what is gone, and the next
   reaches back at least to what was held when the first of it was. */
static void mesh_tern_missed(struct mesh_tern *tern) {
    tern->missed_news = true;
    if (!tern->has_missed_since || tern->newest_id < tern->missed_since) {
        tern->missed_since = tern->newest_id;
    }
    tern->has_missed_since = true;
}

static void mesh_tern_on_news(struct mesh_tern *tern, const uint8_t *raw, size_t len) {
    /* Every news frame counts, one whose type the version spoken does not define included: the
       count is the node's, of what it sent. Such a frame is ignored; one of a type it does
       define that cannot be read is a record lost as surely as one that never came. */
    struct mesh_tern_frame frame;
    const enum mesh_tern_decode_result result =
        mesh_tern_decode(raw, len, mesh_tern_spoken(tern), &frame);
    if (raw[1] != tern->news_expected || result == MESH_TERN_DECODE_MALFORMED) {
        inkwell_log_info("tern", "News %u %s where %u was due; syncing again", (unsigned)raw[1],
                         result == MESH_TERN_DECODE_MALFORMED ? "malformed" : "arrived",
                         (unsigned)tern->news_expected);
        mesh_tern_missed(tern);
    }
    tern->news_expected = (uint8_t)(raw[1] + 1U);
    /* A sync's answer is due after its last news, not after the wait from the request. */
    if (tern->awaiting && mesh_tern_head_type(tern) == MESH_TERN_SYNC) {
        tern->awaiting_since_ms = tern->now_ms;
    }

    if (result == MESH_TERN_DECODE_OK) {
        mesh_tern_apply_news(tern, &frame);
    }
    /* The frame that showed the gap is live news, applied as such, and only then the sync
       asked: one taken as part of the sync would be called replayed, or keep a contact the sync
       left out. Mid-sync, the sync's own end asks again (mesh_tern_synced()). */
    if (tern->missed_news && tern->phase == MESH_TERN_READY) {
        tern->missed_news = false;
        tern->phase = MESH_TERN_SYNCING;
        mesh_tern_begin_sync(tern, mesh_tern_sync_after(tern));
    }
}

static void mesh_tern_apply_news(struct mesh_tern *tern, const struct mesh_tern_frame *f) {
    const struct mesh_tern_frame frame = *f;
    switch ((enum mesh_tern_type)frame.type) {
    case MESH_TERN_SELF:
        mesh_tern_store_self(tern, &frame);
        break;
    case MESH_TERN_CONTACT:
        mesh_tern_store_contact(tern, &frame);
        break;
    case MESH_TERN_CONTACT_GONE:
        mesh_tern_drop_contact(tern, frame.address);
        break;
    case MESH_TERN_MESSAGE:
        mesh_tern_store_message(tern, &frame);
        break;
    case MESH_TERN_STATE:
        mesh_tern_store_state(tern, &frame);
        break;
    case MESH_TERN_NEIGHBOUR:
        mesh_tern_store_neighbour(tern, &frame);
        break;
    case MESH_TERN_NEIGHBOUR_GONE:
        mesh_tern_drop_neighbour(tern, frame.routing_id);
        break;
    case MESH_TERN_AIRTIME:
        mesh_tern_store_airtime(tern, &frame);
        break;
    case MESH_TERN_POWER:
        mesh_tern_store_power(tern, &frame);
        break;
    case MESH_TERN_ASKED:
        mesh_tern_store_asked(tern, &frame);
        break;
    case MESH_TERN_GROUP:
        mesh_tern_store_group(tern, &frame);
        break;
    case MESH_TERN_GROUP_GONE:
        mesh_tern_drop_group(tern, frame.group);
        break;
    case MESH_TERN_GROUP_MESSAGE:
    case MESH_TERN_INVITE:
        mesh_tern_store_group_item(tern, &frame);
        break;
    case MESH_TERN_POSITION:
    case MESH_TERN_GROUP_POSITION:
        mesh_tern_store_position(tern, &frame);
        break;
    case MESH_TERN_SHARING:
    case MESH_TERN_GROUP_SHARING:
        mesh_tern_store_sharing(tern, &frame);
        break;
    case MESH_TERN_CARD:
        mesh_tern_store_card(tern, &frame);
        break;
    case MESH_TERN_CARD_GONE:
        mesh_tern_drop_card(tern, frame.address);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------------- updates */

/* An ERROR 10 says the node does not hold what was sent where it was sent: ask where to go on
   from, but not for ever. */
#define TERN_UPDATE_TRIES_NOT_THERE 3U

static void mesh_tern_update_finish(struct mesh_tern *tern, enum mesh_tern_update_outcome outcome,
                                    uint8_t code) {
    struct mesh_tern_update *update = &tern->update;
    update->phase = MESH_TERN_UPDATE_FINISHED;
    update->outcome = outcome;
    update->code = code;
    update->image = NULL; /* the caller's again */
    inkwell_log_info("tern", "Update finished (%d, error %u) with %u of %u bytes held",
                     (int)outcome, (unsigned)code, (unsigned)update->held, (unsigned)update->size);
}

static void mesh_tern_update_begin_request(struct mesh_tern *tern) {
    struct mesh_tern_update *update = &tern->update;
    if (update->cancel_wanted) {
        mesh_tern_update_finish(tern, MESH_TERN_UPDATE_CANCELLED, 0U);
        return;
    }
    struct mesh_tern_frame frame = {.type = MESH_TERN_UPDATE_BEGIN, .size = update->size};
    memcpy(frame.digest, update->digest, sizeof frame.digest);
    update->phase = MESH_TERN_UPDATE_BEGINNING;
    if (mesh_tern_enqueue(tern, &frame) < 0) {
        update->phase = MESH_TERN_UPDATE_WAITING;
    }
}

/* The next request of an update the node has just answered: the bytes from what it holds, or
   UPDATE_END once it holds them all. */
static void mesh_tern_update_next(struct mesh_tern *tern) {
    struct mesh_tern_update *update = &tern->update;
    if (update->cancel_wanted) {
        mesh_tern_update_finish(tern, MESH_TERN_UPDATE_CANCELLED, 0U);
        return;
    }
    struct mesh_tern_frame frame;
    memset(&frame, 0, sizeof frame);
    if (update->held >= update->size) {
        frame.type = MESH_TERN_UPDATE_END;
        update->phase = MESH_TERN_UPDATE_ENDING;
    } else {
        const uint32_t left = update->size - update->held;
        frame.type = MESH_TERN_UPDATE_DATA;
        frame.offset = update->held;
        frame.data_len = (uint8_t)(left < MESH_TERN_UPDATE_CHUNK ? left : MESH_TERN_UPDATE_CHUNK);
        memcpy(frame.data, update->image + update->held, frame.data_len);
        update->phase = MESH_TERN_UPDATE_SENDING;
    }
    if (mesh_tern_enqueue(tern, &frame) < 0) {
        update->phase = MESH_TERN_UPDATE_WAITING;
    }
}

/* The answer to an update request, or the ERROR it was refused with (`code`, 0 for an answer). */
static void mesh_tern_update_answer(struct mesh_tern *tern, uint8_t asked,
                                    const struct mesh_tern_frame *frame, uint8_t code) {
    struct mesh_tern_update *update = &tern->update;
    if (update->phase != MESH_TERN_UPDATE_BEGINNING && update->phase != MESH_TERN_UPDATE_SENDING &&
        update->phase != MESH_TERN_UPDATE_ENDING) {
        return;
    }
    if (code == MESH_TERN_ERR_NOT_THERE && asked != MESH_TERN_UPDATE_BEGIN) {
        update->not_there += 1U;
        if (update->not_there >= TERN_UPDATE_TRIES_NOT_THERE) {
            mesh_tern_update_finish(tern, MESH_TERN_UPDATE_REFUSED, code);
        } else {
            mesh_tern_update_begin_request(tern);
        }
        return;
    }
    if (code != 0U) {
        mesh_tern_update_finish(tern, MESH_TERN_UPDATE_REFUSED, code);
        return;
    }
    switch (asked) {
    case MESH_TERN_UPDATE_BEGIN:
        if (frame->type != MESH_TERN_UPDATING) {
            mesh_tern_update_finish(tern, MESH_TERN_UPDATE_CONFUSED, 0U);
            return;
        }
        /* A node holds no more than the image; one that says so is sent UPDATE_END, which finds
           out what it holds. */
        update->held = frame->offset < update->size ? frame->offset : update->size;
        mesh_tern_update_next(tern);
        return;
    case MESH_TERN_UPDATE_DATA:
        if (frame->type != MESH_TERN_OK) {
            mesh_tern_update_finish(tern, MESH_TERN_UPDATE_CONFUSED, 0U);
            return;
        }
        update->not_there = 0U;
        {
            const uint32_t left = update->size - update->held;
            update->held += left < MESH_TERN_UPDATE_CHUNK ? left : MESH_TERN_UPDATE_CHUNK;
        }
        mesh_tern_update_next(tern);
        return;
    case MESH_TERN_UPDATE_END:
        mesh_tern_update_finish(tern,
                                frame->type == MESH_TERN_OK ? MESH_TERN_UPDATE_RESTARTING
                                                            : MESH_TERN_UPDATE_CONFUSED,
                                0U);
        return;
    default:
        return;
    }
}

/* The link under an update went, or the conversation started again: an update part of the way
   waits for the next connection; one whose UPDATE_END was out is never sent it again. */
static void mesh_tern_update_lost(struct mesh_tern *tern) {
    switch (tern->update.phase) {
    case MESH_TERN_UPDATE_BEGINNING:
    case MESH_TERN_UPDATE_SENDING:
        tern->update.phase = MESH_TERN_UPDATE_WAITING;
        break;
    case MESH_TERN_UPDATE_ENDING:
        mesh_tern_update_finish(tern, MESH_TERN_UPDATE_UNCONFIRMED, 0U);
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------------- answers */

/*
 * The id a sync asks after. Normally the greatest held. After missed news, one less than the
 * least of any whose record may have changed unseen (`open`), and never more than the greatest
 * held when the news was first missed: what was lost may be a record below ones that arrived
 * after it. At a later version than the last sync spoke - or with none - 0, once.
 */
static uint32_t mesh_tern_sync_after(const struct mesh_tern *tern) {
    if (!tern->has_synced_version || tern->synced_version < mesh_tern_spoken(tern)) {
        return 0U;
    }
    if (!tern->has_missed_since) {
        return tern->newest_id;
    }
    uint32_t after = tern->missed_since;
    for (size_t i = 0; i < tern->open_count; ++i) {
        if (tern->open[i].id - 1U < after) {
            after = tern->open[i].id - 1U;
        }
    }
    return after;
}

static void mesh_tern_synced(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    /* The sync's last news is the one a gap cannot show: nothing comes after it. Version 3's
       SYNCED says what the count should be by now. */
    if (mesh_tern_spoken(tern) >= 3U && frame->news != tern->news_expected) {
        inkwell_log_info("tern", "Sync ended at news %u where %u was due; syncing again",
                         (unsigned)frame->news, (unsigned)tern->news_expected);
        mesh_tern_missed(tern);
        tern->news_expected = frame->news;
    }
    if (tern->missed_news) {
        /* What this sync did not send may be what was lost: nothing is forgotten on its
           account, and it is asked again. */
        tern->missed_news = false;
        tern->phase = MESH_TERN_SYNCING;
        mesh_tern_begin_sync(tern, mesh_tern_sync_after(tern));
        return;
    }
    /* A sync is the whole of the three lists: what it did not send, the node no longer holds.
       Before version 2 a sync sends no groups, which says nothing of whether they are gone. */
    for (size_t i = tern->contact_count; i > 0U; --i) {
        if (!tern->contacts[i - 1U].synced) {
            mesh_tern_drop_contact(tern, tern->contacts[i - 1U].address);
        }
    }
    for (size_t i = tern->neighbour_count; i > 0U; --i) {
        if (!tern->neighbours[i - 1U].synced) {
            mesh_tern_drop_neighbour(tern, tern->neighbours[i - 1U].routing_id);
        }
    }
    if (mesh_tern_spoken(tern) >= 2U) {
        for (size_t i = tern->group_count; i > 0U; --i) {
            if (!tern->groups[i - 1U].synced) {
                mesh_tern_drop_group(tern, tern->groups[i - 1U].id);
            }
        }
    }
    /* Positions and sharing likewise from version 5, cards from 6. Sharing the sync sent nothing
       for is off. */
    if (mesh_tern_spoken(tern) >= 5U) {
        for (size_t i = tern->position_count; i > 0U; --i) {
            if (!tern->positions[i - 1U].synced) {
                mesh_tern_drop_position(tern, false, i - 1U);
            }
        }
        for (size_t i = tern->group_position_count; i > 0U; --i) {
            if (!tern->group_positions[i - 1U].synced) {
                mesh_tern_drop_position(tern, true, i - 1U);
            }
        }
        for (size_t i = tern->sharing_count; i > 0U; --i) {
            if (!tern->sharing[i - 1U].synced) {
                tern->sharing[i - 1U] = tern->sharing[--tern->sharing_count];
            }
        }
    }
    if (mesh_tern_spoken(tern) >= 6U) {
        for (size_t i = tern->card_count; i > 0U; --i) {
            if (!tern->cards[i - 1U].synced) {
                mesh_tern_drop_card(tern, tern->cards[i - 1U].address);
            }
        }
    }
    tern->has_synced_version = true;
    tern->synced_version = mesh_tern_spoken(tern);
    tern->has_missed_since = false;
    const bool first = tern->phase != MESH_TERN_READY;
    tern->phase = MESH_TERN_READY;
    mesh_session_model_sync_complete(tern->model);
    if (first) {
        inkwell_log_info("tern", "Synced: %zu contacts, %zu groups, %zu neighbours",
                         tern->contact_count, tern->group_count, tern->neighbour_count);
    }
    /* An update the last link dropped goes on from wherever the node says, if this node still
       speaks updates. */
    if (tern->update.phase == MESH_TERN_UPDATE_WAITING) {
        if (mesh_tern_spoken(tern) >= mesh_tern_since(MESH_TERN_UPDATE_BEGIN)) {
            mesh_tern_update_begin_request(tern);
        } else {
            mesh_tern_update_finish(tern, MESH_TERN_UPDATE_REFUSED, MESH_TERN_ERR_UNKNOWN);
        }
    }
}

static void mesh_tern_begin_sync(struct mesh_tern *tern, uint32_t after) {
    for (size_t i = 0; i < tern->contact_count; ++i) {
        tern->contacts[i].synced = false;
    }
    for (size_t i = 0; i < tern->neighbour_count; ++i) {
        tern->neighbours[i].synced = false;
    }
    for (size_t i = 0; i < tern->group_count; ++i) {
        tern->groups[i].synced = false;
    }
    for (size_t i = 0; i < tern->position_count; ++i) {
        tern->positions[i].synced = false;
    }
    for (size_t i = 0; i < tern->group_position_count; ++i) {
        tern->group_positions[i].synced = false;
    }
    for (size_t i = 0; i < tern->sharing_count; ++i) {
        tern->sharing[i].synced = false;
    }
    for (size_t i = 0; i < tern->card_count; ++i) {
        tern->cards[i].synced = false;
    }
    mesh_tern_request_sync(tern, after);
}

static int mesh_tern_hello(struct mesh_tern *tern);

static void mesh_tern_on_answer(struct mesh_tern *tern, const uint8_t *raw, size_t len) {
    if (!tern->awaiting || tern->queue_count == 0U ||
        raw[1] != tern->queue[tern->queue_head].frame[1]) {
        return; /* an answer to a request given up on */
    }
    struct mesh_tern_frame frame;
    if (mesh_tern_decode(raw, len, mesh_tern_spoken(tern), &frame) != MESH_TERN_DECODE_OK) {
        return; /* discarded; the request times out as if unanswered */
    }
    const uint8_t asked = mesh_tern_head_type(tern);
    const uint32_t ticket = tern->queue[tern->queue_head].ticket;
    mesh_tern_pop(tern);
    tern->answered_ms = tern->now_ms;

    if (frame.type == MESH_TERN_ERROR) {
        if (frame.code == MESH_TERN_ERR_HELLO_FIRST && asked != MESH_TERN_HELLO) {
            /* Taken for gone - over USB the node cannot see a client leave, and lapses one
               that went quiet - so start again. */
            inkwell_log_info("tern", "Node lapsed the connection; saying HELLO again");
            (void)mesh_tern_hello(tern);
            return;
        }
        inkwell_log_warn("tern", "Request 0x%02x refused (error %u)", (unsigned)asked,
                         (unsigned)frame.code);
        if (asked == MESH_TERN_SEND) {
            mesh_tern_note_queued(tern, ticket, 0U);
        }
        if (asked == MESH_TERN_UPDATE_BEGIN || asked == MESH_TERN_UPDATE_DATA ||
            asked == MESH_TERN_UPDATE_END) {
            mesh_tern_update_answer(tern, asked, &frame,
                                    frame.code != 0U ? frame.code : MESH_TERN_ERR_UNKNOWN);
        }
        if (asked == MESH_TERN_HELLO || asked == MESH_TERN_SYNC) {
            /* No conversation without these: drop the link and let it be opened again. */
            tern->gone = true;
        }
        mesh_tern_pump(tern);
        return;
    }

    switch (asked) {
    case MESH_TERN_HELLO:
        if (frame.type != MESH_TERN_INFO) {
            break;
        }
        tern->node_version = frame.version;
        tern->has_agreed = true;
        tern->agreed = frame.version < tern->version ? frame.version : tern->version;
        inkwell_str_copy(tern->firmware, sizeof tern->firmware, frame.text);
        inkwell_str_copy(tern->board, sizeof tern->board, frame.board);
        inkwell_str_copy(tern->release, sizeof tern->release, frame.release);
        inkwell_log_info("tern", "Node speaks version %u (%s); speaking %u",
                         (unsigned)frame.version, tern->firmware, (unsigned)tern->agreed);
        tern->phase = MESH_TERN_SYNCING;
        /* What changed while there was no connection was never sent: news missed. */
        mesh_tern_missed(tern);
        tern->missed_news = false;
        {
            const uint32_t now = inkwell_time_wall_credible_s();
            if (now != 0U) {
                const struct mesh_tern_frame set_time = {.type = MESH_TERN_SET_TIME, .time = now};
                (void)mesh_tern_enqueue(tern, &set_time);
            }
        }
        mesh_tern_begin_sync(tern, mesh_tern_sync_after(tern));
        break;
    case MESH_TERN_SYNC:
        if (frame.type == MESH_TERN_SYNCED) {
            mesh_tern_synced(tern, &frame);
        }
        break;
    case MESH_TERN_SEND:
        if (frame.type == MESH_TERN_QUEUED) {
            mesh_tern_note_queued(tern, ticket, frame.id);
        }
        break;
    case MESH_TERN_GROUP_LINK:
        if (frame.type == MESH_TERN_LINK) {
            inkwell_wipe(tern->link, sizeof tern->link);
            memcpy(tern->link, frame.text, frame.text_len);
            tern->has_link = true;
        }
        inkwell_wipe(&frame, sizeof frame);
        break;
    case MESH_TERN_UPDATE_BEGIN:
    case MESH_TERN_UPDATE_DATA:
    case MESH_TERN_UPDATE_END:
        mesh_tern_update_answer(tern, asked, &frame, 0U);
        break;
    default:
        break;
    }
    mesh_tern_pump(tern);
}

/* ------------------------------------------------------------------------- protocol ops */

/* The model's own send path while Tern holds the link: a Meshtastic verb a screen forgot to
   gate fails rather than writing a protobuf at a node that does not speak them. */
static int mesh_tern_refuse(void *ctx, const uint8_t *frame, size_t len, uint32_t frame_id) {
    (void)ctx;
    (void)frame;
    (void)len;
    (void)frame_id;
    return -EPROTONOSUPPORT;
}

static void mesh_tern_reset_link(struct mesh_tern *tern) {
    inkwell_wipe(tern->queue, sizeof tern->queue);
    mesh_tern_update_lost(tern);
    tern->queue_head = 0U;
    tern->queue_count = 0U;
    tern->awaiting = false;
    tern->gone = false;
    tern->missed_news = false;
    tern->news_expected = 0U;
    tern->queued_count = 0U;
    tern->has_agreed = false;
}

static void mesh_tern_attach(void *self, mesh_protocol_send_fn send, void *send_ctx) {
    struct mesh_tern *tern = self;
    tern->send = send;
    tern->send_ctx = send_ctx;
    mesh_session_attach(tern->model, mesh_tern_refuse, NULL);
}

static void mesh_tern_detach(void *self) {
    struct mesh_tern *tern = self;
    mesh_tern_reset_link(tern);
    tern->send = NULL;
    tern->send_ctx = NULL;
    tern->phase = MESH_TERN_IDLE;
    tern->has_airtime = false;
    mesh_session_detach(tern->model);
}

static int mesh_tern_hello(struct mesh_tern *tern) {
    mesh_tern_reset_link(tern);
    tern->phase = MESH_TERN_HELLO_SENT;
    tern->answered_ms = tern->now_ms;
    const struct mesh_tern_frame hello = {.type = MESH_TERN_HELLO, .version = tern->version};
    return mesh_tern_enqueue(tern, &hello);
}

static int mesh_tern_begin(void *self) {
    struct mesh_tern *tern = self;
    if (tern->send == NULL) {
        return -ENOTCONN;
    }
    tern->now_ms = inkwell_time_monotonic_ms();
    mesh_session_model_sync_begin(tern->model);
    return mesh_tern_hello(tern);
}

static void mesh_tern_receive(void *self, const uint8_t *frame, size_t len) {
    struct mesh_tern *tern = self;
    if (frame == NULL || len < 2U) {
        return;
    }
    if (MESH_TERN_IS_NEWS(frame[0])) {
        /* No news is sent before HELLO is answered; anything that looks like it is a frame
           from a connection before this one. */
        if (tern->phase == MESH_TERN_SYNCING || tern->phase == MESH_TERN_READY) {
            mesh_tern_on_news(tern, frame, len);
        }
    } else if (MESH_TERN_IS_ANSWER(frame[0])) {
        mesh_tern_on_answer(tern, frame, len);
    }
    /* A request, or a reserved type, is not the node's to send. */
}

static void mesh_tern_frame_failed(void *self, uint32_t frame_id) {
    (void)frame_id;
    struct mesh_tern *tern = self;
    /* A request the link could not deliver will never be answered. */
    if (tern->awaiting) {
        tern->gone = true;
    }
}

static void mesh_tern_tick(void *self, uint64_t now_ms) {
    struct mesh_tern *tern = self;
    tern->now_ms = now_ms;
    if (tern->send == NULL || tern->phase == MESH_TERN_IDLE) {
        return;
    }
    if (tern->awaiting && now_ms >= tern->awaiting_since_ms &&
        now_ms - tern->awaiting_since_ms >= MESH_TERN_ANSWER_WAIT_MS) {
        inkwell_log_warn("tern", "Request 0x%02x unanswered after %u ms; the node has gone",
                         (unsigned)mesh_tern_head_type(tern), (unsigned)MESH_TERN_ANSWER_WAIT_MS);
        mesh_tern_pop(tern);
        tern->gone = true;
        return;
    }
    /* A request no later than IDLE after the last answer, whether or not news is arriving:
       over USB that is the only way the node knows this client is still here. */
    if (tern->phase == MESH_TERN_READY && !tern->awaiting && tern->queue_count == 0U &&
        now_ms - tern->answered_ms >= MESH_TERN_IDLE_MS) {
        (void)mesh_tern_request(tern, MESH_TERN_PING);
    }
}

static bool mesh_tern_silent(const void *self) {
    const struct mesh_tern *tern = self;
    return tern->send != NULL && tern->gone;
}

static const struct mesh_protocol_ops k_tern_ops = {
    .name = "tern",
    .stream_framing = &mesh_stream_framing_tern,
    .ble_profile = &mesh_ble_profile_tern,
    .attach = mesh_tern_attach,
    .detach = mesh_tern_detach,
    .begin = mesh_tern_begin,
    .receive = mesh_tern_receive,
    .frame_failed = mesh_tern_frame_failed,
    .tick = mesh_tern_tick,
    .silent = mesh_tern_silent,
    .keepalive = NULL,
};

/* ------------------------------------------------------------------------------- public */

void mesh_tern_init(struct mesh_tern *tern, struct mesh_session *model) {
    if (tern == NULL) {
        return;
    }
    memset(tern, 0, sizeof *tern);
    tern->model = model;
    tern->version = MESH_TERN_VERSION;
}

struct mesh_protocol mesh_tern_protocol(struct mesh_tern *tern) {
    if (tern == NULL || tern->model == NULL) {
        return (struct mesh_protocol){NULL, NULL};
    }
    return (struct mesh_protocol){&k_tern_ops, tern};
}

bool mesh_tern_ready(const struct mesh_tern *tern) {
    return tern != NULL && tern->send != NULL && tern->phase == MESH_TERN_READY;
}

/* A `ref` the node can tell from every other client's: chosen at random, as the draft asks. */
static uint32_t mesh_tern_ref(struct mesh_tern *tern) {
    uint32_t ref = mesh_session_next_packet_id(tern->model);
    return ref != 0U ? ref : 1U;
}

/* The address of the node `node`: a contact's, or a node a message came from, which the roster
   holds by its address. */
static int mesh_tern_address_of(const struct mesh_tern *tern, uint32_t node, uint8_t *out) {
    const size_t contact = mesh_tern_contact_by_node(tern, node);
    if (contact != SIZE_MAX) {
        memcpy(out, tern->contacts[contact].address, MESH_TERN_ADDRESS_LEN);
        return 0;
    }
    const struct mesh_node_summary *summary = mesh_tern_roster_node(tern, node);
    if (summary == NULL || summary->public_key_len != MESH_TERN_ADDRESS_LEN ||
        mesh_tern_routing_id(summary->public_key) != node) {
        return -ENOENT;
    }
    memcpy(out, summary->public_key, MESH_TERN_ADDRESS_LEN);
    return 0;
}

/* `text` into the frame's one string, `max` bytes at most; `empty_ok` for a name. */
static int mesh_tern_put_text(struct mesh_tern_frame *frame, const char *text, size_t max,
                              bool empty_ok) {
    if (text == NULL || (!empty_ok && text[0] == '\0')) {
        return -EINVAL;
    }
    const size_t len = strlen(text);
    if (len > max) {
        return -EMSGSIZE;
    }
    if (!mesh_tern_utf8_valid((const uint8_t *)text, len)) {
        return -EINVAL;
    }
    memcpy(frame->text, text, len);
    frame->text[len] = '\0';
    frame->text_len = (uint8_t)len;
    return 0;
}

static int mesh_tern_connected(const struct mesh_tern *tern) {
    return tern->send == NULL || tern->phase == MESH_TERN_IDLE ? -ENOTCONN : 0;
}

int mesh_tern_send_text(struct mesh_tern *tern, uint32_t dest, const char *text,
                        uint32_t *out_ticket) {
    if (tern == NULL || text == NULL || text[0] == '\0') {
        return -EINVAL;
    }
    if (mesh_tern_connected(tern) < 0) {
        return -ENOTCONN;
    }
    if (strlen(text) > MESH_TERN_TEXT_MAX) {
        return -EMSGSIZE;
    }
    struct mesh_tern_frame frame = {.type = MESH_TERN_SEND, .ref = mesh_tern_ref(tern)};
    int result = mesh_tern_address_of(tern, dest, frame.address);
    if (result == 0) {
        result = mesh_tern_put_text(&frame, text, MESH_TERN_TEXT_MAX, false);
    }
    if (result < 0) {
        return result;
    }
    tern->next_ticket = tern->next_ticket + 1U != 0U ? tern->next_ticket + 1U : 1U;
    result = mesh_tern_enqueue_ticket(tern, &frame, tern->next_ticket);
    if (result == 0 && out_ticket != NULL) {
        *out_ticket = tern->next_ticket;
    }
    return result;
}

int mesh_tern_save_contact(struct mesh_tern *tern, const uint8_t address[MESH_TERN_ADDRESS_LEN],
                           const char *name) {
    if (tern == NULL || address == NULL) {
        return -EINVAL;
    }
    if (mesh_tern_connected(tern) < 0) {
        return -ENOTCONN;
    }
    struct mesh_tern_frame frame = {.type = MESH_TERN_SAVE_CONTACT};
    memcpy(frame.address, address, MESH_TERN_ADDRESS_LEN);
    const int result = mesh_tern_put_text(&frame, name, MESH_TERN_NAME_MAX, true);
    return result < 0 ? result : mesh_tern_enqueue(tern, &frame);
}

int mesh_tern_end_session(struct mesh_tern *tern, uint32_t node) {
    if (tern == NULL) {
        return -EINVAL;
    }
    if (mesh_tern_connected(tern) < 0) {
        return -ENOTCONN;
    }
    struct mesh_tern_frame frame = {.type = MESH_TERN_END_SESSION};
    const int result = mesh_tern_address_of(tern, node, frame.address);
    return result < 0 ? result : mesh_tern_enqueue(tern, &frame);
}

/* A group request: `group` (may be NULL) and the string, checked as `max` and `empty_ok` say. */
static int mesh_tern_group_request(struct mesh_tern *tern, struct mesh_tern_frame *frame,
                                   const uint8_t *group, const char *text, size_t max,
                                   bool empty_ok) {
    if (tern == NULL) {
        return -EINVAL;
    }
    if (mesh_tern_connected(tern) < 0) {
        return -ENOTCONN;
    }
    if (group != NULL) {
        memcpy(frame->group, group, MESH_TERN_GROUP_LEN);
    }
    if (max > 0U) {
        const int result = mesh_tern_put_text(frame, text, max, empty_ok);
        if (result < 0) {
            return result;
        }
    }
    return mesh_tern_enqueue(tern, frame);
}

int mesh_tern_make_group(struct mesh_tern *tern, const char *name) {
    struct mesh_tern_frame frame = {.type = MESH_TERN_MAKE_GROUP};
    return mesh_tern_group_request(tern, &frame, NULL, name, MESH_TERN_NAME_MAX, true);
}

int mesh_tern_leave_group(struct mesh_tern *tern, const uint8_t group[MESH_TERN_GROUP_LEN]) {
    struct mesh_tern_frame frame = {.type = MESH_TERN_LEAVE_GROUP};
    return group == NULL ? -EINVAL : mesh_tern_group_request(tern, &frame, group, NULL, 0U, true);
}

int mesh_tern_name_group(struct mesh_tern *tern, const uint8_t group[MESH_TERN_GROUP_LEN],
                         const char *name) {
    struct mesh_tern_frame frame = {.type = MESH_TERN_NAME_GROUP};
    return group == NULL
               ? -EINVAL
               : mesh_tern_group_request(tern, &frame, group, name, MESH_TERN_NAME_MAX, true);
}

int mesh_tern_send_group(struct mesh_tern *tern, const uint8_t group[MESH_TERN_GROUP_LEN],
                         const char *text) {
    if (tern == NULL || group == NULL) {
        return -EINVAL;
    }
    /* A `ref` as SEND's, under the same rule: the node sends a repeat once. */
    struct mesh_tern_frame frame = {.type = MESH_TERN_SEND_GROUP, .ref = mesh_tern_ref(tern)};
    return mesh_tern_group_request(tern, &frame, group, text, MESH_TERN_TEXT_MAX, false);
}

int mesh_tern_send_invite(struct mesh_tern *tern, const uint8_t group[MESH_TERN_GROUP_LEN],
                          uint32_t node) {
    if (tern == NULL || group == NULL) {
        return -EINVAL;
    }
    struct mesh_tern_frame frame = {.type = MESH_TERN_SEND_INVITE};
    const int result = mesh_tern_address_of(tern, node, frame.address);
    return result < 0 ? result : mesh_tern_group_request(tern, &frame, group, NULL, 0U, true);
}

int mesh_tern_join(struct mesh_tern *tern, uint32_t invite_id) {
    if (invite_id == 0U) {
        return -EINVAL;
    }
    struct mesh_tern_frame frame = {.type = MESH_TERN_JOIN, .id = invite_id};
    return mesh_tern_group_request(tern, &frame, NULL, NULL, 0U, true);
}

int mesh_tern_mark_read(struct mesh_tern *tern, uint32_t through) {
    if (tern == NULL || through == 0U) {
        return -EINVAL;
    }
    if (tern->send == NULL || tern->phase == MESH_TERN_IDLE) {
        return -ENOTCONN;
    }
    const struct mesh_tern_frame frame = {.type = MESH_TERN_READ,
                                          .through = through ^ mesh_tern_mix(tern)};
    return mesh_tern_enqueue(tern, &frame);
}

uint32_t mesh_tern_packet_id(const struct mesh_tern *tern, uint32_t message_id) {
    return tern != NULL && message_id != 0U ? message_id ^ mesh_tern_mix(tern) : 0U;
}

bool mesh_tern_take_queued(struct mesh_tern *tern, struct mesh_tern_queued *out) {
    if (tern == NULL || tern->queued_count == 0U) {
        return false;
    }
    const struct mesh_tern_queued oldest = tern->queued[0];
    if (oldest.id != 0U && !mesh_tern_logged(tern, oldest.id)) {
        return false; /* its MESSAGE is still to come */
    }
    if (out != NULL) {
        *out = (struct mesh_tern_queued){.ticket = oldest.ticket,
                                         .id = mesh_tern_packet_id(tern, oldest.id)};
    }
    memmove(tern->queued, tern->queued + 1, (tern->queued_count - 1U) * sizeof tern->queued[0]);
    tern->queued_count -= 1U;
    return true;
}

int mesh_tern_set_value(struct mesh_tern *tern, uint8_t setting, uint32_t value) {
    if (tern == NULL) {
        return -EINVAL;
    }
    if (mesh_tern_connected(tern) < 0) {
        return -ENOTCONN;
    }
    if (mesh_tern_setting_since(setting) > mesh_tern_spoken(tern)) {
        return -EOPNOTSUPP;
    }
    if (setting == MESH_TERN_SETTING_REGION || setting == MESH_TERN_SETTING_CARD_NAME) {
        return -EINVAL;
    }
    /* The node refuses a value it does not take - a power past its radio, cards neither on nor
       off - with ERROR 3; the client does not second-guess it. */
    const struct mesh_tern_frame frame = {.type = MESH_TERN_SET,
                                          .setting = setting,
                                          .role = (uint8_t)value,
                                          .power = (int8_t)(uint8_t)value,
                                          .passkey = value,
                                          .cards = (uint8_t)value};
    return mesh_tern_enqueue(tern, &frame);
}

int mesh_tern_set_text(struct mesh_tern *tern, uint8_t setting, const char *text) {
    if (tern == NULL) {
        return -EINVAL;
    }
    if (mesh_tern_connected(tern) < 0) {
        return -ENOTCONN;
    }
    if (mesh_tern_setting_since(setting) > mesh_tern_spoken(tern)) {
        return -EOPNOTSUPP;
    }
    if (setting != MESH_TERN_SETTING_REGION && setting != MESH_TERN_SETTING_CARD_NAME) {
        return -EINVAL;
    }
    struct mesh_tern_frame frame = {.type = MESH_TERN_SET, .setting = setting};
    const int result = mesh_tern_put_text(&frame, text,
                                          setting == MESH_TERN_SETTING_REGION ? MESH_TERN_REGION_MAX
                                                                              : MESH_TERN_NAME_MAX,
                                          setting == MESH_TERN_SETTING_CARD_NAME);
    return result < 0 ? result : mesh_tern_enqueue(tern, &frame);
}

int mesh_tern_set_position(struct mesh_tern *tern, int32_t lat, int32_t lon, int16_t altitude,
                           uint16_t accuracy, uint16_t age) {
    if (tern == NULL || lat < -900000000 || lat > 900000000 || lon < -1800000000 ||
        lon > 1800000000) {
        return -EINVAL;
    }
    if (mesh_tern_connected(tern) < 0) {
        return -ENOTCONN;
    }
    const struct mesh_tern_frame frame = {.type = MESH_TERN_SET_POSITION,
                                          .lat = lat,
                                          .lon = lon,
                                          .altitude = altitude,
                                          .accuracy = accuracy,
                                          .age = age};
    return mesh_tern_enqueue(tern, &frame);
}

static void mesh_tern_put_share(struct mesh_tern_frame *frame,
                                const struct mesh_tern_share *share) {
    frame->precision = share->precision;
    frame->fields = share->fields;
    frame->interval = share->interval;
    frame->minutes = share->minutes;
}

int mesh_tern_share(struct mesh_tern *tern, uint32_t node, const struct mesh_tern_share *share) {
    if (tern == NULL || share == NULL) {
        return -EINVAL;
    }
    if (mesh_tern_connected(tern) < 0) {
        return -ENOTCONN;
    }
    struct mesh_tern_frame frame = {.type = MESH_TERN_SHARE};
    const int result = mesh_tern_address_of(tern, node, frame.address);
    if (result < 0) {
        return result;
    }
    mesh_tern_put_share(&frame, share);
    return mesh_tern_enqueue(tern, &frame);
}

int mesh_tern_share_group(struct mesh_tern *tern, const uint8_t group[MESH_TERN_GROUP_LEN],
                          const struct mesh_tern_share *share) {
    if (share == NULL || group == NULL) {
        return -EINVAL;
    }
    struct mesh_tern_frame frame = {.type = MESH_TERN_SHARE_GROUP};
    mesh_tern_put_share(&frame, share);
    return mesh_tern_group_request(tern, &frame, group, NULL, 0U, true);
}

int mesh_tern_group_link(struct mesh_tern *tern, const uint8_t group[MESH_TERN_GROUP_LEN]) {
    struct mesh_tern_frame frame = {.type = MESH_TERN_GROUP_LINK};
    return group == NULL ? -EINVAL : mesh_tern_group_request(tern, &frame, group, NULL, 0U, true);
}

bool mesh_tern_take_link(struct mesh_tern *tern, char *out, size_t out_len) {
    if (tern == NULL || !tern->has_link) {
        return false;
    }
    if (out != NULL && out_len > 0U) {
        inkwell_str_copy(out, out_len, tern->link);
    }
    inkwell_wipe(tern->link, sizeof tern->link);
    tern->has_link = false;
    return true;
}

int mesh_tern_join_link(struct mesh_tern *tern, const char *link) {
    struct mesh_tern_frame frame = {.type = MESH_TERN_JOIN_LINK};
    const int result = mesh_tern_group_request(tern, &frame, NULL, link, MESH_TERN_LINK_MAX, false);
    inkwell_wipe(&frame, sizeof frame);
    return result;
}

int mesh_tern_update_start(struct mesh_tern *tern, const uint8_t *image, size_t size) {
    if (tern == NULL) {
        return -EINVAL;
    }
    if (!mesh_tern_ready(tern)) {
        return -ENOTCONN;
    }
    if (mesh_tern_spoken(tern) < mesh_tern_since(MESH_TERN_UPDATE_BEGIN)) {
        return -EOPNOTSUPP;
    }
    if (image == NULL || size == 0U || (uint64_t)size > (uint64_t)UINT32_MAX) {
        return -EINVAL;
    }
    struct mesh_tern_update *update = &tern->update;
    if (update->phase != MESH_TERN_UPDATE_IDLE && update->phase != MESH_TERN_UPDATE_FINISHED) {
        return -EBUSY;
    }
    memset(update, 0, sizeof *update);
    update->image = image;
    update->size = (uint32_t)size;
    struct inkwell_sha256 sha;
    inkwell_sha256_init(&sha);
    inkwell_sha256_update(&sha, image, size);
    inkwell_sha256_final(&sha, update->digest);
    mesh_tern_update_begin_request(tern);
    if (update->phase != MESH_TERN_UPDATE_BEGINNING) {
        memset(update, 0, sizeof *update); /* never begun: nothing to go on with later */
        return -ENOBUFS;
    }
    return 0;
}

bool mesh_tern_update_cancel(struct mesh_tern *tern) {
    if (tern == NULL) {
        return false;
    }
    switch (tern->update.phase) {
    case MESH_TERN_UPDATE_WAITING:
        mesh_tern_update_finish(tern, MESH_TERN_UPDATE_CANCELLED, 0U);
        return true;
    case MESH_TERN_UPDATE_BEGINNING:
    case MESH_TERN_UPDATE_SENDING:
        /* The request out is answered first: one at a time. */
        tern->update.cancel_wanted = true;
        return true;
    default:
        return false;
    }
}

const struct mesh_tern_update *mesh_tern_update_status(const struct mesh_tern *tern) {
    return tern != NULL ? &tern->update : NULL;
}
