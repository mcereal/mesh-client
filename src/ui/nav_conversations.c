#define _POSIX_C_SOURCE 200809L

/*
 * Who the user is talking to: channel and node naming, the conversation list, and the send-to
 * picker.
 *
 * The Messages tab is a list of conversations rather than of messages, so it has to be derived
 * on every draw from the flat message log the transport keeps - there is no conversation object
 * anywhere. mesh_ui_nav_conversation_summarise() is that derivation, and the unread counts come
 * out of the same pass.
 */

#include "nav_internal.h"

#include "mesh/core/message.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <string.h>

/* ---- channels and names ------------------------------------------------------------------ */

static const struct mesh_ui_channel *mesh_ui_nav_channel(const struct mesh_ui_store *store,
                                                         uint8_t index) {
    if (store == NULL || !store->handshake_valid) {
        return NULL;
    }
    const struct mesh_ui_handshake_state *hs = &store->handshake;
    for (uint32_t i = 0; i < hs->channel_count && i < MESH_UI_MAX_CHANNELS; ++i) {
        if (hs->channels[i].index == index && hs->channels[i].role != 0U) {
            return &hs->channels[i];
        }
    }
    return NULL;
}

void mesh_ui_nav_channel_name(const struct mesh_ui_store *store, uint8_t index, char *out,
                                     size_t out_len) {
    const struct mesh_ui_channel *channel = mesh_ui_nav_channel(store, index);
    if (channel != NULL && channel->name[0] != '\0') {
        snprintf(out, out_len, "#%s", channel->name);
    } else if (index == 0U) {
        /* An unnamed slot 0 is the default primary channel; the firmware shows the modem
           preset name there, which we do not track. */
        snprintf(out, out_len, "%s", "#Primary");
    } else {
        snprintf(out, out_len, "#Ch%u", (unsigned)index);
    }
}

void mesh_ui_nav_node_name(const struct mesh_ui_store *store, uint32_t node_id, char *out,
                                  size_t out_len) {
    if (store != NULL && store->handshake_valid) {
        const struct mesh_ui_handshake_state *hs = &store->handshake;
        for (uint32_t i = 0; i < hs->node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
            if (hs->nodes[i].node_id != node_id) {
                continue;
            }
            if (hs->nodes[i].short_name[0] != '\0') {
                snprintf(out, out_len, "%s", hs->nodes[i].short_name);
                return;
            }
            if (hs->nodes[i].long_name[0] != '\0') {
                snprintf(out, out_len, "%s", hs->nodes[i].long_name);
                return;
            }
            break;
        }
    }
    snprintf(out, out_len, "!%08x", node_id);
}

/* ---- send-to picker ----------------------------------------------------------------------- */

static uint32_t mesh_ui_nav_enabled_channels(const struct mesh_ui_store *store, uint8_t *slots,
                                             uint32_t capacity) {
    uint32_t count = 0U;
    if (store != NULL && store->handshake_valid) {
        const struct mesh_ui_handshake_state *hs = &store->handshake;
        for (uint32_t i = 0; i < hs->channel_count && i < MESH_UI_MAX_CHANNELS && count < capacity;
             ++i) {
            if (hs->channels[i].role != 0U) {
                slots[count++] = hs->channels[i].index;
            }
        }
    }
    if (count == 0U && capacity > 0U) {
        slots[count++] = 0U; /* no table yet: the primary slot still exists */
    }
    return count;
}

uint32_t mesh_ui_nav_picker_count(const struct mesh_ui_store *store) {
    uint8_t slots[MESH_UI_MAX_CHANNELS];
    uint32_t count = mesh_ui_nav_enabled_channels(store, slots, MESH_UI_MAX_CHANNELS);
    if (store != NULL && store->handshake_valid) {
        const struct mesh_ui_handshake_state *hs = &store->handshake;
        const uint32_t me = hs->has_my_info ? hs->my_info.node_num : 0U;
        for (uint32_t i = 0; i < hs->node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
            if (hs->nodes[i].node_id != 0U && hs->nodes[i].node_id != me) {
                count++;
            }
        }
    }
    return count;
}

bool mesh_ui_nav_picker_row(const struct mesh_ui_store *store, uint32_t index, uint32_t *out_node,
                            uint8_t *out_channel, char *out_name, size_t out_name_len) {
    uint8_t slots[MESH_UI_MAX_CHANNELS];
    const uint32_t channels = mesh_ui_nav_enabled_channels(store, slots, MESH_UI_MAX_CHANNELS);
    if (index < channels) {
        if (out_node != NULL) {
            *out_node = MESH_MESSAGE_BROADCAST_ADDR;
        }
        if (out_channel != NULL) {
            *out_channel = slots[index];
        }
        if (out_name != NULL) {
            mesh_ui_nav_channel_name(store, slots[index], out_name, out_name_len);
        }
        return true;
    }
    if (store == NULL || !store->handshake_valid) {
        return false;
    }
    const struct mesh_ui_handshake_state *hs = &store->handshake;
    const uint32_t me = hs->has_my_info ? hs->my_info.node_num : 0U;
    uint32_t position = channels;
    for (uint32_t i = 0; i < hs->node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
        const struct mesh_ui_node_summary *node = &hs->nodes[i];
        if (node->node_id == 0U || node->node_id == me) {
            continue;
        }
        if (position == index) {
            if (out_node != NULL) {
                *out_node = node->node_id;
            }
            if (out_channel != NULL) {
                *out_channel = 0U;
            }
            if (out_name != NULL) {
                if (node->long_name[0] != '\0' && node->short_name[0] != '\0') {
                    snprintf(out_name, out_name_len, "%s  %s", node->short_name, node->long_name);
                } else {
                    mesh_ui_nav_node_name(store, node->node_id, out_name, out_name_len);
                }
            }
            return true;
        }
        position++;
    }
    return false;
}

void mesh_ui_nav_picker_open(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                    bool then_compose) {
    nav->picker_open = true;
    nav->picker_to_compose = then_compose;
    nav->picker_cursor = 0U;
    /* Start on the current target so a stray A changes nothing. */
    const uint32_t count = mesh_ui_nav_picker_count(store);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t node = 0U;
        uint8_t channel = 0U;
        if (!mesh_ui_nav_picker_row(store, i, &node, &channel, NULL, 0U)) {
            break;
        }
        if (node == nav->target_node &&
            (node != MESH_MESSAGE_BROADCAST_ADDR || channel == nav->target_channel)) {
            nav->picker_cursor = i;
            break;
        }
    }
}

bool mesh_ui_nav_picker_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   enum mesh_ui_key key) {
    const uint32_t count = mesh_ui_nav_picker_count(store);
    if (nav->picker_cursor >= count && count > 0U) {
        nav->picker_cursor = count - 1U;
    }
    switch (key) {
    case MESH_UI_KEY_UP:
        if (nav->picker_cursor == 0U) {
            return false;
        }
        nav->picker_cursor--;
        return true;
    case MESH_UI_KEY_DOWN:
        if (nav->picker_cursor + 1U >= count) {
            return false;
        }
        nav->picker_cursor++;
        return true;
    case MESH_UI_KEY_LEFT:
    case MESH_UI_KEY_L1:
        /* Page up: a 130-node mesh is not walked one row at a time. */
        nav->picker_cursor = nav->picker_cursor > 10U ? nav->picker_cursor - 10U : 0U;
        return true;
    case MESH_UI_KEY_RIGHT:
    case MESH_UI_KEY_R1:
        if (count == 0U) {
            return false;
        }
        nav->picker_cursor =
            nav->picker_cursor + 10U < count ? nav->picker_cursor + 10U : count - 1U;
        return true;
    case MESH_UI_KEY_A:
    case MESH_UI_KEY_START: {
        uint32_t node = 0U;
        uint8_t channel = 0U;
        if (mesh_ui_nav_picker_row(store, nav->picker_cursor, &node, &channel, NULL, 0U)) {
            /* Picking is the one other way to open a thread: it names a destination, so the
               target moves and the conversation opens together. */
            mesh_ui_nav_open_thread(nav, store, node, channel, NULL);
            if (nav->picker_to_compose) {
                mesh_ui_nav_open_compose(nav);
            }
        }
        nav->picker_open = false;
        nav->picker_to_compose = false;
        return true;
    }
    case MESH_UI_KEY_B:
        nav->picker_open = false;
        nav->picker_to_compose = false;
        return true;
    default:
        return false;
    }
}

/* ---- conversation list --------------------------------------------------------------------- */

/* The distinct nodes we have direct messages with, newest traffic first. Returns how many were
   written; the ring caps the answer, so no allocation is needed anywhere. */
static uint32_t mesh_ui_nav_direct_peers(const struct mesh_ui_store *store, uint32_t *out_peers,
                                         uint32_t capacity) {
    if (store == NULL) {
        return 0U;
    }
    const struct mesh_ui_message_list *messages = &store->messages;
    const uint32_t count =
        messages->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : messages->count;
    uint32_t written = 0U;
    /* The log is oldest-first, so walk it backwards to meet the newest peer first. */
    for (uint32_t i = count; i > 0U; --i) {
        const struct mesh_ui_message *message = &messages->entries[i - 1U];
        if (message->broadcast) {
            continue;
        }
        bool duplicate = false;
        for (uint32_t j = 0; j < written; ++j) {
            if (out_peers[j] == message->peer) {
                duplicate = true;
                break;
            }
        }
        if (duplicate || written >= capacity) {
            continue;
        }
        out_peers[written++] = message->peer;
    }
    return written;
}

/* The mark for one conversation, or NULL when it has never been read. */
static const struct mesh_ui_read_mark *mesh_ui_nav_read_mark(const struct mesh_ui_read_state *state,
                                                             uint8_t kind, uint32_t node,
                                                             uint8_t channel) {
    if (state == NULL) {
        return NULL;
    }
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

/* Newest message in a conversation for the list's preview line, plus how many inbound ones
   arrived after it was last read. */
static void mesh_ui_nav_conversation_summarise(const struct mesh_ui_store *store,
                                               struct mesh_ui_conversation *conversation) {
    const struct mesh_ui_message_list *messages = &store->messages;
    const uint32_t count =
        messages->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : messages->count;

    const struct mesh_ui_read_mark *mark = mesh_ui_nav_read_mark(
        &store->read_state, conversation->kind, conversation->node, conversation->channel);
    const uint32_t read_id = (mark != NULL) ? mark->packet_id : 0U;
    /* Everything inbound, and everything inbound since the mark. Which one is the unread count
       depends on whether the marked message is still in the log: if the ring has evicted it,
       every message still in view arrived after it. */
    uint32_t inbound_total = 0U;
    uint32_t since_mark = 0U;
    bool mark_seen = false;

    for (uint32_t i = 0; i < count; ++i) {
        const struct mesh_ui_message *message = &messages->entries[i];
        bool belongs = false;
        switch ((enum mesh_ui_conversation_kind)conversation->kind) {
        case MESH_UI_CONVERSATION_ALL:
            belongs = true;
            break;
        case MESH_UI_CONVERSATION_CHANNEL:
            belongs = message->broadcast && message->channel == conversation->channel;
            break;
        case MESH_UI_CONVERSATION_DIRECT:
            belongs = !message->broadcast && message->peer == conversation->node;
            break;
        case MESH_UI_CONVERSATION_NEW:
        default:
            return;
        }
        if (!belongs) {
            continue;
        }
        conversation->message_count++;
        /* Oldest first, so the last match seen is the newest. The preview is one list row;
           anything longer is the thread's business. */
        mesh_str_copy(conversation->preview, sizeof conversation->preview, message->text);
        conversation->last_time = message->rx_time;
        conversation->preview_outbound = (message->direction == MESH_MESSAGE_OUTBOUND);

        if (read_id != 0U && message->packet_id == read_id) {
            mark_seen = true;
            since_mark = 0U; /* the marked message and everything before it are read */
            continue;
        }
        if (message->direction != MESH_MESSAGE_OUTBOUND) {
            inbound_total++;
            since_mark++;
        }
    }

    if (read_id == 0U || !mark_seen) {
        conversation->unread = inbound_total;
    } else {
        conversation->unread = since_mark;
    }
}

uint32_t mesh_ui_nav_unread_total(const struct mesh_ui_store *store) {
    if (store == NULL) {
        return 0U;
    }
    const uint32_t rows = mesh_ui_nav_conversation_count(store);
    uint32_t total = 0U;
    /* Row 0 is the all-traffic row this total belongs to, and the last is "New message". */
    for (uint32_t i = 1U; i + 1U < rows; ++i) {
        struct mesh_ui_conversation conversation;
        if (!mesh_ui_nav_conversation_at(store, i, &conversation)) {
            break;
        }
        total += conversation.unread;
    }
    return total;
}

uint32_t mesh_ui_nav_conversation_count(const struct mesh_ui_store *store) {
    uint8_t slots[MESH_UI_MAX_CHANNELS];
    uint32_t peers[MESH_UI_MAX_MESSAGES];
    const uint32_t channels = mesh_ui_nav_enabled_channels(store, slots, MESH_UI_MAX_CHANNELS);
    const uint32_t directs = mesh_ui_nav_direct_peers(store, peers, MESH_UI_MAX_MESSAGES);
    /* All traffic + channels + direct peers + New message. */
    return 1U + channels + directs + 1U;
}

bool mesh_ui_nav_conversation_at(const struct mesh_ui_store *store, uint32_t index,
                                 struct mesh_ui_conversation *out) {
    if (store == NULL || out == NULL) {
        return false;
    }
    uint8_t slots[MESH_UI_MAX_CHANNELS];
    uint32_t peers[MESH_UI_MAX_MESSAGES];
    const uint32_t channels = mesh_ui_nav_enabled_channels(store, slots, MESH_UI_MAX_CHANNELS);
    const uint32_t directs = mesh_ui_nav_direct_peers(store, peers, MESH_UI_MAX_MESSAGES);

    memset(out, 0, sizeof *out);
    out->node = MESH_MESSAGE_BROADCAST_ADDR;

    if (index == 0U) {
        out->kind = MESH_UI_CONVERSATION_ALL;
        snprintf(out->name, sizeof out->name, "%s", "All traffic");
        mesh_ui_nav_conversation_summarise(store, out);
        /* All traffic is a view, not a conversation: it keeps no mark of its own (opening it
           marks nothing read), so its badge is what the rows below it still owe. */
        out->unread = mesh_ui_nav_unread_total(store);
        return true;
    }
    if (index < 1U + channels) {
        out->kind = MESH_UI_CONVERSATION_CHANNEL;
        out->channel = slots[index - 1U];
        mesh_ui_nav_channel_name(store, out->channel, out->name, sizeof out->name);
        mesh_ui_nav_conversation_summarise(store, out);
        return true;
    }
    if (index < 1U + channels + directs) {
        out->kind = MESH_UI_CONVERSATION_DIRECT;
        out->node = peers[index - 1U - channels];
        mesh_ui_nav_node_name(store, out->node, out->name, sizeof out->name);
        mesh_ui_nav_conversation_summarise(store, out);
        return true;
    }
    if (index == 1U + channels + directs) {
        out->kind = MESH_UI_CONVERSATION_NEW;
        snprintf(out->name, sizeof out->name, "%s", "New message");
        return true;
    }
    return false;
}

bool mesh_ui_nav_conversation_is_open(const struct mesh_ui_nav *nav,
                                      const struct mesh_ui_conversation *conversation) {
    if (nav == NULL || conversation == NULL || !nav->thread_open) {
        return false;
    }
    switch ((enum mesh_ui_conversation_kind)conversation->kind) {
    case MESH_UI_CONVERSATION_ALL:
        return nav->inbox;
    case MESH_UI_CONVERSATION_CHANNEL:
        return !nav->inbox && nav->target_node == MESH_MESSAGE_BROADCAST_ADDR &&
               nav->target_channel == conversation->channel;
    case MESH_UI_CONVERSATION_DIRECT:
        return !nav->inbox && nav->target_node == conversation->node;
    case MESH_UI_CONVERSATION_NEW:
    default:
        return false;
    }
}

/* A on a conversation row. Returns true when the frame changed. */
bool mesh_ui_nav_open_conversation(struct mesh_ui_nav *nav,
                                          const struct mesh_ui_store *store, uint32_t index,
                                          bool then_compose) {
    struct mesh_ui_conversation conversation;
    if (!mesh_ui_nav_conversation_at(store, index, &conversation)) {
        return false;
    }
    switch ((enum mesh_ui_conversation_kind)conversation.kind) {
    case MESH_UI_CONVERSATION_ALL:
        /* Nothing to compose to: all traffic is a view, not a destination. */
        mesh_ui_nav_open_all_traffic(nav);
        return true;
    case MESH_UI_CONVERSATION_CHANNEL:
        mesh_ui_nav_open_thread(nav, store, MESH_MESSAGE_BROADCAST_ADDR, conversation.channel,
                                NULL);
        break;
    case MESH_UI_CONVERSATION_DIRECT:
        mesh_ui_nav_open_thread(nav, store, conversation.node, 0U, NULL);
        break;
    case MESH_UI_CONVERSATION_NEW:
    default:
        mesh_ui_nav_picker_open(nav, store, true);
        return true;
    }
    if (then_compose) {
        mesh_ui_nav_open_compose(nav);
    }
    return true;
}
