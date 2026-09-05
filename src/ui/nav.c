#define _POSIX_C_SOURCE 200809L

/*
 * Screen routing: which screen is up, where the cursor is, and who gets the next key.
 *
 * What is left here after the split is the router. mesh_ui_nav_handle_key() offers each key to
 * whatever overlay is up - keyboard, picker, confirm sheet - before the screen underneath sees
 * it, and mesh_ui_nav_confirm() is the A button's switch across every screen. The subjects those
 * two dispatch into are the neighbouring nav_*.c files, through nav_internal.h.
 */

#include "nav_internal.h"

#include "mesh/core/message.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/settings.h"
#include "mesh/utils/array.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

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
void mesh_ui_nav_open_thread(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                             uint32_t node_id, uint8_t channel, const char *name_hint) {
    if (!nav->thread_open) {
        nav->conversation_list_cursor = nav->cursor[MESH_UI_SCREEN_MESSAGES];
    }
    mesh_ui_nav_set_target(nav, store, node_id, channel, name_hint);
    nav->thread_open = true;
    nav->screen = MESH_UI_SCREEN_MESSAGES;
    nav->cursor[MESH_UI_SCREEN_MESSAGES] = 0U;
}

void mesh_ui_nav_open_all_traffic(struct mesh_ui_nav *nav) {
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
void mesh_ui_nav_open_compose(struct mesh_ui_nav *nav) {
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
    nav->node_remove_armed = false;
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
        return mesh_ui_node_detail_count(node, mesh_ui_nav_node_is_self(store, node),
                                         &store->traceroute);
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
            node, mesh_ui_nav_node_is_self(store, node), 0U, &store->traceroute,
            nav->node_remove_armed, items, MESH_UI_NODE_ITEMS_MAX);
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
        if (items[cursor].action == MESH_UI_NODE_ACTION_TRACEROUTE) {
            if (action != NULL) {
                action->type = MESH_UI_ACTION_TRACEROUTE;
                action->dest = node->node_id;
            }
            return false; /* the rows redraw when the app publishes the trace */
        }
        if (items[cursor].action == MESH_UI_NODE_ACTION_REQUEST_INFO) {
            if (action != NULL) {
                action->type = MESH_UI_ACTION_REQUEST_NODE_INFO;
                action->dest = node->node_id;
            }
            return false; /* the row redraws if and when the node answers */
        }
        if (items[cursor].action == MESH_UI_NODE_ACTION_IGNORE) {
            if (action != NULL) {
                /* The wanted state, not a bare "toggle": a press that races an incoming
                   NodeInfo must not cancel itself out, same as the pin. */
                action->type = MESH_UI_ACTION_TOGGLE_IGNORE;
                action->dest = node->node_id;
                action->number = node->is_ignored ? 0U : 1U;
            }
            return false; /* the row redraws when the app flips the flag */
        }
        if (items[cursor].action == MESH_UI_NODE_ACTION_MUTE) {
            if (action != NULL) {
                /* No wanted state to send: toggle_muted_node is all the firmware offers. */
                action->type = MESH_UI_ACTION_TOGGLE_MUTE;
                action->dest = node->node_id;
            }
            return false; /* the row redraws when the app flips the flag */
        }
        if (items[cursor].action == MESH_UI_NODE_ACTION_REMOVE) {
            if (!nav->node_remove_armed) {
                nav->node_remove_armed = true; /* the row now says "A again to remove" */
                return true;
            }
            nav->node_remove_armed = false;
            if (action != NULL) {
                action->type = MESH_UI_ACTION_REMOVE_NODE;
                action->dest = node->node_id;
            }
            /* The detail closes on the next clamp, when the node is gone from the list. */
            return false;
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
            /* An ACTION row asks the app to do something rather than editing a value, so it
               is answered here where out_action is in hand. It carries what it does in
               `number`, which keeps the nav from needing to know what any section means. */
            struct mesh_ui_settings_item item;
            if (mesh_ui_nav_settings_current(nav, store, true, &item) &&
                item.kind == MESH_UI_SETTING_ACTION && item.field == MESH_UI_FIELD_NONE) {
                /* A destructive radio action is never done on the press that selected it: the
                   row opens the question, and the answer to that is what goes out. The rest go
                   straight through. */
                const enum mesh_ui_settings_action which =
                    (enum mesh_ui_settings_action)item.number;
                if (mesh_ui_settings_action_needs_confirm(which)) {
                    nav->confirm_open = true;
                    nav->confirm_cursor = 1U; /* Cancel, so a repeated press changes nothing */
                    nav->confirm_action = (uint8_t)which;
                    return true;
                }
                if (mesh_ui_settings_action_is_radio(which)) {
                    mesh_ui_nav_fill_radio_action(nav, which, action);
                    return false; /* the rows redraw when the read-back lands */
                }
                if (action != NULL) {
                    if (item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CHECK_UPDATE) {
                        action->type = MESH_UI_ACTION_CHECK_UPDATE;
                    } else if (item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_INSTALL_UPDATE) {
                        action->type = MESH_UI_ACTION_INSTALL_UPDATE;
                    } else if (item.number ==
                               (uint32_t)MESH_UI_SETTINGS_ACTION_CYCLE_UPDATE_CHANNEL) {
                        action->type = MESH_UI_ACTION_CYCLE_UPDATE_CHANNEL;
                    } else if (item.number ==
                               (uint32_t)MESH_UI_SETTINGS_ACTION_TOGGLE_DEV_UPDATES) {
                        action->type = MESH_UI_ACTION_TOGGLE_DEV_UPDATES;
                    }
                }
                /* The row itself does not change; the app's reply comes back as new state. */
                return false;
            }
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

    /* One press arms Y on the Devices tab; anything else stands it back down. */
    if (nav->devices_forget_armed &&
        (key != MESH_UI_KEY_Y || nav->screen != MESH_UI_SCREEN_DEVICES)) {
        nav->devices_forget_armed = false;
        changed = true;
    }
    /* The same for the node detail's remove row, which only A on that row may re-arm. Moving
       the cursor off it is enough to stand it down, so the arming cannot outlive the row the
       user was looking at. */
    if (nav->node_remove_armed &&
        (key != MESH_UI_KEY_A || nav->screen != MESH_UI_SCREEN_NODES || !nav->node_detail_open)) {
        nav->node_remove_armed = false;
        changed = true;
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
        if (nav->screen == MESH_UI_SCREEN_DEVICES) {
            /* Only one radio is ever connected, so this does not depend on the row: it drops
               the link that is up (or the one coming up), which is what stops auto-connect
               taking the radio straight back. */
            const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_DEVICES];
            if (out_action != NULL) {
                out_action->type = MESH_UI_ACTION_DISCONNECT;
                if (cursor < store->device_count &&
                    (store->devices[cursor].connected || store->devices[cursor].busy)) {
                    snprintf(out_action->identifier, sizeof out_action->identifier, "%s",
                             store->devices[cursor].identifier);
                    out_action->kind = store->devices[cursor].kind;
                }
            }
            return changed;
        }
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
        if (nav->screen == MESH_UI_SCREEN_DEVICES) {
            /* Forgetting a bond costs a re-pair with the node's PIN, so the first press only
               arms it and the backends say so. A press on any other row re-arms from there. */
            const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_DEVICES];
            if (cursor >= store->device_count ||
                store->devices[cursor].kind != (uint8_t)MESH_UI_DEVICE_BLE) {
                return changed; /* a USB port has no bond to forget */
            }
            if (!nav->devices_forget_armed || nav->devices_forget_row != cursor) {
                nav->devices_forget_armed = true;
                nav->devices_forget_row = cursor;
                return true;
            }
            nav->devices_forget_armed = false;
            if (out_action != NULL) {
                out_action->type = MESH_UI_ACTION_FORGET;
                out_action->kind = store->devices[cursor].kind;
                snprintf(out_action->identifier, sizeof out_action->identifier, "%s",
                         store->devices[cursor].identifier);
            }
            return true;
        }
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

bool mesh_ui_nav_open_passkey(struct mesh_ui_nav *nav, const char *label, uint32_t passkey,
                              bool confirm) {
    if (nav == NULL) {
        return false;
    }
    if (nav->keyboard_passkey) {
        return false; /* already up for this pairing */
    }

    /* Whatever the keyboard was doing is parked, not lost: the prompt arrives in the middle of
       whatever the user was typing and BlueZ will not wait for them to finish. That includes a
       keyboard that is already open - its text and its target both come back on close. */
    snprintf(nav->draft_saved, sizeof nav->draft_saved, "%s", nav->draft);
    nav->keyboard_field_displaced = nav->keyboard_open ? nav->keyboard_field : MESH_UI_FIELD_NONE;
    nav->keyboard_field = MESH_UI_FIELD_NONE;
    nav->keyboard_passkey = true;
    nav->pairing_confirm = confirm;
    snprintf(nav->pairing_label, sizeof nav->pairing_label, "%s", label != NULL ? label : "node");
    /* A numeric comparison is answered by pressing Send on the number BlueZ handed us; a PIN
       is typed, so it starts empty. */
    if (confirm) {
        snprintf(nav->draft, sizeof nav->draft, "%06u", (unsigned)passkey);
    } else {
        nav->draft[0] = '\0';
    }
    nav->keyboard_open = true;
    nav->kb_row = 0U; /* the digit row */
    nav->kb_col = 0U;
    nav->kb_layer = MESH_UI_KB_LOWER;
    return true;
}

bool mesh_ui_nav_close_passkey(struct mesh_ui_nav *nav) {
    if (nav == NULL || !nav->keyboard_passkey) {
        return false;
    }
    nav->draft[0] = '\0';
    mesh_ui_nav_keyboard_close(nav);
    return true;
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
