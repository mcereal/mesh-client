#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/nav.h"

#include "mesh/ui/node_detail.h"
#include "mesh/ui/settings.h"

#include "mesh/mesh_message.h"
#include "mesh/ui/store.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

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
    case MESH_UI_SCREEN_DEVICES:
        return "Devices";
    case MESH_UI_SCREEN_STATUS:
        return "Status";
    case MESH_UI_SCREEN_SETTINGS:
        return "Settings";
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
        return (nav != NULL && nav->keyboard_field != MESH_UI_FIELD_NONE) ? "done" : "send";
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

/* Opening a thread is the only thing that moves the target, and it always parks the
   conversation list's cursor so B can put it back. */
static void mesh_ui_nav_open_thread(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                    uint32_t node_id, uint8_t channel, const char *name_hint) {
    if (!nav->thread_open) {
        nav->conversation_list_cursor = nav->cursor[MESH_UI_SCREEN_MESSAGES];
    }
    mesh_ui_nav_set_target(nav, store, node_id, channel, name_hint);
    nav->thread_open = true;
    nav->screen = MESH_UI_SCREEN_MESSAGES;
    nav->cursor[MESH_UI_SCREEN_MESSAGES] = 0U;
}

static void mesh_ui_nav_open_all_traffic(struct mesh_ui_nav *nav) {
    if (!nav->thread_open) {
        nav->conversation_list_cursor = nav->cursor[MESH_UI_SCREEN_MESSAGES];
    }
    nav->inbox = true;
    nav->thread_open = true;
    nav->messages_seen = 0U;
    nav->screen = MESH_UI_SCREEN_MESSAGES;
    nav->cursor[MESH_UI_SCREEN_MESSAGES] = 0U;
}

/* The compose overlay always writes to the open thread, so it needs no target of its own. */
static void mesh_ui_nav_open_compose(struct mesh_ui_nav *nav) {
    nav->compose_open = true;
    nav->compose_cursor =
        nav->draft[0] != '\0' ? MESH_UI_COMPOSE_ROW_DRAFT : MESH_UI_COMPOSE_FIRST_CANNED;
}

/* B out of a thread. Returns false when the conversation list is already showing. */
static bool mesh_ui_nav_close_thread(struct mesh_ui_nav *nav) {
    if (!nav->thread_open) {
        return false;
    }
    nav->thread_open = false;
    nav->inbox = false;
    nav->messages_seen = 0U;
    nav->cursor[MESH_UI_SCREEN_MESSAGES] = nav->conversation_list_cursor;
    return true;
}

/* B out of a node's detail. Returns false when the node list is already showing. */
static bool mesh_ui_nav_close_node_detail(struct mesh_ui_nav *nav) {
    if (!nav->node_detail_open) {
        return false;
    }
    nav->node_detail_open = false;
    nav->node_detail_node = 0U;
    nav->cursor[MESH_UI_SCREEN_NODES] = nav->node_list_cursor;
    return true;
}

void mesh_ui_nav_conversation_name(const struct mesh_ui_nav *nav, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (nav == NULL) {
        snprintf(out, out_len, "%s", "Messages");
        return;
    }
    if (!nav->thread_open) {
        snprintf(out, out_len, "%s", "Messages");
        return;
    }
    if (nav->inbox) {
        snprintf(out, out_len, "%s", "All traffic");
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
    nav->thread_open = false; /* land on the conversation list, the way a phone does */
    nav->inbox = false;
    snprintf(nav->target_name, sizeof nav->target_name, "%s", "#Primary");
    nav->settings_section = MESH_UI_SETTINGS_NO_SECTION;
    nav->settings_channel = MESH_UI_SETTINGS_NO_CHANNEL;
}

/* ---- message filter ----------------------------------------------------------------------- */

/* Which messages belong to the thread the nav has open. Only meaningful while thread_open;
   the conversation list below matches on its own terms. */
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

/* Our own node, which cannot be messaged and whose SNR and hop count mean nothing. */
static bool mesh_ui_nav_node_is_self(const struct mesh_ui_store *store,
                                     const struct mesh_ui_node_summary *node) {
    return node != NULL && store->handshake.has_my_info &&
           node->node_id == store->handshake.my_info.node_num;
}

/* Asks the app for the opposite of the node's current pin state. The state is sent rather than
   a bare "toggle" so a press that races an incoming NodeInfo cannot cancel itself out. */
static void mesh_ui_nav_fill_favorite(struct mesh_ui_action *action,
                                      const struct mesh_ui_node_summary *node) {
    if (action == NULL || node == NULL) {
        return;
    }
    action->type = MESH_UI_ACTION_TOGGLE_FAVORITE;
    action->dest = node->node_id;
    action->number = node->is_favorite ? 0U : 1U;
}

/* ---- rows and cursors --------------------------------------------------------------------- */

uint32_t mesh_ui_nav_row_count(const struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                               enum mesh_ui_screen screen) {
    if (store == NULL) {
        return 0U;
    }
    switch (screen) {
    case MESH_UI_SCREEN_MESSAGES:
        if (nav == NULL || !nav->thread_open) {
            return mesh_ui_nav_conversation_count(store);
        }
        return mesh_ui_nav_filter_messages(nav, &store->messages, NULL, 0U);
    case MESH_UI_SCREEN_NODES: {
        if (!store->handshake_valid) {
            return 0U;
        }
        const uint32_t nodes = store->handshake.node_count > MESH_UI_MAX_HANDSHAKE_NODES
                                   ? MESH_UI_MAX_HANDSHAKE_NODES
                                   : store->handshake.node_count;
        if (nav == NULL || !nav->node_detail_open) {
            return nodes;
        }
        const struct mesh_ui_node_summary *node =
            mesh_ui_node_detail_find(&store->handshake, nav->node_detail_node);
        return mesh_ui_node_detail_count(node, mesh_ui_nav_node_is_self(store, node));
    }
    case MESH_UI_SCREEN_DEVICES:
        return (uint32_t)store->device_count;
    case MESH_UI_SCREEN_SETTINGS:
        if (nav->settings_section == MESH_UI_SETTINGS_NO_SECTION) {
            return MESH_UI_SETTINGS_SECTION_COUNT;
        }
        return mesh_ui_settings_item_count(
            &store->settings, store->handshake_valid ? &store->handshake : NULL,
            (enum mesh_ui_settings_section)nav->settings_section, nav->settings_channel);
    case MESH_UI_SCREEN_STATUS:
    default:
        return 0U;
    }
}

uint32_t mesh_ui_nav_compose_row_count(void) {
    return MESH_UI_COMPOSE_FIRST_CANNED + (uint32_t)mesh_ui_canned_count();
}

bool mesh_ui_nav_clamp(struct mesh_ui_nav *nav, const struct mesh_ui_store *store) {
    if (nav == NULL || store == NULL) {
        return false;
    }

    bool moved = false;

    /* A node can fall out of the list while its detail is open: the cache holds 256 and the UI
       carries 128 of them, re-ranked every publish. Back out rather than draw an empty screen.
       This runs before the cursor clamp so the restored list position is clamped with it. */
    if (nav->node_detail_open &&
        mesh_ui_node_detail_find(&store->handshake, nav->node_detail_node) == NULL) {
        mesh_ui_nav_close_node_detail(nav);
        moved = true;
    }

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

        if (screen == MESH_UI_SCREEN_MESSAGES && nav->thread_open) {
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

    /* The parked conversation-list position, so backing out of a thread lands on a real row
       even when the list shrank while it was open. */
    if (nav->thread_open) {
        const uint32_t conversations = mesh_ui_nav_conversation_count(store);
        if (nav->conversation_list_cursor >= conversations) {
            nav->conversation_list_cursor = conversations > 0U ? conversations - 1U : 0U;
        }
    }

    /* The compose overlay's own cursor: the canned list is replaceable at runtime. */
    const uint32_t compose_rows = mesh_ui_nav_compose_row_count();
    if (nav->compose_cursor >= compose_rows) {
        nav->compose_cursor = compose_rows > 0U ? compose_rows - 1U : 0U;
        moved = moved || nav->compose_open;
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

static void mesh_ui_nav_picker_open(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
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

static bool mesh_ui_nav_picker_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
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
        snprintf(conversation->preview, sizeof conversation->preview, "%.*s",
                 (int)(sizeof conversation->preview - 1U), message->text);
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
static bool mesh_ui_nav_open_conversation(struct mesh_ui_nav *nav,
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
/* ---- keyboard ----------------------------------------------------------------------------- */

/* The most bytes the draft may hold: the message limit, or the field's cap when the keyboard
   is editing a setting. */
static size_t mesh_ui_nav_draft_cap(const struct mesh_ui_nav *nav) {
    if (nav->keyboard_field != MESH_UI_FIELD_NONE) {
        const uint32_t cap =
            mesh_ui_settings_text_max((enum mesh_ui_setting_field)nav->keyboard_field);
        return cap < MESH_UI_DRAFT_MAX - 1U ? cap : MESH_UI_DRAFT_MAX - 1U;
    }
    return MESH_UI_DRAFT_MAX - 1U;
}

static void mesh_ui_nav_draft_append(struct mesh_ui_nav *nav, char ch) {
    const size_t len = strlen(nav->draft);
    if (len + 1U >= sizeof nav->draft || len >= mesh_ui_nav_draft_cap(nav)) {
        return;
    }
    nav->draft[len] = ch;
    nav->draft[len + 1U] = '\0';
}

/* Removes one character. A name preloaded from the radio may hold UTF-8 the keyboard cannot
   type; deleting byte-wise would leave a broken sequence behind. */
static bool mesh_ui_nav_draft_delete(struct mesh_ui_nav *nav) {
    size_t len = strlen(nav->draft);
    if (len == 0U) {
        return false;
    }
    while (len > 1U && ((unsigned char)nav->draft[len - 1U] & 0xC0U) == 0x80U) {
        len--;
    }
    nav->draft[len - 1U] = '\0';
    return true;
}

/* Closing always parks the cursor at the top-left so the next message starts the same way.
   A keyboard opened for a setting returns to that section and restores the Compose draft; one
   opened for a message falls back to the compose overlay it was opened from. */
static void mesh_ui_nav_keyboard_close(struct mesh_ui_nav *nav) {
    nav->keyboard_open = false;
    nav->kb_row = 0U;
    nav->kb_col = 0U;
    nav->kb_layer = MESH_UI_KB_LOWER;
    if (nav->keyboard_field != MESH_UI_FIELD_NONE) {
        nav->keyboard_field = MESH_UI_FIELD_NONE;
        snprintf(nav->draft, sizeof nav->draft, "%s", nav->draft_saved);
        nav->draft_saved[0] = '\0';
        nav->screen = MESH_UI_SCREEN_SETTINGS;
    }
}

/* ---- settings edits ----------------------------------------------------------------------- */

static const struct mesh_ui_handshake_state *
mesh_ui_nav_handshake(const struct mesh_ui_store *store) {
    return store->handshake_valid ? &store->handshake : NULL;
}

/* The row under the cursor in the open section, with (or without) the pending edits. */
static bool mesh_ui_nav_settings_current(const struct mesh_ui_nav *nav,
                                         const struct mesh_ui_store *store, bool with_edits,
                                         struct mesh_ui_settings_item *out) {
    if (nav->screen != MESH_UI_SCREEN_SETTINGS ||
        nav->settings_section == MESH_UI_SETTINGS_NO_SECTION) {
        return false;
    }
    return mesh_ui_settings_item(&store->settings, mesh_ui_nav_handshake(store),
                                 with_edits ? nav->settings_edits : NULL,
                                 with_edits ? nav->settings_edit_count : 0U,
                                 (enum mesh_ui_settings_section)nav->settings_section,
                                 nav->settings_channel, nav->cursor[MESH_UI_SCREEN_SETTINGS], out);
}

static void mesh_ui_nav_edit_remove(struct mesh_ui_nav *nav, enum mesh_ui_setting_field field) {
    for (uint8_t i = 0; i < nav->settings_edit_count; ++i) {
        if (nav->settings_edits[i].field != (uint8_t)field) {
            continue;
        }
        for (uint8_t j = i; j + 1U < nav->settings_edit_count; ++j) {
            nav->settings_edits[j] = nav->settings_edits[j + 1U];
        }
        nav->settings_edit_count--;
        memset(&nav->settings_edits[nav->settings_edit_count], 0, sizeof nav->settings_edits[0]);
        return;
    }
}

/* Records an edit for the row under the cursor; an edit that puts the radio's own value back
   is dropped instead, so toggling something twice leaves the section clean. */
static bool mesh_ui_nav_edit_set(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                 enum mesh_ui_setting_field field, uint32_t number,
                                 const char *text) {
    struct mesh_ui_settings_item base;
    if (!mesh_ui_nav_settings_current(nav, store, false, &base) || base.field != field) {
        return false;
    }
    bool same;
    if (base.kind == MESH_UI_SETTING_TEXT) {
        same = (text != NULL && strcmp(base.text, text) == 0);
    } else if (base.kind == MESH_UI_SETTING_KEY) {
        /* Keeping the key, or typing the very key the radio has, is no edit. */
        same = (number == MESH_UI_PSK_KEEP);
        if (number == MESH_UI_PSK_TYPED && text != NULL) {
            uint8_t current[MESH_UI_PSK_MAX];
            uint8_t typed[MESH_UI_PSK_MAX];
            size_t current_len = 0U;
            size_t typed_len = 0U;
            same = mesh_ui_settings_key_parse(base.text, current, sizeof current, &current_len) &&
                   mesh_ui_settings_key_parse(text, typed, sizeof typed, &typed_len) &&
                   current_len == typed_len && memcmp(current, typed, typed_len) == 0;
        }
    } else {
        same = (base.number == number);
    }
    if (same) {
        mesh_ui_nav_edit_remove(nav, field);
        return true;
    }
    struct mesh_ui_setting_edit *slot = NULL;
    for (uint8_t i = 0; i < nav->settings_edit_count; ++i) {
        if (nav->settings_edits[i].field == (uint8_t)field) {
            slot = &nav->settings_edits[i];
            break;
        }
    }
    if (slot == NULL) {
        if (nav->settings_edit_count >= MESH_UI_SETTINGS_EDITS_MAX) {
            return false;
        }
        slot = &nav->settings_edits[nav->settings_edit_count++];
    }
    memset(slot, 0, sizeof *slot);
    slot->field = (uint8_t)field;
    slot->number = number;
    if (text != NULL) {
        snprintf(slot->text, sizeof slot->text, "%s", text);
    }
    return true;
}

static void mesh_ui_nav_edits_clear(struct mesh_ui_nav *nav) {
    memset(nav->settings_edits, 0, sizeof nav->settings_edits);
    nav->settings_edit_count = 0U;
    nav->settings_discard_armed = false;
}

/* Opens the keyboard on a field's text. The Compose draft is parked until done or cancel. */
static void mesh_ui_nav_open_field_keyboard(struct mesh_ui_nav *nav,
                                            const struct mesh_ui_settings_item *item) {
    snprintf(nav->draft_saved, sizeof nav->draft_saved, "%s", nav->draft);
    snprintf(nav->draft, sizeof nav->draft, "%s", item->text);
    nav->keyboard_field = (uint8_t)item->field;
    nav->keyboard_open = true;
    nav->kb_row = 0U;
    nav->kb_col = 0U;
    nav->kb_layer = MESH_UI_KB_LOWER;
}

/* A, Left or Right on a row of an open section. Toggles flip, enums cycle, numbers step
   through their presets, text opens the keyboard. Read-only rows ignore the press. */
static bool mesh_ui_nav_settings_edit_key(struct mesh_ui_nav *nav,
                                          const struct mesh_ui_store *store, enum mesh_ui_key key) {
    struct mesh_ui_settings_item item;
    if (!mesh_ui_nav_settings_current(nav, store, true, &item) ||
        item.field == MESH_UI_FIELD_NONE) {
        return false;
    }
    const enum mesh_ui_setting_field field = item.field;
    const int delta = (key == MESH_UI_KEY_LEFT) ? -1 : +1;
    switch (item.kind) {
    case MESH_UI_SETTING_TOGGLE:
        return mesh_ui_nav_edit_set(nav, store, field, item.number != 0U ? 0U : 1U, NULL);
    case MESH_UI_SETTING_ENUM: {
        const uint32_t count = mesh_ui_settings_enum_count(field);
        if (count == 0U) {
            return false;
        }
        const uint32_t next =
            (item.number + count + (uint32_t)(delta < 0 ? count - 1U : 1U)) % count;
        return mesh_ui_nav_edit_set(nav, store, field, next, NULL);
    }
    case MESH_UI_SETTING_NUMBER: {
        const uint32_t next = mesh_ui_settings_number_step(field, item.number, delta);
        if (next == item.number) {
            return false;
        }
        return mesh_ui_nav_edit_set(nav, store, field, next, NULL);
    }
    case MESH_UI_SETTING_KEY:
        if (key == MESH_UI_KEY_A) {
            mesh_ui_nav_open_field_keyboard(nav, &item); /* the key as hex */
            return true;
        }
        {
            /* Left/Right walk the choices the field offers; a typed key counts as "keep". */
            const uint32_t allowed = mesh_ui_settings_key_choices(field);
            uint32_t choice = item.number >= MESH_UI_PSK_TYPED ? 0U : item.number;
            for (unsigned step = 0; step < (unsigned)MESH_UI_PSK_TYPED; ++step) {
                choice = (choice + (uint32_t)MESH_UI_PSK_TYPED +
                          (uint32_t)(delta < 0 ? (unsigned)MESH_UI_PSK_TYPED - 1U : 1U)) %
                         (uint32_t)MESH_UI_PSK_TYPED;
                if ((allowed & MESH_UI_PSK_CHOICE_BIT(choice)) != 0U) {
                    break;
                }
            }
            return mesh_ui_nav_edit_set(nav, store, field, choice, NULL);
        }
    case MESH_UI_SETTING_TEXT:
        if (key != MESH_UI_KEY_A) {
            return false;
        }
        mesh_ui_nav_open_field_keyboard(nav, &item);
        return true;
    default:
        return false;
    }
}

/* Done on the keyboard while it edits a setting: the draft becomes the pending edit. */
static bool mesh_ui_nav_settings_commit_text(struct mesh_ui_nav *nav,
                                             const struct mesh_ui_store *store) {
    const enum mesh_ui_setting_field field = (enum mesh_ui_setting_field)nav->keyboard_field;
    char text[MESH_UI_SETTING_TEXT_MAX];
    snprintf(text, sizeof text, "%.*s", (int)(sizeof text - 1U), nav->draft);
    size_t cap = mesh_ui_settings_text_max(field);
    if (cap >= sizeof text) {
        cap = sizeof text - 1U;
    }
    if (strlen(text) > cap) {
        /* Never cut a UTF-8 sequence in half. */
        while (cap > 0U && ((unsigned char)text[cap] & 0xC0U) == 0x80U) {
            cap--;
        }
        text[cap] = '\0';
    }
    if (mesh_ui_settings_field_kind(field) == MESH_UI_SETTING_KEY) {
        uint8_t parsed[MESH_UI_PSK_MAX];
        size_t parsed_len = 0U;
        if (!mesh_ui_settings_key_parse(text, parsed, sizeof parsed, &parsed_len) ||
            !mesh_ui_settings_key_len_ok(field, parsed_len)) {
            return true; /* not a key this field takes: stay on the keyboard so it can be fixed */
        }
        mesh_ui_nav_edit_set(nav, store, field, MESH_UI_PSK_TYPED, text);
    } else {
        mesh_ui_nav_edit_set(nav, store, field, 0U, text);
    }
    mesh_ui_nav_keyboard_close(nav);
    return true;
}

/* Y with edits: emit the save, or ask first for sections that can cut us off. */
static void mesh_ui_nav_fill_save(const struct mesh_ui_nav *nav, struct mesh_ui_action *action) {
    if (action == NULL) {
        return;
    }
    action->type = MESH_UI_ACTION_SAVE_SETTINGS;
    action->section = nav->settings_section;
    action->channel = nav->settings_channel;
    action->edit_count = nav->settings_edit_count;
    memcpy(action->edits, nav->settings_edits, sizeof action->edits);
}

static bool mesh_ui_nav_confirm_key(struct mesh_ui_nav *nav, enum mesh_ui_key key,
                                    struct mesh_ui_action *action) {
    switch (key) {
    case MESH_UI_KEY_UP:
    case MESH_UI_KEY_DOWN:
    case MESH_UI_KEY_LEFT:
    case MESH_UI_KEY_RIGHT:
        nav->confirm_cursor = nav->confirm_cursor == 0U ? 1U : 0U;
        return true;
    case MESH_UI_KEY_A:
    case MESH_UI_KEY_START:
        if (nav->confirm_cursor == 0U) {
            mesh_ui_nav_fill_save(nav, action);
        }
        nav->confirm_open = false;
        return true;
    case MESH_UI_KEY_B:
        nav->confirm_open = false;
        return true;
    default:
        return false;
    }
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
    /* Land back in the thread it went to, with the compose overlay out of the way. */
    nav->compose_open = false;
    nav->screen = MESH_UI_SCREEN_MESSAGES;
    return true;
}

static bool mesh_ui_nav_keyboard_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                     enum mesh_ui_key key, struct mesh_ui_action *action) {
    const bool for_setting = (nav->keyboard_field != MESH_UI_FIELD_NONE);
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
            return for_setting ? mesh_ui_nav_settings_commit_text(nav, store)
                               : mesh_ui_nav_send_draft(nav, action);
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
        return for_setting ? mesh_ui_nav_settings_commit_text(nav, store)
                           : mesh_ui_nav_send_draft(nav, action);
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

/* ---- compose overlay ---------------------------------------------------------------------- */

/* Sends one canned reply to the open thread. */
static bool mesh_ui_nav_send_canned(struct mesh_ui_nav *nav, struct mesh_ui_action *action,
                                    size_t index) {
    if (index >= mesh_ui_canned_count()) {
        return false;
    }
    if (action != NULL) {
        action->type = MESH_UI_ACTION_SEND_TEXT;
        action->dest = nav->target_node;
        action->channel = nav->target_channel;
        snprintf(action->text, sizeof action->text, "%s", mesh_ui_canned_text(index));
    }
    /* Back to the thread it went to; the app's toast reports the outcome. */
    nav->compose_open = false;
    return true;
}

static bool mesh_ui_nav_compose_key(struct mesh_ui_nav *nav, enum mesh_ui_key key,
                                    struct mesh_ui_action *action) {
    const uint32_t rows = mesh_ui_nav_compose_row_count();
    if (nav->compose_cursor >= rows && rows > 0U) {
        nav->compose_cursor = rows - 1U;
    }
    switch (key) {
    case MESH_UI_KEY_UP:
        if (nav->compose_cursor == 0U) {
            return false;
        }
        nav->compose_cursor--;
        return true;
    case MESH_UI_KEY_DOWN:
        if (nav->compose_cursor + 1U >= rows) {
            return false;
        }
        nav->compose_cursor++;
        return true;
    case MESH_UI_KEY_A:
    case MESH_UI_KEY_START:
        if (nav->compose_cursor == MESH_UI_COMPOSE_ROW_DRAFT) {
            nav->keyboard_open = true;
            return true;
        }
        return mesh_ui_nav_send_canned(nav, action,
                                       nav->compose_cursor - MESH_UI_COMPOSE_FIRST_CANNED);
    case MESH_UI_KEY_Y:
        /* Y opened this; a second press types, which is what the row it lands on offers. */
        nav->compose_cursor = MESH_UI_COMPOSE_ROW_DRAFT;
        nav->keyboard_open = true;
        return true;
    case MESH_UI_KEY_B:
        nav->compose_open = false;
        return true;
    default:
        return false;
    }
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
        if (!nav->thread_open) {
            return mesh_ui_nav_open_conversation(nav, store, cursor, false);
        }
        uint32_t indices[MESH_UI_MAX_MESSAGES];
        const uint32_t count =
            mesh_ui_nav_filter_messages(nav, &store->messages, indices, MESH_UI_MAX_MESSAGES);
        if (cursor >= count) {
            return false;
        }
        const struct mesh_ui_message *message = &store->messages.entries[indices[cursor]];
        if (nav->inbox) {
            /* All traffic is a view over several conversations: A drills into the one this
               line belongs to rather than guessing a destination. */
            if (message->broadcast) {
                mesh_ui_nav_open_thread(nav, store, MESH_MESSAGE_BROADCAST_ADDR, message->channel,
                                        NULL);
            } else {
                mesh_ui_nav_open_thread(nav, store, message->peer, 0U, message->peer_name);
            }
            return true;
        }
        /* Inside a conversation there is only one thing A can mean: reply here. */
        mesh_ui_nav_open_compose(nav);
        return true;
    }
    case MESH_UI_SCREEN_NODES: {
        if (cursor >= rows) {
            return false;
        }
        if (!nav->node_detail_open) {
            /* A on a contact opens what we know about it, the way tapping one in the phone
               app does. Writing to it is the first row inside, and Y still goes straight
               there from the list. */
            const struct mesh_ui_node_summary *node = &store->handshake.nodes[cursor];
            if (node->node_id == 0U) {
                return false;
            }
            nav->node_list_cursor = cursor;
            nav->node_detail_node = node->node_id;
            nav->node_detail_open = true;
            nav->cursor[MESH_UI_SCREEN_NODES] = 0U;
            return true;
        }
        const struct mesh_ui_node_summary *node =
            mesh_ui_node_detail_find(&store->handshake, nav->node_detail_node);
        if (node == NULL) {
            return false;
        }
        struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
        const uint32_t count = mesh_ui_node_detail_build(
            node, mesh_ui_nav_node_is_self(store, node), 0U, items, MESH_UI_NODE_ITEMS_MAX);
        if (cursor >= count || items[cursor].kind != MESH_UI_NODE_ROW_ACTION) {
            return false;
        }
        if (items[cursor].action == MESH_UI_NODE_ACTION_MESSAGE) {
            mesh_ui_nav_open_thread(nav, store, node->node_id, 0U, NULL);
            return true;
        }
        if (items[cursor].action == MESH_UI_NODE_ACTION_FAVORITE) {
            mesh_ui_nav_fill_favorite(action, node);
            return false; /* the row redraws when the app flips the flag */
        }
        return false;
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
            action->kind = device->kind;
            snprintf(action->identifier, sizeof action->identifier, "%s", device->identifier);
        }
        return false;
    }
    case MESH_UI_SCREEN_SETTINGS: {
        if (nav->settings_section == MESH_UI_SETTINGS_CHANNELS &&
            nav->settings_channel == MESH_UI_SETTINGS_NO_CHANNEL) {
            /* A on a channel row opens that slot, when the radio's full table is held. */
            const int slot = mesh_ui_settings_channel_at_row(&store->settings,
                                                             mesh_ui_nav_handshake(store), cursor);
            if (slot < 0) {
                return false;
            }
            nav->settings_channel_list_cursor = cursor;
            nav->settings_channel = (uint8_t)slot;
            nav->cursor[MESH_UI_SCREEN_SETTINGS] = 0U;
            return true;
        }
        if (nav->settings_section != MESH_UI_SETTINGS_NO_SECTION) {
            return mesh_ui_nav_settings_edit_key(nav, store, MESH_UI_KEY_A);
        }
        if (cursor >= MESH_UI_SETTINGS_SECTION_COUNT) {
            return false;
        }
        nav->settings_list_cursor = cursor;
        nav->settings_section = (uint8_t)cursor;
        nav->cursor[MESH_UI_SCREEN_SETTINGS] = 0U;
        return true;
    }
    case MESH_UI_SCREEN_STATUS:
    default:
        return false;
    }
}

static bool mesh_ui_nav_settings_back(struct mesh_ui_nav *nav) {
    if (nav->settings_section == MESH_UI_SETTINGS_NO_SECTION) {
        return false;
    }
    mesh_ui_nav_edits_clear(nav);
    if (nav->settings_channel != MESH_UI_SETTINGS_NO_CHANNEL) {
        nav->settings_channel = MESH_UI_SETTINGS_NO_CHANNEL;
        nav->cursor[MESH_UI_SCREEN_SETTINGS] = nav->settings_channel_list_cursor;
        return true;
    }
    nav->settings_section = MESH_UI_SETTINGS_NO_SECTION;
    nav->cursor[MESH_UI_SCREEN_SETTINGS] = nav->settings_list_cursor;
    return true;
}

/* Keys that mean something different while a Settings section is open: Left/Right edit the
   row instead of switching tabs (L1/R1 still do), Y saves, B asks before discarding edits.
   Returns false to let the ordinary handling run. */
static bool mesh_ui_nav_settings_section_key(struct mesh_ui_nav *nav,
                                             const struct mesh_ui_store *store,
                                             enum mesh_ui_key key, struct mesh_ui_action *action,
                                             bool *handled) {
    *handled = true;
    switch (key) {
    case MESH_UI_KEY_LEFT:
    case MESH_UI_KEY_RIGHT:
        return mesh_ui_nav_settings_edit_key(nav, store, key);
    case MESH_UI_KEY_Y:
        if (nav->settings_edit_count == 0U) {
            return false;
        }
        if (mesh_ui_settings_section_needs_confirm(
                (enum mesh_ui_settings_section)nav->settings_section)) {
            nav->confirm_open = true;
            nav->confirm_cursor = 1U; /* Cancel, so a repeated press changes nothing */
            return true;
        }
        mesh_ui_nav_fill_save(nav, action);
        return false; /* the app clears the edits once the write is queued */
    case MESH_UI_KEY_B:
        if (nav->settings_edit_count > 0U && !nav->settings_discard_armed) {
            nav->settings_discard_armed = true; /* the footer now says "B again to discard" */
            return true;
        }
        return mesh_ui_nav_settings_back(nav);
    default:
        *handled = false;
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

    if (nav->confirm_open) {
        return mesh_ui_nav_confirm_key(nav, key, out_action) || changed;
    }
    if (nav->picker_open) {
        return mesh_ui_nav_picker_key(nav, store, key) || changed;
    }
    if (nav->keyboard_open) {
        return mesh_ui_nav_keyboard_key(nav, store, key, out_action) || changed;
    }
    if (nav->compose_open) {
        return mesh_ui_nav_compose_key(nav, key, out_action) || changed;
    }

    if (nav->screen == MESH_UI_SCREEN_SETTINGS &&
        nav->settings_section != MESH_UI_SETTINGS_NO_SECTION) {
        /* A second press of anything but B stands the discard question down. */
        const bool was_armed = nav->settings_discard_armed;
        if (key != MESH_UI_KEY_B) {
            nav->settings_discard_armed = false;
            changed = changed || was_armed;
        }
        bool handled = false;
        const bool result = mesh_ui_nav_settings_section_key(nav, store, key, out_action, &handled);
        if (handled) {
            return result || changed;
        }
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
        /* Back out of a thread to the conversation list; elsewhere B is a no-op so a stray
           press never drops the user somewhere unexpected. */
        if (nav->screen == MESH_UI_SCREEN_MESSAGES) {
            return mesh_ui_nav_close_thread(nav) || changed;
        }
        if (nav->screen == MESH_UI_SCREEN_SETTINGS) {
            return mesh_ui_nav_settings_back(nav) || changed;
        }
        if (nav->screen == MESH_UI_SCREEN_NODES) {
            return mesh_ui_nav_close_node_detail(nav) || changed;
        }
        return changed;
    case MESH_UI_KEY_X:
        if (nav->screen == MESH_UI_SCREEN_NODES) {
            /* The one-press version of the detail's "Pinned to top" row, from either level -
               pinning a node you can see in the list should not cost a drill-down. */
            const struct mesh_ui_node_summary *node =
                nav->node_detail_open
                    ? mesh_ui_node_detail_find(&store->handshake, nav->node_detail_node)
                    : mesh_ui_node_detail_at(&store->handshake, nav->cursor[nav->screen]);
            if (node != NULL && node->node_id != 0U && !mesh_ui_nav_node_is_self(store, node)) {
                mesh_ui_nav_fill_favorite(out_action, node);
            }
            return changed;
        }
        if (nav->screen == MESH_UI_SCREEN_SETTINGS) {
            if (out_action != NULL) {
                out_action->type = MESH_UI_ACTION_REFRESH_SETTINGS;
                /* A refresh keeps pending edits, so the app can say how many are still
                   waiting. X sits next to Y on the same screen and the two are easy to
                   confuse; a refresh that reports nothing reads like a save that did
                   nothing. The edits themselves are not needed, only the count. */
                out_action->edit_count = nav->settings_edit_count;
            }
            return changed;
        }
        return changed;
    case MESH_UI_KEY_Y:
        if (nav->screen == MESH_UI_SCREEN_MESSAGES) {
            /* In a conversation, write to it. On the list (or in the all-traffic view, which
               has no single destination), pick who to write to first. */
            if (nav->thread_open && !nav->inbox) {
                mesh_ui_nav_open_compose(nav);
            } else {
                mesh_ui_nav_picker_open(nav, store, true);
            }
            return true;
        }
        if (nav->screen == MESH_UI_SCREEN_NODES) {
            /* Straight from a contact into writing to it, from the list or from inside the
               node's detail. Which cursor names the node depends on which level is showing. */
            const struct mesh_ui_node_summary *node =
                nav->node_detail_open
                    ? mesh_ui_node_detail_find(&store->handshake, nav->node_detail_node)
                    : mesh_ui_node_detail_at(&store->handshake, nav->cursor[nav->screen]);
            if (node == NULL || node->node_id == 0U || mesh_ui_nav_node_is_self(store, node)) {
                return changed;
            }
            mesh_ui_nav_open_thread(nav, store, node->node_id, 0U, NULL);
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
