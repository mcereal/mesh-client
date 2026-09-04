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
    "OK",        "Yes",       "No",           "On my way", "Where are you?", "I'm here",
    "Call me",   "Need help", "Heading back", "Ping",
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
        snprintf(staged[count], sizeof staged[0], "%.*s", (int)(MESH_UI_CANNED_TEXT_MAX - 1U), line);
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

void mesh_ui_nav_init(struct mesh_ui_nav *nav) {
    if (nav == NULL) {
        return;
    }
    memset(nav, 0, sizeof *nav);
    nav->screen = MESH_UI_SCREEN_MESSAGES;
    nav->target_node = MESH_MESSAGE_BROADCAST_ADDR;
    snprintf(nav->target_name, sizeof nav->target_name, "%s", "All");
}

uint32_t mesh_ui_nav_row_count(const struct mesh_ui_store *store, enum mesh_ui_screen screen) {
    if (store == NULL) {
        return 0U;
    }
    switch (screen) {
    case MESH_UI_SCREEN_MESSAGES:
        return store->messages.count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES
                                                            : store->messages.count;
    case MESH_UI_SCREEN_NODES:
        if (!store->handshake_valid) {
            return 0U;
        }
        return store->handshake.node_count > MESH_UI_MAX_HANDSHAKE_NODES
                   ? MESH_UI_MAX_HANDSHAKE_NODES
                   : store->handshake.node_count;
    case MESH_UI_SCREEN_COMPOSE:
        return 1U + (uint32_t)mesh_ui_canned_count();
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
        const uint32_t rows = mesh_ui_nav_row_count(store, (enum mesh_ui_screen)screen);
        uint32_t *cursor = &nav->cursor[screen];
        if (rows == 0U) {
            if (*cursor != 0U) {
                *cursor = 0U;
                moved = true;
            }
            continue;
        }

        if (screen == MESH_UI_SCREEN_MESSAGES) {
            /* Newest at the bottom; a cursor sitting on the newest line stays on the newest
               line as traffic arrives. Anywhere else it holds its place. */
            const bool at_tail = (nav->messages_seen == 0U) ||
                                 (nav->messages_seen > 0U && *cursor + 1U >= nav->messages_seen);
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
    return moved;
}

static void mesh_ui_nav_name_for_node(const struct mesh_ui_store *store, uint32_t node_id,
                                      char *out, size_t out_len) {
    if (node_id == MESH_MESSAGE_BROADCAST_ADDR) {
        snprintf(out, out_len, "%s", "All");
        return;
    }
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

static void mesh_ui_nav_set_target(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                   uint32_t node_id, const char *name_hint) {
    nav->target_node = node_id;
    if (name_hint != NULL && name_hint[0] != '\0' && node_id != MESH_MESSAGE_BROADCAST_ADDR) {
        snprintf(nav->target_name, sizeof nav->target_name, "%s", name_hint);
        return;
    }
    mesh_ui_nav_name_for_node(store, node_id, nav->target_name, sizeof nav->target_name);
}

/* Compose "To:" row: All, then every node we know except ourselves, then back to All. */
static void mesh_ui_nav_cycle_target(struct mesh_ui_nav *nav, const struct mesh_ui_store *store) {
    const struct mesh_ui_handshake_state *hs = store->handshake_valid ? &store->handshake : NULL;
    const uint32_t count = (hs != NULL) ? (hs->node_count > MESH_UI_MAX_HANDSHAKE_NODES
                                               ? MESH_UI_MAX_HANDSHAKE_NODES
                                               : hs->node_count)
                                        : 0U;
    const uint32_t me = (hs != NULL && hs->has_my_info) ? hs->my_info.node_num : 0U;

    uint32_t start = 0U; /* index after the current target; 0 means "start from the top" */
    if (nav->target_node != MESH_MESSAGE_BROADCAST_ADDR) {
        for (uint32_t i = 0; i < count; ++i) {
            if (hs->nodes[i].node_id == nav->target_node) {
                start = i + 1U;
                break;
            }
        }
        if (start == 0U) {
            /* Target is no longer in the list; wrap to All. */
            mesh_ui_nav_set_target(nav, store, MESH_MESSAGE_BROADCAST_ADDR, NULL);
            return;
        }
    }

    for (uint32_t i = start; i < count; ++i) {
        const struct mesh_ui_node_summary *node = &hs->nodes[i];
        if (node->node_id == 0U || (me != 0U && node->node_id == me)) {
            continue;
        }
        mesh_ui_nav_set_target(nav, store, node->node_id, NULL);
        return;
    }
    mesh_ui_nav_set_target(nav, store, MESH_MESSAGE_BROADCAST_ADDR, NULL);
}

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
    const uint32_t rows = mesh_ui_nav_row_count(store, nav->screen);
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

static bool mesh_ui_nav_confirm(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                struct mesh_ui_action *action) {
    const uint32_t rows = mesh_ui_nav_row_count(store, nav->screen);
    const uint32_t cursor = nav->cursor[nav->screen];

    switch (nav->screen) {
    case MESH_UI_SCREEN_MESSAGES: {
        if (cursor >= rows) {
            return false;
        }
        const struct mesh_ui_message *message = &store->messages.entries[cursor];
        /* Reply: a broadcast goes back to the channel, a direct message back to its sender. */
        const uint32_t dest = message->broadcast ? MESH_MESSAGE_BROADCAST_ADDR : message->peer;
        mesh_ui_nav_set_target(nav, store, dest, message->peer_name);
        nav->screen = MESH_UI_SCREEN_COMPOSE;
        nav->cursor[MESH_UI_SCREEN_COMPOSE] = 1U; /* land on the first reply, not the To: row */
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
        mesh_ui_nav_set_target(nav, store, node->node_id, NULL);
        nav->screen = MESH_UI_SCREEN_COMPOSE;
        nav->cursor[MESH_UI_SCREEN_COMPOSE] = 1U;
        return true;
    }
    case MESH_UI_SCREEN_COMPOSE: {
        if (cursor == 0U) {
            mesh_ui_nav_cycle_target(nav, store);
            return true;
        }
        const size_t canned_index = cursor - 1U;
        if (canned_index >= mesh_ui_canned_count()) {
            return false;
        }
        if (action != NULL) {
            action->type = MESH_UI_ACTION_SEND_TEXT;
            action->dest = nav->target_node;
            action->channel = 0U;
            snprintf(action->text, sizeof action->text, "%s", mesh_ui_canned_text(canned_index));
        }
        return false; /* the app's toast reports the outcome; nothing visible changes here */
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
    case MESH_UI_KEY_Y:
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
