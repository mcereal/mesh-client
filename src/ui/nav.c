#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/nav.h"

#include "mesh/mesh_message.h"
#include "mesh/ui/store.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Short, unambiguous, and what you actually want to say with no keyboard. Replaceable through
   mesh_ui_canned_load(). */
static const char *const k_default_canned[] = {
    "OK",       "Yes",     "No",        "On my way",    "Where are you?",
    "I'm here", "Call me", "Need help", "Heading back", "Ping",
};

/* Keyboard layers. Each string is one row of MESH_UI_KB_COLS cells. */
static const char *const k_kb_layers[MESH_UI_KB_LAYER_COUNT][MESH_UI_KB_CHAR_ROWS] = {
    {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM!-:"},
    {"!@#$%^&*()", "-_=+[]{}<>", ";:'\"/\\|`~", ",.?!@#&%*+"},
};

static char s_canned[MESH_UI_CANNED_MAX][MESH_UI_CANNED_TEXT_MAX];
static size_t s_canned_count;
static bool s_canned_loaded;

static void mesh_ui_canned_defaults(void) {
    s_canned_count = 0U;
    for (size_t i = 0; i < sizeof(k_default_canned) / sizeof(k_default_canned[0]) &&
                       s_canned_count < MESH_UI_CANNED_MAX;
         ++i) {
        snprintf(s_canned[s_canned_count], sizeof s_canned[0], "%s", k_default_canned[i]);
        s_canned_count++;
    }
    s_canned_loaded = true;
}

void mesh_ui_canned_reset(void) { mesh_ui_canned_defaults(); }

size_t mesh_ui_canned_count(void) {
    if (!s_canned_loaded) {
        mesh_ui_canned_defaults();
    }
    return s_canned_count;
}

const char *mesh_ui_canned_text(size_t index) {
    if (!s_canned_loaded) {
        mesh_ui_canned_defaults();
    }
    if (index >= s_canned_count) {
        return "";
    }
    return s_canned[index];
}

int mesh_ui_canned_load(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -errno;
    }

    char staged[MESH_UI_CANNED_MAX][MESH_UI_CANNED_TEXT_MAX];
    size_t count = 0U;
    char line[256];
    while (count < MESH_UI_CANNED_MAX && fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        /* Control bytes would reach the radio and the framebuffer as-is; drop the line. */
        bool clean = true;
        for (const unsigned char *c = (const unsigned char *)line; *c != '\0'; ++c) {
            if (*c < 0x20U) {
                clean = false;
                break;
            }
        }
        if (!clean || line[0] == '\0' || line[0] == '#') {
            continue;
        }
        snprintf(staged[count], sizeof staged[0], "%.*s", (int)(MESH_UI_CANNED_TEXT_MAX - 1U),
                 line);
        count++;
    }
    fclose(file);

    if (count == 0U) {
        return -ENODATA;
    }

    memcpy(s_canned, staged, sizeof s_canned);
    s_canned_count = count;
    s_canned_loaded = true;
    return (int)count;
}

const char *mesh_ui_screen_name(enum mesh_ui_screen screen) {
    switch (screen) {
    case MESH_UI_SCREEN_MESSAGES:
        return "Messages";
    case MESH_UI_SCREEN_NODES:
        return "Nodes";
    case MESH_UI_SCREEN_COMPOSE:
        return "Compose";
    case MESH_UI_SCREEN_DEVICES:
        return "Devices";
    case MESH_UI_SCREEN_STATUS:
        return "Status";
    default:
        return "?";
    }
}

char mesh_ui_kb_char(enum mesh_ui_kb_layer layer, unsigned row, unsigned col) {
    if (layer >= MESH_UI_KB_LAYER_COUNT || row >= MESH_UI_KB_CHAR_ROWS || col >= MESH_UI_KB_COLS) {
        return '\0';
    }
    return k_kb_layers[layer][row][col];
}

const char *mesh_ui_kb_action_label(const struct mesh_ui_nav *nav, enum mesh_ui_kb_action action) {
    switch (action) {
    case MESH_UI_KB_ACTION_LAYER:
        if (nav != NULL && nav->kb_layer == MESH_UI_KB_LOWER) {
            return "ABC";
        }
        if (nav != NULL && nav->kb_layer == MESH_UI_KB_UPPER) {
            return "#+=";
        }
        return "abc";
    case MESH_UI_KB_ACTION_SPACE:
        return "space";
    case MESH_UI_KB_ACTION_DELETE:
        return "del";
    case MESH_UI_KB_ACTION_SEND:
        return "send";
    case MESH_UI_KB_ACTION_CANCEL:
        return "cancel";
    default:
        return "";
    }
}

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

static void mesh_ui_nav_channel_name(const struct mesh_ui_store *store, uint8_t index, char *out,
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

static void mesh_ui_nav_node_name(const struct mesh_ui_store *store, uint32_t node_id, char *out,
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

static void mesh_ui_nav_refresh_target_name(struct mesh_ui_nav *nav,
                                            const struct mesh_ui_store *store,
                                            const char *name_hint) {
    if (nav->target_node == MESH_MESSAGE_BROADCAST_ADDR) {
        mesh_ui_nav_channel_name(store, nav->target_channel, nav->target_name,
                                 sizeof nav->target_name);
        return;
    }
    if (name_hint != NULL && name_hint[0] != '\0') {
        snprintf(nav->target_name, sizeof nav->target_name, "%s", name_hint);
        return;
    }
    mesh_ui_nav_node_name(store, nav->target_node, nav->target_name, sizeof nav->target_name);
}

/* Switching conversation moves the Messages cursor back to the newest line. */
static void mesh_ui_nav_set_target(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   uint32_t node_id, uint8_t channel, const char *name_hint) {
    nav->target_node = node_id;
    nav->target_channel = (node_id == MESH_MESSAGE_BROADCAST_ADDR) ? channel : nav->target_channel;
    nav->inbox = false;
    nav->messages_seen = 0U;
    mesh_ui_nav_refresh_target_name(nav, store, name_hint);
}

void mesh_ui_nav_conversation_name(const struct mesh_ui_nav *nav, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (nav == NULL || nav->inbox) {
        snprintf(out, out_len, "%s", "Inbox");
        return;
    }
    snprintf(out, out_len, "%s", nav->target_name);
}

void mesh_ui_nav_init(struct mesh_ui_nav *nav) {
    if (nav == NULL) {
        return;
    }
    memset(nav, 0, sizeof *nav);
    nav->screen = MESH_UI_SCREEN_MESSAGES;
    nav->target_node = MESH_MESSAGE_BROADCAST_ADDR;
    nav->target_channel = 0U;
    nav->inbox = true; /* nothing hidden until the user picks a conversation */
    snprintf(nav->target_name, sizeof nav->target_name, "%s", "#Primary");
}

/* ---- message filter ----------------------------------------------------------------------- */

static bool mesh_ui_nav_message_matches(const struct mesh_ui_nav *nav,
                                        const struct mesh_ui_message *message) {
    if (nav->inbox) {
        return true;
    }
    if (nav->target_node == MESH_MESSAGE_BROADCAST_ADDR) {
        return message->broadcast && message->channel == nav->target_channel;
    }
    return !message->broadcast && message->peer == nav->target_node;
}

uint32_t mesh_ui_nav_filter_messages(const struct mesh_ui_nav *nav,
                                     const struct mesh_ui_message_list *messages,
                                     uint32_t *out_indices, uint32_t capacity) {
    if (nav == NULL || messages == NULL) {
        return 0U;
    }
    const uint32_t count =
        messages->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : messages->count;
    uint32_t written = 0U;
    for (uint32_t i = 0; i < count; ++i) {
        if (!mesh_ui_nav_message_matches(nav, &messages->entries[i])) {
            continue;
        }
        if (out_indices != NULL && written < capacity) {
            out_indices[written] = i;
        }
        written++;
    }
    return written;
}

/* ---- rows and cursors --------------------------------------------------------------------- */

uint32_t mesh_ui_nav_row_count(const struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                               enum mesh_ui_screen screen) {
    if (store == NULL) {
        return 0U;
    }
    switch (screen) {
    case MESH_UI_SCREEN_MESSAGES:
        return mesh_ui_nav_filter_messages(nav, &store->messages, NULL, 0U);
    case MESH_UI_SCREEN_NODES:
        if (!store->handshake_valid) {
            return 0U;
        }
        return store->handshake.node_count > MESH_UI_MAX_HANDSHAKE_NODES
                   ? MESH_UI_MAX_HANDSHAKE_NODES
                   : store->handshake.node_count;
    case MESH_UI_SCREEN_COMPOSE:
        return MESH_UI_COMPOSE_FIRST_CANNED + (uint32_t)mesh_ui_canned_count();
    case MESH_UI_SCREEN_DEVICES:
        return (uint32_t)store->device_count;
    case MESH_UI_SCREEN_STATUS:
    default:
        return 0U;
    }
}

bool mesh_ui_nav_clamp(struct mesh_ui_nav *nav, const struct mesh_ui_store *store) {
    if (nav == NULL || store == NULL) {
        return false;
    }

    bool moved = false;
    for (int screen = 0; screen < MESH_UI_SCREEN_COUNT; ++screen) {
        const uint32_t rows = mesh_ui_nav_row_count(nav, store, (enum mesh_ui_screen)screen);
        uint32_t *cursor = &nav->cursor[screen];
        if (rows == 0U) {
            if (*cursor != 0U) {
                *cursor = 0U;
                moved = true;
            }
            if (screen == MESH_UI_SCREEN_MESSAGES) {
                nav->messages_seen = 0U;
            }
            continue;
        }

        if (screen == MESH_UI_SCREEN_MESSAGES) {
            /* Newest at the bottom; a cursor sitting on the newest line stays on the newest
               line as traffic arrives. Anywhere else it holds its place. */
            const bool at_tail = (nav->messages_seen == 0U) || (*cursor + 1U >= nav->messages_seen);
            if (at_tail && *cursor != rows - 1U) {
                *cursor = rows - 1U;
                moved = true;
            }
            nav->messages_seen = rows;
        }

        if (*cursor >= rows) {
            *cursor = rows - 1U;
            moved = true;
        }
    }

    /* A stale target name (node renamed, channel list arrived) is refreshed here too. */
    if (nav->target_node == MESH_MESSAGE_BROADCAST_ADDR || store->handshake_valid) {
        char fresh[MESH_UI_NAV_TARGET_NAME_MAX];
        if (nav->target_node == MESH_MESSAGE_BROADCAST_ADDR) {
            mesh_ui_nav_channel_name(store, nav->target_channel, fresh, sizeof fresh);
        } else {
            mesh_ui_nav_node_name(store, nav->target_node, fresh, sizeof fresh);
            if (fresh[0] == '!' && nav->target_name[0] != '!') {
                fresh[0] = '\0'; /* node fell out of the list; keep the name we had */
            }
        }
        if (fresh[0] != '\0' && strcmp(fresh, nav->target_name) != 0) {
            snprintf(nav->target_name, sizeof nav->target_name, "%s", fresh);
            moved = true;
        }
    }
    return moved;
}

/* ---- target cycling ----------------------------------------------------------------------- */

/* Order: every enabled channel by slot, then every node except ourselves, then back around. */
static void mesh_ui_nav_cycle_target(struct mesh_ui_nav *nav, const struct mesh_ui_store *store) {
    const struct mesh_ui_handshake_state *hs = store->handshake_valid ? &store->handshake : NULL;
    const uint32_t node_count =
        (hs != NULL) ? (hs->node_count > MESH_UI_MAX_HANDSHAKE_NODES ? MESH_UI_MAX_HANDSHAKE_NODES
                                                                     : hs->node_count)
                     : 0U;
    const uint32_t me = (hs != NULL && hs->has_my_info) ? hs->my_info.node_num : 0U;

    /* Enabled channel slots, in order; with no channel table known, slot 0 stands in. */
    uint8_t slots[MESH_UI_MAX_CHANNELS];
    uint32_t slot_count = 0U;
    if (hs != NULL) {
        for (uint32_t i = 0; i < hs->channel_count && i < MESH_UI_MAX_CHANNELS; ++i) {
            if (hs->channels[i].role != 0U) {
                slots[slot_count++] = hs->channels[i].index;
            }
        }
    }
    if (slot_count == 0U) {
        slots[slot_count++] = 0U;
    }

    if (nav->target_node == MESH_MESSAGE_BROADCAST_ADDR) {
        uint32_t position = slot_count; /* not found: fall through to the first node */
        for (uint32_t i = 0; i < slot_count; ++i) {
            if (slots[i] == nav->target_channel) {
                position = i + 1U;
                break;
            }
        }
        if (position < slot_count) {
            mesh_ui_nav_set_target(nav, store, MESH_MESSAGE_BROADCAST_ADDR, slots[position], NULL);
            return;
        }
        for (uint32_t i = 0; i < node_count; ++i) {
            const struct mesh_ui_node_summary *node = &hs->nodes[i];
            if (node->node_id == 0U || (me != 0U && node->node_id == me)) {
                continue;
            }
            mesh_ui_nav_set_target(nav, store, node->node_id, 0U, NULL);
            return;
        }
        mesh_ui_nav_set_target(nav, store, MESH_MESSAGE_BROADCAST_ADDR, slots[0], NULL);
        return;
    }

    uint32_t start = node_count; /* unknown node: wrap to the first channel */
    for (uint32_t i = 0; i < node_count; ++i) {
        if (hs->nodes[i].node_id == nav->target_node) {
            start = i + 1U;
            break;
        }
    }
    for (uint32_t i = start; i < node_count; ++i) {
        const struct mesh_ui_node_summary *node = &hs->nodes[i];
        if (node->node_id == 0U || (me != 0U && node->node_id == me)) {
            continue;
        }
        mesh_ui_nav_set_target(nav, store, node->node_id, 0U, NULL);
        return;
    }
    mesh_ui_nav_set_target(nav, store, MESH_MESSAGE_BROADCAST_ADDR, slots[0], NULL);
}

/* X on Messages: Inbox, then each channel, then each node we have direct messages with. */
static void mesh_ui_nav_cycle_conversation(struct mesh_ui_nav *nav,
                                           const struct mesh_ui_store *store) {
    if (nav->inbox) {
        /* Leave the inbox for the first channel. */
        const struct mesh_ui_handshake_state *hs =
            store->handshake_valid ? &store->handshake : NULL;
        uint8_t first = 0U;
        if (hs != NULL) {
            for (uint32_t i = 0; i < hs->channel_count && i < MESH_UI_MAX_CHANNELS; ++i) {
                if (hs->channels[i].role != 0U) {
                    first = hs->channels[i].index;
                    break;
                }
            }
        }
        mesh_ui_nav_set_target(nav, store, MESH_MESSAGE_BROADCAST_ADDR, first, NULL);
        return;
    }

    if (nav->target_node == MESH_MESSAGE_BROADCAST_ADDR) {
        /* Next channel, else the first node with direct traffic, else the inbox. */
        const struct mesh_ui_handshake_state *hs =
            store->handshake_valid ? &store->handshake : NULL;
        if (hs != NULL) {
            bool passed = false;
            for (uint32_t i = 0; i < hs->channel_count && i < MESH_UI_MAX_CHANNELS; ++i) {
                if (hs->channels[i].role == 0U) {
                    continue;
                }
                if (passed) {
                    mesh_ui_nav_set_target(nav, store, MESH_MESSAGE_BROADCAST_ADDR,
                                           hs->channels[i].index, NULL);
                    return;
                }
                if (hs->channels[i].index == nav->target_channel) {
                    passed = true;
                }
            }
        }
        const struct mesh_ui_message_list *messages = &store->messages;
        const uint32_t count =
            messages->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : messages->count;
        for (uint32_t i = 0; i < count; ++i) {
            if (!messages->entries[i].broadcast) {
                mesh_ui_nav_set_target(nav, store, messages->entries[i].peer, 0U,
                                       messages->entries[i].peer_name);
                return;
            }
        }
        nav->inbox = true;
        nav->messages_seen = 0U;
        return;
    }

    /* On a node: the next distinct direct-message peer after this one, else the inbox. */
    const struct mesh_ui_message_list *messages = &store->messages;
    const uint32_t count =
        messages->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : messages->count;
    uint32_t seen[MESH_UI_MAX_MESSAGES];
    uint32_t seen_count = 0U;
    bool passed = false;
    for (uint32_t i = 0; i < count; ++i) {
        const struct mesh_ui_message *message = &messages->entries[i];
        if (message->broadcast) {
            continue;
        }
        bool duplicate = false;
        for (uint32_t j = 0; j < seen_count; ++j) {
            if (seen[j] == message->peer) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        seen[seen_count++] = message->peer;
        if (passed) {
            mesh_ui_nav_set_target(nav, store, message->peer, 0U, message->peer_name);
            return;
        }
        if (message->peer == nav->target_node) {
            passed = true;
        }
    }
    nav->inbox = true;
    nav->messages_seen = 0U;
}

/* ---- keyboard ----------------------------------------------------------------------------- */

static void mesh_ui_nav_draft_append(struct mesh_ui_nav *nav, char ch) {
    const size_t len = strlen(nav->draft);
    if (len + 1U >= sizeof nav->draft) {
        return;
    }
    nav->draft[len] = ch;
    nav->draft[len + 1U] = '\0';
}

static bool mesh_ui_nav_draft_delete(struct mesh_ui_nav *nav) {
    const size_t len = strlen(nav->draft);
    if (len == 0U) {
        return false;
    }
    nav->draft[len - 1U] = '\0';
    return true;
}

/* Closing always parks the cursor at the top-left so the next message starts the same way. */
static void mesh_ui_nav_keyboard_close(struct mesh_ui_nav *nav) {
    nav->keyboard_open = false;
    nav->kb_row = 0U;
    nav->kb_col = 0U;
    nav->kb_layer = MESH_UI_KB_LOWER;
    nav->screen = MESH_UI_SCREEN_COMPOSE;
}

/* Sends the draft as-is; empty drafts are ignored. */
static bool mesh_ui_nav_send_draft(struct mesh_ui_nav *nav, struct mesh_ui_action *action) {
    if (nav->draft[0] == '\0') {
        return false;
    }
    if (action != NULL) {
        action->type = MESH_UI_ACTION_SEND_TEXT;
        action->dest = nav->target_node;
        action->channel = nav->target_channel;
        snprintf(action->text, sizeof action->text, "%s", nav->draft);
    }
    nav->draft[0] = '\0';
    mesh_ui_nav_keyboard_close(nav);
    nav->inbox = false; /* show the conversation the message went to */
    nav->messages_seen = 0U;
    nav->screen = MESH_UI_SCREEN_MESSAGES;
    return true;
}

static bool mesh_ui_nav_keyboard_key(struct mesh_ui_nav *nav, enum mesh_ui_key key,
                                     struct mesh_ui_action *action) {
    switch (key) {
    case MESH_UI_KEY_UP:
    case MESH_UI_KEY_DOWN: {
        const bool was_actions = (nav->kb_row == MESH_UI_KB_CHAR_ROWS);
        if (key == MESH_UI_KEY_UP) {
            nav->kb_row =
                (nav->kb_row == 0U) ? (uint8_t)(MESH_UI_KB_ROWS - 1U) : (uint8_t)(nav->kb_row - 1U);
        } else {
            nav->kb_row = (uint8_t)((nav->kb_row + 1U) % MESH_UI_KB_ROWS);
        }
        /* The action row has five wide keys under ten narrow ones; keep the cursor under
           roughly the same spot when crossing between them. */
        const bool is_actions = (nav->kb_row == MESH_UI_KB_CHAR_ROWS);
        if (!was_actions && is_actions) {
            nav->kb_col = (uint8_t)(nav->kb_col * MESH_UI_KB_ACTIONS / MESH_UI_KB_COLS);
        } else if (was_actions && !is_actions) {
            nav->kb_col = (uint8_t)(nav->kb_col * MESH_UI_KB_COLS / MESH_UI_KB_ACTIONS);
        }
        return true;
    }
    case MESH_UI_KEY_LEFT: {
        const unsigned cols =
            (nav->kb_row == MESH_UI_KB_CHAR_ROWS) ? MESH_UI_KB_ACTIONS : MESH_UI_KB_COLS;
        nav->kb_col = (nav->kb_col == 0U) ? (uint8_t)(cols - 1U) : (uint8_t)(nav->kb_col - 1U);
        return true;
    }
    case MESH_UI_KEY_RIGHT: {
        const unsigned cols =
            (nav->kb_row == MESH_UI_KB_CHAR_ROWS) ? MESH_UI_KB_ACTIONS : MESH_UI_KB_COLS;
        nav->kb_col = (uint8_t)((nav->kb_col + 1U) % cols);
        return true;
    }
    case MESH_UI_KEY_A:
        if (nav->kb_row < MESH_UI_KB_CHAR_ROWS) {
            const char ch =
                mesh_ui_kb_char((enum mesh_ui_kb_layer)nav->kb_layer, nav->kb_row, nav->kb_col);
            if (ch != '\0') {
                mesh_ui_nav_draft_append(nav, ch);
                /* One capital, then back to lower case, like a phone keyboard. */
                if (nav->kb_layer == MESH_UI_KB_UPPER) {
                    nav->kb_layer = MESH_UI_KB_LOWER;
                }
            }
            return true;
        }
        switch ((enum mesh_ui_kb_action)nav->kb_col) {
        case MESH_UI_KB_ACTION_LAYER:
            nav->kb_layer = (uint8_t)((nav->kb_layer + 1U) % MESH_UI_KB_LAYER_COUNT);
            return true;
        case MESH_UI_KB_ACTION_SPACE:
            mesh_ui_nav_draft_append(nav, ' ');
            return true;
        case MESH_UI_KB_ACTION_DELETE:
            return mesh_ui_nav_draft_delete(nav);
        case MESH_UI_KB_ACTION_SEND:
            return mesh_ui_nav_send_draft(nav, action);
        case MESH_UI_KB_ACTION_CANCEL:
            nav->draft[0] = '\0';
            mesh_ui_nav_keyboard_close(nav);
            return true;
        default:
            return false;
        }
    case MESH_UI_KEY_B:
    case MESH_UI_KEY_L1:
        /* Backspace; with nothing left to delete, B closes the keyboard and keeps nothing. */
        if (mesh_ui_nav_draft_delete(nav)) {
            return true;
        }
        if (key == MESH_UI_KEY_B) {
            mesh_ui_nav_keyboard_close(nav);
            return true;
        }
        return false;
    case MESH_UI_KEY_X:
        nav->kb_layer = (uint8_t)((nav->kb_layer + 1U) % MESH_UI_KB_LAYER_COUNT);
        return true;
    case MESH_UI_KEY_Y:
    case MESH_UI_KEY_R1:
        mesh_ui_nav_draft_append(nav, ' ');
        return true;
    case MESH_UI_KEY_START:
        return mesh_ui_nav_send_draft(nav, action);
    case MESH_UI_KEY_SELECT:
    case MESH_UI_KEY_NONE:
    default:
        return false;
    }
}

/* ---- tabs --------------------------------------------------------------------------------- */

static bool mesh_ui_nav_switch_screen(struct mesh_ui_nav *nav, int delta) {
    int next = (int)nav->screen + delta;
    if (next < 0) {
        next = MESH_UI_SCREEN_COUNT - 1;
    } else if (next >= MESH_UI_SCREEN_COUNT) {
        next = 0;
    }
    nav->screen = (enum mesh_ui_screen)next;
    return true;
}

static bool mesh_ui_nav_move_cursor(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                    int delta) {
    const uint32_t rows = mesh_ui_nav_row_count(nav, store, nav->screen);
    if (rows == 0U) {
        return false;
    }
    uint32_t *cursor = &nav->cursor[nav->screen];
    if (delta < 0) {
        if (*cursor == 0U) {
            return false;
        }
        *cursor -= 1U;
        return true;
    }
    if (*cursor + 1U >= rows) {
        return false;
    }
    *cursor += 1U;
    return true;
}

static void mesh_ui_nav_open_compose(struct mesh_ui_nav *nav) {
    nav->screen = MESH_UI_SCREEN_COMPOSE;
    nav->cursor[MESH_UI_SCREEN_COMPOSE] =
        nav->draft[0] != '\0' ? MESH_UI_COMPOSE_ROW_DRAFT : MESH_UI_COMPOSE_FIRST_CANNED;
}

static bool mesh_ui_nav_confirm(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                struct mesh_ui_action *action) {
    const uint32_t rows = mesh_ui_nav_row_count(nav, store, nav->screen);
    const uint32_t cursor = nav->cursor[nav->screen];

    switch (nav->screen) {
    case MESH_UI_SCREEN_MESSAGES: {
        if (cursor >= rows) {
            return false;
        }
        uint32_t indices[MESH_UI_MAX_MESSAGES];
        const uint32_t count =
            mesh_ui_nav_filter_messages(nav, &store->messages, indices, MESH_UI_MAX_MESSAGES);
        if (cursor >= count) {
            return false;
        }
        const struct mesh_ui_message *message = &store->messages.entries[indices[cursor]];
        /* Reply: a broadcast goes back to its channel, a direct message back to the peer. */
        if (message->broadcast) {
            mesh_ui_nav_set_target(nav, store, MESH_MESSAGE_BROADCAST_ADDR, message->channel, NULL);
        } else {
            mesh_ui_nav_set_target(nav, store, message->peer, 0U, message->peer_name);
        }
        mesh_ui_nav_open_compose(nav);
        return true;
    }
    case MESH_UI_SCREEN_NODES: {
        if (cursor >= rows) {
            return false;
        }
        const struct mesh_ui_node_summary *node = &store->handshake.nodes[cursor];
        if (node->node_id == 0U) {
            return false;
        }
        if (store->handshake.has_my_info && node->node_id == store->handshake.my_info.node_num) {
            return false; /* messaging yourself is not a thing */
        }
        mesh_ui_nav_set_target(nav, store, node->node_id, 0U, NULL);
        mesh_ui_nav_open_compose(nav);
        return true;
    }
    case MESH_UI_SCREEN_COMPOSE: {
        if (cursor == MESH_UI_COMPOSE_ROW_TARGET) {
            mesh_ui_nav_cycle_target(nav, store);
            return true;
        }
        if (cursor == MESH_UI_COMPOSE_ROW_DRAFT) {
            nav->keyboard_open = true;
            return true;
        }
        const size_t canned_index = cursor - MESH_UI_COMPOSE_FIRST_CANNED;
        if (canned_index >= mesh_ui_canned_count()) {
            return false;
        }
        if (action != NULL) {
            action->type = MESH_UI_ACTION_SEND_TEXT;
            action->dest = nav->target_node;
            action->channel = nav->target_channel;
            snprintf(action->text, sizeof action->text, "%s", mesh_ui_canned_text(canned_index));
        }
        /* Show the conversation it went to; the app's toast reports the outcome. */
        nav->inbox = false;
        nav->messages_seen = 0U;
        nav->screen = MESH_UI_SCREEN_MESSAGES;
        return true;
    }
    case MESH_UI_SCREEN_DEVICES: {
        if (cursor >= rows) {
            return false;
        }
        const struct mesh_ui_device *device = &store->devices[cursor];
        if (device->connected || device->identifier[0] == '\0') {
            return false;
        }
        if (action != NULL) {
            action->type = MESH_UI_ACTION_CONNECT;
            snprintf(action->identifier, sizeof action->identifier, "%s", device->identifier);
        }
        return false;
    }
    case MESH_UI_SCREEN_STATUS:
    default:
        return false;
    }
}

bool mesh_ui_nav_handle_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                            enum mesh_ui_key key, struct mesh_ui_action *out_action) {
    if (out_action != NULL) {
        memset(out_action, 0, sizeof *out_action);
    }
    if (nav == NULL || store == NULL) {
        return false;
    }

    /* Any press dismisses a notice; whether the frame changes is decided below. */
    bool changed = false;
    if (nav->toast[0] != '\0') {
        nav->toast[0] = '\0';
        nav->toast_until_ms = 0U;
        changed = true;
    }

    if (nav->keyboard_open) {
        return mesh_ui_nav_keyboard_key(nav, key, out_action) || changed;
    }

    switch (key) {
    case MESH_UI_KEY_LEFT:
    case MESH_UI_KEY_L1:
        return mesh_ui_nav_switch_screen(nav, -1) || changed;
    case MESH_UI_KEY_RIGHT:
    case MESH_UI_KEY_R1:
        return mesh_ui_nav_switch_screen(nav, +1) || changed;
    case MESH_UI_KEY_UP:
        return mesh_ui_nav_move_cursor(nav, store, -1) || changed;
    case MESH_UI_KEY_DOWN:
        return mesh_ui_nav_move_cursor(nav, store, +1) || changed;
    case MESH_UI_KEY_A:
    case MESH_UI_KEY_START:
        return mesh_ui_nav_confirm(nav, store, out_action) || changed;
    case MESH_UI_KEY_B:
        /* Back out of Compose to the conversation; elsewhere B is a no-op so a stray press
           never drops the user somewhere unexpected. */
        if (nav->screen == MESH_UI_SCREEN_COMPOSE) {
            nav->screen = MESH_UI_SCREEN_MESSAGES;
            return true;
        }
        return changed;
    case MESH_UI_KEY_X:
        if (nav->screen == MESH_UI_SCREEN_MESSAGES) {
            mesh_ui_nav_cycle_conversation(nav, store);
            return true;
        }
        return changed;
    case MESH_UI_KEY_Y:
        /* Compose to the current conversation from anywhere it makes sense. */
        if (nav->screen == MESH_UI_SCREEN_MESSAGES || nav->screen == MESH_UI_SCREEN_NODES) {
            mesh_ui_nav_open_compose(nav);
            return true;
        }
        return changed;
    case MESH_UI_KEY_SELECT:
    case MESH_UI_KEY_NONE:
    default:
        return changed;
    }
}

void mesh_ui_nav_set_toast(struct mesh_ui_nav *nav, uint64_t now_ms, const char *text) {
    if (nav == NULL) {
        return;
    }
    if (text == NULL || text[0] == '\0') {
        nav->toast[0] = '\0';
        nav->toast_until_ms = 0U;
        return;
    }
    snprintf(nav->toast, sizeof nav->toast, "%s", text);
    nav->toast_until_ms = now_ms + 4000U;
}

bool mesh_ui_nav_tick(struct mesh_ui_nav *nav, uint64_t now_ms) {
    if (nav == NULL || nav->toast[0] == '\0' || now_ms < nav->toast_until_ms) {
        return false;
    }
    nav->toast[0] = '\0';
    nav->toast_until_ms = 0U;
    return true;
}
