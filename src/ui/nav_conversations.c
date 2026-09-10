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
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_CHANNEL_PRIMARY));
    } else {
        mesh_str_format(out, out_len, MESH_STR_CHANNEL_NUMBERED, (unsigned)index);
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
                             enum mesh_ui_picker_follow follow) {
    nav->picker_open = true;
    nav->picker_follow = (uint8_t)follow;
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
            /* Whatever asked for the picker gets its own follow-up over the new thread. */
            if (nav->picker_follow == (uint8_t)MESH_UI_PICKER_FOLLOW_KEYBOARD) {
                mesh_ui_nav_open_keyboard(nav);
            } else if (nav->picker_follow == (uint8_t)MESH_UI_PICKER_FOLLOW_QUICK) {
                mesh_ui_nav_open_compose(nav);
            }
        }
        nav->picker_open = false;
        nav->picker_follow = (uint8_t)MESH_UI_PICKER_FOLLOW_NONE;
        return true;
    }
    case MESH_UI_KEY_B:
        nav->picker_open = false;
        nav->picker_follow = (uint8_t)MESH_UI_PICKER_FOLLOW_NONE;
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
        /* A reaction is an annotation on a message rather than one of its own, so it is not the
           conversation's preview and does not raise its unread count - the same rule the thread
           filter follows. A row reading "\U0001F44D" says nothing about what was said. */
        if (message->is_reaction) {
            continue;
        }
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

    conversation->muted = mesh_ui_store_conversation_muted(
        store, conversation->kind, conversation->node, conversation->channel);
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
        /*
         * A muted conversation contributes nothing, and this is the one place that rule needs
         * to be written: this total is what the navigation bar badges the Messages tab with and
         * what the all-traffic row carries, so both are "how much is waiting that asked to be
         * waited for". The rows themselves still count their own - see struct
         * mesh_ui_conversation.
         *
         * It is also what keeps the badge worth looking at. One busy channel outruns everything
         * else on a mesh, and a tab that is permanently badged says exactly as much as one that
         * never is.
         */
        if (conversation.muted) {
            continue;
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

/* The cell classification behind mesh_ui_nav_initials(), whose contract is in nav.h. */
static bool mesh_ui_nav_is_word_break(unsigned char c) {
    return c == ' ' || c == '_' || c == '-' || c == '.';
}

/* A character that can stand for a name. Everything outside ASCII counts: it is one drawn
   cell whatever it is spelled with, and there is no case to fold. */
static bool mesh_ui_nav_is_name_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80U;
}

/* Bytes in the cell starting at `p`: one for ASCII, the whole sequence for anything else. */
static size_t mesh_ui_nav_cell_bytes(const unsigned char *p) {
    size_t bytes = 1U;
    while (p[bytes] != '\0' && (p[bytes] & 0xC0U) == 0x80U) {
        ++bytes;
    }
    return bytes;
}

/* Appends the cell at `p`, upper-cased when it is a lower-case ASCII letter. */
static void mesh_ui_nav_append_cell(const unsigned char *p, char *out, size_t out_len,
                                    size_t *written) {
    const size_t bytes = mesh_ui_nav_cell_bytes(p);
    if (*written + bytes + 1U > out_len) {
        return;
    }
    if (bytes == 1U && *p >= 'a' && *p <= 'z') {
        out[(*written)++] = (char)(*p - ('a' - 'A'));
    } else {
        memcpy(&out[*written], p, bytes);
        *written += bytes;
    }
    out[*written] = '\0';
}

void mesh_ui_nav_initials(const char *name, char *out, size_t out_len) {
    out[0] = '\0';
    if (name == NULL || out_len < 2U) {
        return;
    }
    const unsigned char *text = (const unsigned char *)name;

    /* The first letter, whatever punctuation leads. */
    size_t i = 0U;
    while (text[i] != '\0' && !mesh_ui_nav_is_name_char(text[i])) {
        ++i;
    }
    if (text[i] == '\0') {
        return; /* nothing in the name is a letter; the disc draws empty */
    }
    size_t written = 0U;
    mesh_ui_nav_append_cell(&text[i], out, out_len, &written);
    i += mesh_ui_nav_cell_bytes(&text[i]);

    /* The second: the first letter of the next word if the name has one, else simply the
       character after the first - which is the case that matters here, because a Meshtastic
       short name is one word of four ("BRVO" -> "BR"). */
    const size_t next = i;
    bool after_break = false;
    for (; text[i] != '\0'; ++i) {
        if (mesh_ui_nav_is_word_break(text[i])) {
            after_break = true;
            continue;
        }
        if (after_break && mesh_ui_nav_is_name_char(text[i])) {
            mesh_ui_nav_append_cell(&text[i], out, out_len, &written);
            return;
        }
    }
    if (mesh_ui_nav_is_name_char(text[next])) {
        mesh_ui_nav_append_cell(&text[next], out, out_len, &written);
    }
}

/*
 * The seed that colours a channel's disc.
 *
 * The channel *slot* rather than its name, so renaming a channel keeps the colour the user has
 * learned to look for - and offset well clear of the node-number space so a channel and a node
 * cannot collide onto the same tint. One function because two lists ask it, and a tint derived
 * two ways is the same channel in two colours.
 */
static uint32_t mesh_ui_nav_channel_tint(uint8_t channel) {
    return 0x0C000000U + (uint32_t)channel;
}

/* Everything a backend needs to draw the avatar: the two cells and the seed that colours
   them. The seed is the conversation's identity - a node number, a channel slot - rather than
   its name, so the tint survives a rename. */
static void mesh_ui_nav_conversation_avatar(struct mesh_ui_conversation *out) {
    switch ((enum mesh_ui_conversation_kind)out->kind) {
    case MESH_UI_CONVERSATION_ALL:
        /* Not a person and not a place: the one row that is a view over the others gets a
           mark rather than initials, and the backend tints it with the accent. */
        mesh_str_copy(out->initials, sizeof out->initials, "*");
        out->tint = 0U;
        return;
    case MESH_UI_CONVERSATION_NEW:
        mesh_str_copy(out->initials, sizeof out->initials, "+");
        out->tint = 0U;
        return;
    case MESH_UI_CONVERSATION_CHANNEL:
        /* A channel is a place, and '#' is what says so everywhere else on this screen. The
           slot rather than the name seeds it, so renaming a channel keeps its colour. */
        mesh_str_copy(out->initials, sizeof out->initials, "#");
        out->tint = mesh_ui_nav_channel_tint(out->channel);
        return;
    case MESH_UI_CONVERSATION_DIRECT:
    default:
        mesh_ui_nav_initials(out->name, out->initials, sizeof out->initials);
        out->tint = out->node;
        return;
    }
}

/* The same two facts for any target a list can show. A channel is a place and wears '#' seeded
   by its slot, exactly as MESH_UI_CONVERSATION_CHANNEL does; a node wears the initials of the
   name the conversation list knows it by, which is why this resolves the node itself rather
   than taking the caller's display string or one of the node's fields. */
void mesh_ui_nav_target_avatar(const struct mesh_ui_store *store, uint32_t node, uint8_t channel,
                               char *out_initials, size_t out_len, uint32_t *out_tint) {
    if (out_initials == NULL || out_len == 0U) {
        return;
    }
    if (node == MESH_MESSAGE_BROADCAST_ADDR) {
        mesh_str_copy(out_initials, out_len, "#");
        if (out_tint != NULL) {
            *out_tint = mesh_ui_nav_channel_tint(channel);
        }
        return;
    }
    char name[MESH_UI_NAV_TARGET_NAME_MAX];
    mesh_ui_nav_node_name(store, node, name, sizeof name);
    mesh_ui_nav_initials(name, out_initials, out_len);
    if (out_tint != NULL) {
        *out_tint = node;
    }
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
        snprintf(out->name, sizeof out->name, "%s", mesh_str(MESH_STR_MESSAGES_ALL_TRAFFIC));
        mesh_ui_nav_conversation_summarise(store, out);
        /* All traffic is a view, not a conversation: it keeps no mark of its own (opening it
           marks nothing read), so its badge is what the rows below it still owe. */
        out->unread = mesh_ui_nav_unread_total(store);
        mesh_ui_nav_conversation_avatar(out);
        return true;
    }
    if (index < 1U + channels) {
        out->kind = MESH_UI_CONVERSATION_CHANNEL;
        out->channel = slots[index - 1U];
        mesh_ui_nav_channel_name(store, out->channel, out->name, sizeof out->name);
        mesh_ui_nav_conversation_summarise(store, out);
        mesh_ui_nav_conversation_avatar(out);
        return true;
    }
    if (index < 1U + channels + directs) {
        out->kind = MESH_UI_CONVERSATION_DIRECT;
        out->node = peers[index - 1U - channels];
        mesh_ui_nav_node_name(store, out->node, out->name, sizeof out->name);
        mesh_ui_nav_conversation_summarise(store, out);
        mesh_ui_nav_conversation_avatar(out);
        return true;
    }
    if (index == 1U + channels + directs) {
        out->kind = MESH_UI_CONVERSATION_NEW;
        snprintf(out->name, sizeof out->name, "%s", mesh_str(MESH_STR_MESSAGES_NEW));
        mesh_ui_nav_conversation_avatar(out);
        return true;
    }
    return false;
}

bool mesh_ui_nav_conversation_is_armed(const struct mesh_ui_nav *nav,
                                       const struct mesh_ui_conversation *conversation) {
    if (nav == NULL || conversation == NULL || !nav->messages_delete_armed ||
        nav->messages_delete_kind != conversation->kind) {
        return false;
    }
    switch ((enum mesh_ui_conversation_kind)conversation->kind) {
    case MESH_UI_CONVERSATION_CHANNEL:
        return nav->messages_delete_channel == conversation->channel;
    case MESH_UI_CONVERSATION_DIRECT:
        return nav->messages_delete_node == conversation->node;
    case MESH_UI_CONVERSATION_ALL:
    case MESH_UI_CONVERSATION_NEW:
    default:
        return false;
    }
}

/*
 * X on a conversation row: arm the delete, or carry it out when this row is already armed.
 *
 * Neither "All traffic" nor "New message" is a conversation - one is a view over the others
 * and the other is a button - so X on either does nothing at all rather than doing something
 * surprising with the whole log. Returns true when the frame changed.
 */
bool mesh_ui_nav_delete_conversation(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                     uint32_t index, struct mesh_ui_action *action) {
    struct mesh_ui_conversation conversation;
    if (!mesh_ui_nav_conversation_at(store, index, &conversation)) {
        return false;
    }
    if (conversation.kind != MESH_UI_CONVERSATION_CHANNEL &&
        conversation.kind != MESH_UI_CONVERSATION_DIRECT) {
        return false;
    }

    if (!mesh_ui_nav_conversation_is_armed(nav, &conversation)) {
        nav->messages_delete_armed = true;
        nav->messages_delete_kind = conversation.kind;
        nav->messages_delete_channel = conversation.channel;
        nav->messages_delete_node = conversation.node;
        return true;
    }

    nav->messages_delete_armed = false;
    if (action != NULL) {
        action->type = MESH_UI_ACTION_DELETE_CONVERSATION;
        action->number = conversation.kind;
        action->dest = conversation.node;
        action->channel = conversation.channel;
        /* The name off the row rather than one the app resolves again: what the toast should
           say is what the user was looking at when they pressed X, and a channel's name lives
           in the handshake's channel table that only this layer walks. */
        mesh_str_copy(action->text, sizeof action->text, conversation.name);
    }
    /* The frame changes when the app publishes the shorter log, not here: saying "deleted"
       before the messages have gone is how a failed delete comes to look like a successful
       one. Standing the arming down is a change on its own, though. */
    return true;
}

/*
 * START on a conversation row: mute it, or let it interrupt again.
 *
 * The press goes to the app rather than being done here because the mute lives in the store and
 * the nav is handed a `const` one - the same reason deleting a conversation is an action. What
 * comes back is a toast and a republished list.
 */
bool mesh_ui_nav_mute_conversation(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   uint32_t index, struct mesh_ui_action *action) {
    (void)nav;
    struct mesh_ui_conversation conversation;
    if (!mesh_ui_nav_conversation_at(store, index, &conversation)) {
        return false;
    }
    if (conversation.kind != MESH_UI_CONVERSATION_CHANNEL &&
        conversation.kind != MESH_UI_CONVERSATION_DIRECT) {
        return false;
    }
    if (action != NULL) {
        action->type = MESH_UI_ACTION_MUTE_CONVERSATION;
        action->number = conversation.kind;
        action->dest = conversation.node;
        action->channel = conversation.channel;
        /* The name off the row, for the toast - the delete's reasoning exactly: a channel's
           name lives in the handshake's channel table that only this layer walks. */
        mesh_str_copy(action->text, sizeof action->text, conversation.name);
    }
    return false;
}

/* A on a conversation row. Returns true when the frame changed. */
bool mesh_ui_nav_open_conversation(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   uint32_t index) {
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
        mesh_ui_nav_picker_open(nav, store, MESH_UI_PICKER_FOLLOW_QUICK);
        return true;
    }
    /* The thread and nothing over it: what to do in it is the next press's business. */
    return true;
}
