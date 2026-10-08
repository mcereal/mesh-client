#include "mesh/core/tern.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/base/time.h"

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
 */

/* Meshtastic's role numbers, which the roster's `role` is read in: a relay forwards like a
   CLIENT, a leaf keeps to itself like a CLIENT_MUTE. */
enum {
    TERN_ROLE_AS_CLIENT = 0,
    TERN_ROLE_AS_CLIENT_MUTE = 1,
};

/* --------------------------------------------------------------------------------- requests */

static void mesh_tern_pump(struct mesh_tern *tern) {
    if (tern->send == NULL || tern->awaiting || tern->queue_count == 0U) {
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
    if (tern->queue_count >= MESH_TERN_QUEUE_LEN) {
        return -ENOBUFS;
    }
    struct mesh_tern_request *slot =
        &tern->queue[(tern->queue_head + tern->queue_count) % MESH_TERN_QUEUE_LEN];
    const int len = mesh_tern_encode(frame, slot->frame, sizeof slot->frame);
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
    tern->newest_id = 0U;
}

static void mesh_tern_store_self(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    /* Another node on this link than the last: what was learned of that one is not this one's,
       and its message ids are its own count. */
    if (tern->has_self && memcmp(tern->self.address, frame->address, MESH_TERN_ADDRESS_LEN) != 0) {
        mesh_tern_forget_books(tern);
    }
    tern->has_self = true;
    memcpy(tern->self.address, frame->address, MESH_TERN_ADDRESS_LEN);
    tern->self.role = frame->role;
    tern->self.power = frame->power;
    tern->self.time = frame->time;
    inkwell_str_copy(tern->self.region, sizeof tern->self.region, frame->text);

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

static bool mesh_tern_logged(const struct mesh_tern *tern, uint32_t id) {
    return mesh_message_log_find(&tern->model->messages, id) != NULL;
}

/*
 * One MESSAGE record. Its id is the node's, and it is the message's packet id in the log too:
 * unique on the node, the same on every client and after every restart, so a record seen again
 * - a sync after a reconnect, a received message marked read - lands on the entry it already
 * has rather than making a second.
 */
static void mesh_tern_store_message(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    if (frame->id == 0U) {
        return;
    }
    if (frame->id > tern->newest_id) {
        tern->newest_id = frame->id;
    }
    const bool inbound = frame->state == MESH_TERN_STATE_RECEIVED;
    if (mesh_tern_logged(tern, frame->id)) {
        if (!inbound) {
            (void)mesh_session_model_mark_ack(
                tern->model, frame->id, (enum mesh_message_ack)mesh_tern_ack_for(frame->state),
                frame->reason);
        }
        return;
    }
    struct mesh_node_summary *peer = mesh_tern_node_at(tern, frame->address, false);
    const uint32_t peer_id = mesh_tern_routing_id(frame->address);

    struct mesh_message message;
    memset(&message, 0, sizeof message);
    message.packet_id = frame->id;
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

static void mesh_tern_store_state(struct mesh_tern *tern, const struct mesh_tern_frame *frame) {
    if (frame->state == MESH_TERN_STATE_RECEIVED) {
        return;
    }
    (void)mesh_session_model_mark_ack(tern->model, frame->id,
                                      (enum mesh_message_ack)mesh_tern_ack_for(frame->state),
                                      frame->reason);
}

/* ---------------------------------------------------------------------------------- news */

static uint32_t mesh_tern_resync_after(const struct mesh_tern *tern);
static void mesh_tern_begin_sync(struct mesh_tern *tern, uint32_t after);

static void mesh_tern_on_news(struct mesh_tern *tern, const uint8_t *raw, size_t len) {
    /* Every news frame counts, one whose type this version does not know included: the count is
       the node's, of what it sent. */
    if (raw[1] != tern->news_expected) {
        inkwell_log_info("tern", "News %u where %u was due; syncing again", (unsigned)raw[1],
                         (unsigned)tern->news_expected);
        tern->missed_news = true;
    }
    tern->news_expected = (uint8_t)(raw[1] + 1U);
    /* Mid-sync, the sync's own end asks again (mesh_tern_synced()); otherwise now. */
    if (tern->missed_news && tern->phase == MESH_TERN_READY) {
        tern->missed_news = false;
        tern->phase = MESH_TERN_SYNCING;
        mesh_tern_begin_sync(tern, mesh_tern_resync_after(tern));
    }
    /* A sync's answer is due after its last news, not after the wait from the request. */
    if (tern->awaiting && mesh_tern_head_type(tern) == MESH_TERN_SYNC) {
        tern->awaiting_since_ms = tern->now_ms;
    }

    struct mesh_tern_frame frame;
    if (mesh_tern_decode(raw, len, &frame) != MESH_TERN_DECODE_OK) {
        return; /* news unknown is ignored, news malformed discarded */
    }
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
    default:
        break;
    }
}

/* -------------------------------------------------------------------------------- answers */

/* The id a sync after missed news asks after: one less than the least a message whose state
   may have moved unseen - one still waiting or sent - holds, or the newest held when none is. */
static uint32_t mesh_tern_resync_after(const struct mesh_tern *tern) {
    uint32_t after = tern->newest_id;
    const struct mesh_message_log *log = &tern->model->messages;
    for (size_t i = 0; i < log->count; ++i) {
        const struct mesh_message *message = mesh_message_log_at(log, i);
        if (message == NULL || message->direction != MESH_MESSAGE_OUTBOUND ||
            message->packet_id == 0U || message->packet_id > tern->newest_id) {
            continue;
        }
        if ((message->ack == MESH_MESSAGE_ACK_WAITING ||
             message->ack == MESH_MESSAGE_ACK_PENDING) &&
            message->packet_id - 1U < after) {
            after = message->packet_id - 1U;
        }
    }
    return after;
}

static void mesh_tern_synced(struct mesh_tern *tern) {
    /* A sync is the whole of both lists: what it did not send, the node no longer holds. */
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
    const bool first = tern->phase != MESH_TERN_READY;
    tern->phase = MESH_TERN_READY;
    mesh_session_model_sync_complete(tern->model);
    if (first) {
        inkwell_log_info("tern", "Synced: %zu contacts, %zu neighbours", tern->contact_count,
                         tern->neighbour_count);
    }
    if (tern->missed_news) {
        tern->missed_news = false;
        tern->phase = MESH_TERN_SYNCING;
        mesh_tern_begin_sync(tern, mesh_tern_resync_after(tern));
    }
}

static void mesh_tern_begin_sync(struct mesh_tern *tern, uint32_t after) {
    for (size_t i = 0; i < tern->contact_count; ++i) {
        tern->contacts[i].synced = false;
    }
    for (size_t i = 0; i < tern->neighbour_count; ++i) {
        tern->neighbours[i].synced = false;
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
    if (mesh_tern_decode(raw, len, &frame) != MESH_TERN_DECODE_OK) {
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
        inkwell_str_copy(tern->firmware, sizeof tern->firmware, frame.text);
        inkwell_log_info("tern", "Node speaks version %u (%s)", (unsigned)frame.version,
                         tern->firmware);
        tern->phase = MESH_TERN_SYNCING;
        {
            const uint32_t now = inkwell_time_wall_credible_s();
            if (now != 0U) {
                const struct mesh_tern_frame set_time = {.type = MESH_TERN_SET_TIME, .time = now};
                (void)mesh_tern_enqueue(tern, &set_time);
            }
        }
        mesh_tern_begin_sync(tern, tern->newest_id);
        break;
    case MESH_TERN_SYNC:
        if (frame.type == MESH_TERN_SYNCED) {
            mesh_tern_synced(tern);
        }
        break;
    case MESH_TERN_SEND:
        if (frame.type == MESH_TERN_QUEUED) {
            mesh_tern_note_queued(tern, ticket, frame.id);
        }
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
    memset(tern->queue, 0, sizeof tern->queue);
    tern->queue_head = 0U;
    tern->queue_count = 0U;
    tern->awaiting = false;
    tern->gone = false;
    tern->missed_news = false;
    tern->news_expected = 0U;
    tern->queued_count = 0U;
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
    const struct mesh_tern_frame hello = {.type = MESH_TERN_HELLO, .version = MESH_TERN_VERSION};
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

int mesh_tern_send_text(struct mesh_tern *tern, uint32_t dest, const char *text,
                        uint32_t *out_ticket) {
    if (tern == NULL || text == NULL || text[0] == '\0') {
        return -EINVAL;
    }
    if (tern->send == NULL || tern->phase == MESH_TERN_IDLE) {
        return -ENOTCONN;
    }
    const size_t len = strlen(text);
    if (len > MESH_TERN_TEXT_MAX) {
        return -EMSGSIZE;
    }
    struct mesh_tern_frame frame = {.type = MESH_TERN_SEND, .ref = mesh_tern_ref(tern)};
    const size_t contact = mesh_tern_contact_by_node(tern, dest);
    if (contact != SIZE_MAX) {
        memcpy(frame.address, tern->contacts[contact].address, MESH_TERN_ADDRESS_LEN);
    } else {
        /* A node a message came from, which the roster holds by its address. */
        const struct mesh_node_summary *node = mesh_tern_roster_node(tern, dest);
        if (node == NULL || node->public_key_len != MESH_TERN_ADDRESS_LEN ||
            mesh_tern_routing_id(node->public_key) != dest) {
            return -ENOENT;
        }
        memcpy(frame.address, node->public_key, MESH_TERN_ADDRESS_LEN);
    }
    if (!mesh_tern_utf8_valid((const uint8_t *)text, len)) {
        return -EINVAL;
    }
    memcpy(frame.text, text, len);
    frame.text_len = (uint8_t)len;
    tern->next_ticket = tern->next_ticket + 1U != 0U ? tern->next_ticket + 1U : 1U;
    const int result = mesh_tern_enqueue_ticket(tern, &frame, tern->next_ticket);
    if (result == 0 && out_ticket != NULL) {
        *out_ticket = tern->next_ticket;
    }
    return result;
}

int mesh_tern_mark_read(struct mesh_tern *tern, uint32_t through) {
    if (tern == NULL || through == 0U) {
        return -EINVAL;
    }
    if (tern->send == NULL || tern->phase == MESH_TERN_IDLE) {
        return -ENOTCONN;
    }
    const struct mesh_tern_frame frame = {.type = MESH_TERN_READ, .through = through};
    return mesh_tern_enqueue(tern, &frame);
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
        *out = oldest;
    }
    memmove(tern->queued, tern->queued + 1, (tern->queued_count - 1U) * sizeof tern->queued[0]);
    tern->queued_count -= 1U;
    return true;
}
