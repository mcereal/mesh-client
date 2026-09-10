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
#include "mesh/ui/help.h"
#include "mesh/ui/history.h"
#include "mesh/ui/map.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/reactions.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/status.h"
#include "mesh/utils/array.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

const char *mesh_ui_screen_name(enum mesh_ui_screen screen) {
    switch (screen) {
    case MESH_UI_SCREEN_MESSAGES:
        return mesh_str(MESH_STR_TAB_MESSAGES);
    case MESH_UI_SCREEN_NODES:
        return mesh_str(MESH_STR_TAB_NODES);
    case MESH_UI_SCREEN_WAYPOINTS:
        return mesh_str(MESH_STR_TAB_WAYPOINTS);
    case MESH_UI_SCREEN_DEVICES:
        return mesh_str(MESH_STR_TAB_DEVICES);
    case MESH_UI_SCREEN_STATUS:
        return mesh_str(MESH_STR_TAB_STATUS);
    case MESH_UI_SCREEN_SETTINGS:
        return mesh_str(MESH_STR_TAB_SETTINGS);
    default:
        return mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT);
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
    /* A reply names a packet id, and a packet id from the conversation we just left is not a
       message in this one. */
    nav->reply_to = 0U;
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
    nav->reply_to = 0U;
}

/* The compose overlay always writes to the open thread, so it needs no target of its own. */
void mesh_ui_nav_open_compose(struct mesh_ui_nav *nav) {
    nav->compose_open = true;
    nav->compose_cursor =
        nav->draft[0] != '\0' ? MESH_UI_COMPOSE_ROW_DRAFT : MESH_UI_COMPOSE_FIRST_CANNED;
}

/* Y's half of the split: typing, with nothing in between. Leaving `compose_open` clear is the
   whole difference - cancelling the keyboard then lands on the thread rather than on the
   canned list the user never asked for. The draft survives, so Y resumes an unsent one. */
void mesh_ui_nav_open_keyboard(struct mesh_ui_nav *nav) {
    nav->compose_open = false;
    /* Y is "write", not "answer this": the bar says so, and the thread is scrolled to wherever
       the user was reading rather than to whatever they mean to write about. */
    nav->reply_to = 0U;
    nav->keyboard_open = true;
    nav->kb_row = 0U;
    nav->kb_col = 0U;
    nav->kb_layer = MESH_UI_KB_LOWER;
}

/* B out of a thread. Returns false when the conversation list is already showing. */
static bool mesh_ui_nav_close_thread(struct mesh_ui_nav *nav) {
    if (!nav->thread_open) {
        return false;
    }
    nav->thread_open = false;
    nav->inbox = false;
    nav->messages_seen = 0U;
    nav->reply_to = 0U;
    nav->reaction_open = false;
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

/*
 * The node a Nodes-list row is about, or NULL when the row is not about a node.
 *
 * One place that knows the list has a map row on the front of it, so the four presses the list
 * offers - A, X, Y and the detail's own opening - cannot disagree about which node row 3 is.
 * Every one of them went through mesh_ui_node_detail_at() with the raw cursor before the row
 * existed, and every one of them would have been off by one after it.
 */
static const struct mesh_ui_node_summary *mesh_ui_nav_node_at_row(const struct mesh_ui_store *store,
                                                                  uint32_t cursor) {
    if (store == NULL || cursor == MESH_UI_NODES_MAP_ROW) {
        return NULL;
    }
    return mesh_ui_node_detail_at(&store->handshake, cursor - 1U);
}

void mesh_ui_nav_conversation_name(const struct mesh_ui_nav *nav, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (nav == NULL || !nav->thread_open) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_TAB_MESSAGES));
        return;
    }
    if (nav->inbox) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_MESSAGES_ALL_TRAFFIC));
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
    snprintf(nav->target_name, sizeof nav->target_name, "%s", mesh_str(MESH_STR_CHANNEL_PRIMARY));
    nav->settings_section = MESH_UI_SETTINGS_NO_SECTION;
    nav->settings_parent = MESH_UI_SETTINGS_NO_SECTION;
    nav->settings_channel = MESH_UI_SETTINGS_NO_CHANNEL;
}

/* ---- message filter ----------------------------------------------------------------------- */

/* Which messages belong to the thread the nav has open. Only meaningful while thread_open;
   the conversation list below matches on its own terms. */
static bool mesh_ui_nav_message_matches(const struct mesh_ui_nav *nav,
                                        const struct mesh_ui_message *message) {
    /*
     * A reaction is an annotation on another message, not a line of its own. The transcript
     * draws it on the bubble it names (fb_thread_row_build), so it must not also appear as a
     * bubble containing a bare emoji - which is exactly what it looked like before the emoji
     * flag was read at all, and is what a phone app never shows.
     *
     * This is the one filter every thread goes through, including all-traffic, which is why it
     * sits here rather than in each caller.
     */
    if (message->is_reaction) {
        return false;
    }
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

/*
 * The verbs the Status cards offer, from the store.
 *
 * mesh_ui_status_actions() takes the two facts as booleans rather than either of the two
 * parallel structs, because it is asked the same question from three places holding different
 * ones - here with a store, and from actions.c and the renderer with a snapshot.
 */
static void mesh_ui_nav_status_actions(const struct mesh_ui_store *store,
                                       struct mesh_ui_status_actions *out) {
    bool connected = false;
    for (size_t i = 0; i < store->device_count; ++i) {
        connected = connected || store->devices[i].connected;
    }
    mesh_ui_status_actions(out, connected, store->handshake_valid,
                           mesh_ui_history_has_airtime(&store->history));
}

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
        if (nav != NULL && nav->map_open && !nav->node_detail_open) {
            /* The map has no rows. The d-pad moves the world here rather than a cursor, so
               there is nothing for the cursor clamp to hold in range - see nav_map.c. */
            return 0U;
        }
        if (nav == NULL || !nav->node_detail_open) {
            /* The map row, and then the nodes. A roster with nothing in it draws an empty
               state instead of a list, so the row that opens a map of it is not offered
               either: the screen it would open is the same nothing one level in. */
            return nodes > 0U ? nodes + 1U : 0U;
        }
        const struct mesh_ui_node_summary *node =
            mesh_ui_node_detail_find(&store->handshake, nav->node_detail_node);
        return mesh_ui_node_detail_count(node, mesh_ui_nav_node_is_self(store, node),
                                         &store->traceroute, &store->handshake);
    }
    case MESH_UI_SCREEN_WAYPOINTS:
        return mesh_ui_nav_waypoint_row_count(nav, store);
    case MESH_UI_SCREEN_DEVICES:
        return (uint32_t)store->device_count;
    case MESH_UI_SCREEN_SETTINGS:
        if (nav->settings_section == MESH_UI_SETTINGS_NO_SECTION) {
            return mesh_ui_settings_root_count();
        }
        return mesh_ui_settings_item_count(
            &store->settings, store->handshake_valid ? &store->handshake : NULL,
            (enum mesh_ui_settings_section)nav->settings_section, nav->settings_channel);
    case MESH_UI_SCREEN_STATUS:
    default: {
        /* Status has no list. Its "rows" are the verbs its cards offer, walked as one flat
           set - a card is focused because the cursor is on one of its buttons, and a card with
           no verb is stepped over. See include/mesh/ui/status.h for why the list is flat. */
        struct mesh_ui_status_actions actions;
        mesh_ui_nav_status_actions(store, &actions);
        return actions.count;
    }
    }
}

uint32_t mesh_ui_nav_compose_row_count(void) {
    return MESH_UI_COMPOSE_FIRST_CANNED + (uint32_t)mesh_ui_canned_count();
}

/*
 * How many rows the help screen has: one per paragraph.
 *
 * A "row" here is an entry rather than a line. The backend measures each paragraph into however
 * many lines it wraps to and scrolls in those, exactly as the transcript does - but what the
 * cursor walks is the entries, because landing between two halves of a sentence is not a place
 * anybody meant to be.
 */
static uint32_t mesh_ui_nav_help_row_count(const struct mesh_ui_nav *nav,
                                           const struct mesh_ui_store *store) {
    struct mesh_ui_help_topic topic;
    if (!mesh_ui_help_topic(&store->settings, mesh_ui_nav_handshake(store), nav, &topic)) {
        return 0U;
    }
    return topic.count;
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
    /* And the same for a place that has left the list, which is what a withdrawal from the
       mesh looks like from here. */
    moved = mesh_ui_nav_waypoint_clamp(nav, store) || moved;
    /* And for a map with nothing left to draw on it, which a forget or a radio swap can leave
       behind. It runs after the node detail's close so a map closing under an open detail takes
       the detail with it rather than stranding it one level up from nowhere. */
    moved = mesh_ui_nav_map_clamp(nav, store) || moved;
    /*
     * And a chart with nothing left to draw, which is the map's clamp one screen along: a radio
     * swap empties the history the way it empties the roster, and a picture of a reading nobody
     * is holding any more is the empty-graticule trap with fewer clues in it. An empty frame
     * with its axes still labelled looks like a mesh that went perfectly quiet.
     */
    if (nav->trend_open && !mesh_ui_history_has_airtime(&store->history)) {
        nav->trend_open = false;
        moved = true;
    }

    /*
     * Help, whose paragraph list can shrink under an open screen: a radio answering for a
     * section it had not sent yet adds rows, and a link dropping takes them away again, so the
     * count this cursor was placed against is not the count the next frame draws.
     *
     * It closes rather than clamps when the topic goes entirely, because a help screen with
     * nothing to explain is not a screen - and the section under it is still there to land on.
     */
    if (nav->help_open) {
        const uint32_t entries = mesh_ui_nav_help_row_count(nav, store);
        if (entries == 0U) {
            nav->help_open = false;
            nav->help_cursor = 0U;
            moved = true;
        } else if (nav->help_cursor >= entries) {
            nav->help_cursor = entries - 1U;
            moved = true;
        }
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
        action->reply_id = nav->reply_to;
        snprintf(action->text, sizeof action->text, "%s", mesh_ui_canned_text(index));
    }
    /* Back to the thread it went to; the app's toast reports the outcome. */
    nav->compose_open = false;
    nav->reply_to = 0U;
    return true;
}

/* ---- the tapback picker -------------------------------------------------------------------- */

uint32_t mesh_ui_nav_reaction_row_count(void) { return (uint32_t)mesh_ui_reaction_count(); }

/* X on a bubble: the emoji list, aimed at that message and at nothing else. */
static bool mesh_ui_nav_open_reactions(struct mesh_ui_nav *nav, uint32_t packet_id) {
    if (packet_id == 0U || mesh_ui_reaction_count() == 0U) {
        return false; /* a message the radio never gave an id has nothing to react to */
    }
    nav->reply_to = packet_id;
    nav->reaction_open = true;
    nav->reaction_cursor = 0U;
    return true;
}

/* Sends one tapback about `reply_to`. */
static bool mesh_ui_nav_send_reaction(struct mesh_ui_nav *nav, struct mesh_ui_action *action,
                                      size_t index) {
    if (index >= mesh_ui_reaction_count() || nav->reply_to == 0U) {
        return false;
    }
    if (action != NULL) {
        action->type = MESH_UI_ACTION_SEND_TEXT;
        action->dest = nav->target_node;
        action->channel = nav->target_channel;
        action->reply_id = nav->reply_to;
        action->is_reaction = true;
        snprintf(action->text, sizeof action->text, "%s", mesh_ui_reaction_emoji(index));
    }
    nav->reaction_open = false;
    nav->reply_to = 0U;
    return true;
}

static bool mesh_ui_nav_reaction_key(struct mesh_ui_nav *nav, enum mesh_ui_key key,
                                     struct mesh_ui_action *action) {
    const uint32_t rows = mesh_ui_nav_reaction_row_count();
    if (nav->reaction_cursor >= rows && rows > 0U) {
        nav->reaction_cursor = rows - 1U;
    }
    switch (key) {
    case MESH_UI_KEY_UP:
        if (nav->reaction_cursor == 0U) {
            return false;
        }
        nav->reaction_cursor--;
        return true;
    case MESH_UI_KEY_DOWN:
        if (nav->reaction_cursor + 1U >= rows) {
            return false;
        }
        nav->reaction_cursor++;
        return true;
    case MESH_UI_KEY_A:
    case MESH_UI_KEY_START:
        return mesh_ui_nav_send_reaction(nav, action, nav->reaction_cursor);
    case MESH_UI_KEY_B:
        /* B, and only B - the compose sheet is opened by A and closed by B, and an overlay
           that also answered the key that raised it would be the one place in this UI where
           backing out is two different presses. */
        nav->reaction_open = false;
        nav->reply_to = 0U;
        return true;
    default:
        return false;
    }
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
        nav->reply_to = 0U;
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
            return mesh_ui_nav_open_conversation(nav, store, cursor);
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
        /*
         * Inside a conversation A is the quick reply: the canned list, one press from sent.
         * Typing one out is Y's job.
         *
         * It answers the bubble under the cursor rather than the conversation, which is what
         * the bar has always said it does. A message whose id we never learned - one restored
         * from the cache before ids were kept - still sends, as an ordinary message: refusing
         * the press because the target has no id would make a reply a thing that sometimes
         * does not happen.
         */
        nav->reply_to = message->packet_id;
        mesh_ui_nav_open_compose(nav);
        return true;
    }
    case MESH_UI_SCREEN_NODES: {
        if (cursor >= rows) {
            return false;
        }
        if (!nav->node_detail_open) {
            if (cursor == MESH_UI_NODES_MAP_ROW) {
                /*
                 * The map, framed on everything the client can place. It cannot be pressed when
                 * there is nothing to place, and it says so out loud rather than doing nothing:
                 * a row that swallows a press is the client telling the reader their Brick is
                 * broken. The Waypoints tab's "New waypoint here" row settled this rule.
                 */
                if (!mesh_ui_map_has_markers(store)) {
                    mesh_ui_nav_raise_toast(nav, mesh_str(MESH_STR_TOAST_MAP_NO_FIXES));
                    return true; /* the toast is nav state, so the frame has changed */
                }
                mesh_ui_nav_open_map(nav, store, 0U);
                return true;
            }
            /* A on a contact opens what we know about it, the way tapping one in the phone
               app does. Writing to it is the first row inside, and Y still goes straight
               there from the list. */
            const struct mesh_ui_node_summary *node = mesh_ui_nav_node_at_row(store, cursor);
            if (node == NULL || node->node_id == 0U) {
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
        const uint32_t count =
            mesh_ui_node_detail_build(node, mesh_ui_nav_node_is_self(store, node), 0U,
                                      &store->traceroute, nav->node_remove_armed, &store->handshake,
                                      &store->history, items, MESH_UI_NODE_ITEMS_MAX);
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
        if (items[cursor].action == MESH_UI_NODE_ACTION_REQUEST_INFO ||
            items[cursor].action == MESH_UI_NODE_ACTION_REQUEST_POSITION ||
            items[cursor].action == MESH_UI_NODE_ACTION_REQUEST_TELEMETRY) {
            if (action != NULL) {
                action->type = items[cursor].action == MESH_UI_NODE_ACTION_REQUEST_POSITION
                                   ? MESH_UI_ACTION_REQUEST_POSITION
                                   : (items[cursor].action == MESH_UI_NODE_ACTION_REQUEST_TELEMETRY
                                          ? MESH_UI_ACTION_REQUEST_TELEMETRY
                                          : MESH_UI_ACTION_REQUEST_NODE_INFO);
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
        if (items[cursor].action == MESH_UI_NODE_ACTION_SHOW_ON_MAP) {
            /*
             * The map, aimed at this node, opened *under* the detail rather than over it: the
             * detail closes and the map is what is left, so B from the map goes on to the node
             * list. Leaving the detail open over its own map would make B land back on the row
             * that had just been pressed, which is a loop rather than a way out.
             */
            const uint32_t focus = node->node_id;
            mesh_ui_nav_close_node_detail(nav);
            mesh_ui_nav_open_map(nav, store, focus);
            return true;
        }
        if (items[cursor].action == MESH_UI_NODE_ACTION_WAYPOINT) {
            /* Straight into naming it. The coordinate is not carried - the keyboard remembers
               whose fix to use and the app reads it from the roster when the name is done, so
               a node that moves while the user is typing is saved where it ends up. */
            mesh_ui_nav_open_waypoint_keyboard(nav, node->node_id);
            return true;
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
    case MESH_UI_SCREEN_WAYPOINTS:
        return mesh_ui_nav_waypoint_confirm(nav, store, cursor, action);
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
        if (nav->settings_section == MESH_UI_SETTINGS_MODULES) {
            /* A on a module row opens that module, exactly as a channel row opens a slot. The
               row carries its section in `number`, so this needs to know no more about what a
               module is than the channel branch above knows about a channel. */
            struct mesh_ui_settings_item row;
            if (!mesh_ui_nav_settings_current(nav, store, true, &row) ||
                row.kind != MESH_UI_SETTING_ACTION) {
                return false;
            }
            nav->settings_module_list_cursor = cursor;
            nav->settings_parent = MESH_UI_SETTINGS_MODULES;
            nav->settings_section = (uint8_t)row.number;
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
                    mesh_ui_nav_fill_settings_action(nav, which, action);
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
                    } else if (item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CYCLE_LANGUAGE) {
                        action->type = MESH_UI_ACTION_CYCLE_LANGUAGE;
                    } else if (item.number == (uint32_t)MESH_UI_SETTINGS_ACTION_CYCLE_THEME) {
                        action->type = MESH_UI_ACTION_CYCLE_THEME;
                    } else if (item.number ==
                               (uint32_t)MESH_UI_SETTINGS_ACTION_CHECK_RADIO_FIRMWARE) {
                        action->type = MESH_UI_ACTION_CHECK_RADIO_FIRMWARE;
                    } else if (item.number ==
                               (uint32_t)MESH_UI_SETTINGS_ACTION_CYCLE_FIRMWARE_CHANNEL) {
                        action->type = MESH_UI_ACTION_CYCLE_FIRMWARE_CHANNEL;
                    }
                }
                /* The row itself does not change; the app's reply comes back as new state. */
                return false;
            }
            return mesh_ui_nav_settings_edit_key(nav, store, MESH_UI_KEY_A);
        }
        if (cursor >= mesh_ui_settings_root_count()) {
            return false;
        }
        nav->settings_list_cursor = cursor;
        nav->settings_parent = MESH_UI_SETTINGS_NO_SECTION;
        nav->settings_section = (uint8_t)mesh_ui_settings_root_at(cursor);
        nav->cursor[MESH_UI_SCREEN_SETTINGS] = 0U;
        return true;
    }
    case MESH_UI_SCREEN_STATUS:
    default: {
        struct mesh_ui_status_actions actions;
        mesh_ui_nav_status_actions(store, &actions);
        if (cursor >= actions.count) {
            return false;
        }
        if (action == NULL) {
            return false;
        }
        switch ((enum mesh_ui_status_verb)actions.items[cursor].verb) {
        case MESH_UI_STATUS_VERB_DISCONNECT:
            /* The same press X makes on the Devices tab, and it names the radio for the same
               reason: only one link is ever up, so the transport is told which one to drop
               rather than being left to work it out. */
            action->type = MESH_UI_ACTION_DISCONNECT;
            for (size_t i = 0; i < store->device_count; ++i) {
                if (!store->devices[i].connected) {
                    continue;
                }
                snprintf(action->identifier, sizeof action->identifier, "%s",
                         store->devices[i].identifier);
                action->kind = store->devices[i].kind;
                break;
            }
            return false;
        case MESH_UI_STATUS_VERB_TREND:
            /* The one verb here that raises no action: what it opens is a screen drawn from what
               this client already watched, so there is nothing to ask the radio for and nothing
               for mesh_app to do. It returns true rather than false for exactly that reason -
               the nav changed, and nobody else is going to say so. */
            nav->trend_open = true;
            return true;
        case MESH_UI_STATUS_VERB_REFRESH:
        default:
            /* No pending edits to report: this screen has none to hold, unlike X on Settings. */
            action->type = MESH_UI_ACTION_REFRESH_SETTINGS;
            return false;
        }
    }
    }
}

/*
 * One key while help is up.
 *
 * Up and Down scroll, B leaves, and SELECT closes it the same way it opened it - a toggle,
 * because a key that only opens a screen leaves the user hunting for the way out of the one
 * screen in the client that exists to stop them hunting. Everything else is swallowed rather
 * than passed through: the section underneath is still holding pending edits, and a Left that
 * reached it would step a value the user cannot see.
 */
static bool mesh_ui_nav_help_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                 enum mesh_ui_key key) {
    switch (key) {
    case MESH_UI_KEY_B:
    case MESH_UI_KEY_SELECT:
        nav->help_open = false;
        nav->help_cursor = 0U;
        return true;
    case MESH_UI_KEY_UP:
        if (nav->help_cursor == 0U) {
            return false;
        }
        nav->help_cursor--;
        return true;
    case MESH_UI_KEY_DOWN: {
        const uint32_t rows = mesh_ui_nav_help_row_count(nav, store);
        if (rows == 0U || nav->help_cursor + 1U >= rows) {
            return false;
        }
        nav->help_cursor++;
        return true;
    }
    default:
        return false;
    }
}

/*
 * SELECT anywhere: open the explanation of this screen, when there is one.
 *
 * It asks mesh_ui_help_offered() rather than testing the nav, because src/ui/actions.c asks the
 * same question to decide whether to draw the keycap - and a press that worked where the bar
 * said nothing, or did nothing where it said "help", is the two-opinions bug this client keeps
 * a single table to avoid. Both halves of that happened here: with a discard armed, SELECT stood
 * the question down *and* opened help off one press, and with an edit pending it opened help
 * with no keycap on the frame.
 */
static bool mesh_ui_nav_open_help(struct mesh_ui_nav *nav, const struct mesh_ui_store *store) {
    const struct mesh_ui_handshake_state *handshake = mesh_ui_nav_handshake(store);
    if (!mesh_ui_help_offered(&store->settings, handshake, nav)) {
        return false;
    }
    struct mesh_ui_help_topic topic;
    if (!mesh_ui_help_topic(&store->settings, handshake, nav, &topic)) {
        return false;
    }
    nav->help_open = true;
    /* Open where the reader was looking rather than at the top: the paragraph about the row the
       question was asked about, or the nearest one above it. */
    nav->help_cursor =
        mesh_ui_help_entry_for_row(&store->settings, handshake, nav, nav->cursor[nav->screen]);
    if (nav->help_cursor >= topic.count) {
        nav->help_cursor = topic.count > 0U ? topic.count - 1U : 0U;
    }
    return true;
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

    /* Help first, because it is drawn over everything else: a key reaching the screen under an
       overlay the user is looking at is a key doing something they cannot see. */
    if (nav->help_open) {
        return mesh_ui_nav_help_key(nav, store, key) || changed;
    }
    /*
     * And opening help, before the overlays rather than after them, because SELECT means the
     * same thing everywhere it means anything at all.
     *
     * The overlay handlers below each consume every key they are given, so a press reaching the
     * bottom of this function is a press on a tab's own screen - which is where SELECT used to
     * be handled, and why the tapback picker could not be explained however much it needed
     * explaining. Hoisting it costs those handlers nothing: mesh_ui_nav_open_help() answers
     * false wherever there is no topic, so an overlay acquires the press by acquiring a
     * paragraph and not otherwise, and the ones that are asking the user a question have
     * neither.
     *
     * It is the same predicate the action bar reads, so the keycap and the press still cannot
     * disagree - including about the settings discard question, which has no topic while it is
     * armed and therefore falls through to the section handler that stands it down.
     */
    if (key == MESH_UI_KEY_SELECT && mesh_ui_nav_open_help(nav, store)) {
        return true;
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
    if (nav->reaction_open) {
        return mesh_ui_nav_reaction_key(nav, key, out_action) || changed;
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
    /* The same again for the open place's delete row: only A on that row may re-arm it, and
       moving off it stands it down, so the arming cannot outlive the row it was made on. */
    if (nav->waypoint_delete_armed &&
        (key != MESH_UI_KEY_A || nav->screen != MESH_UI_SCREEN_WAYPOINTS ||
         !nav->waypoint_detail_open)) {
        nav->waypoint_delete_armed = false;
        changed = true;
    }
    /* And the conversation list's delete, which only a second X on the list may carry out. A
       cursor move standing it down is the point: the row the question was asked about is the
       only row the answer may apply to. */
    if (nav->messages_delete_armed &&
        (key != MESH_UI_KEY_X || nav->screen != MESH_UI_SCREEN_MESSAGES || nav->thread_open)) {
        nav->messages_delete_armed = false;
        changed = true;
    }

    /*
     * The map, before the routing below turns Left and Right into a change of tab.
     *
     * It is the only screen that has to be taken here, and the reason is the one thing that
     * makes it unlike every other screen in the client: its d-pad moves the world rather than a
     * cursor. Left on a map is not "the tab to the left", and a press that fell through to the
     * switch below would walk off the map every time the reader tried to look west.
     *
     * The shoulders are deliberately not taken. L1/R1 and Left/Right are the same press
     * everywhere else, and splitting them is what lets the map have the d-pad without the tab
     * strip above it going dead - so the action bar still says "L/R tabs" here, and still
     * means it.
     */
    if (nav->map_open) {
        bool handled = false;
        const bool result = mesh_ui_nav_map_key(nav, store, key, &handled);
        if (handled) {
            return result || changed;
        }
    }

    /*
     * And the trend chart, for the map's reason with the map's exception the other way round.
     *
     * A chart has no cursor and no rows, so every press that walks or opens something means
     * nothing here - and the two that would otherwise reach the cards underneath are the
     * dangerous ones: Down would move a cursor nobody can see, and A would run whichever verb it
     * had moved onto. Swallowing them is what makes B the only way out, which is what the action
     * bar says.
     *
     * The shoulders and the d-pad's own Left and Right are deliberately not taken, which is
     * where this differs from the map: there is nothing to pan, so they stay the tab switch they
     * are on every other screen. And the screen is checked as well as the flag, because the flag
     * outlives the tab - see `trend_open`.
     */
    if (nav->trend_open && nav->screen == MESH_UI_SCREEN_STATUS) {
        switch (key) {
        case MESH_UI_KEY_LEFT:
        case MESH_UI_KEY_RIGHT:
        case MESH_UI_KEY_L1:
        case MESH_UI_KEY_R1:
        case MESH_UI_KEY_SELECT:
            break; /* the tabs, and the help press: both mean here what they mean everywhere */
        case MESH_UI_KEY_B:
            nav->trend_open = false;
            return true;
        default:
            return changed;
        }
    }

    if (nav->screen == MESH_UI_SCREEN_SETTINGS &&
        nav->settings_section != MESH_UI_SETTINGS_NO_SECTION) {
        /* A second press of anything but B stands the discard question down. */
        const bool was_armed = nav->settings_discard_armed;
        if (key != MESH_UI_KEY_B) {
            nav->settings_discard_armed = false;
            changed = changed || was_armed;
            /*
             * And a press spent standing the question down is spent.
             *
             * SELECT is the only key where that needs saying. Every other press here either
             * does nothing further or is itself the answer, but SELECT would go on to open the
             * help screen off the same press - so the user would get their question dismissed
             * and a screen they did not ask for, from one keystroke.
             *
             * mesh_ui_help_offered() already says help is not offered while the question is up,
             * and this is what stops the nav contradicting it: the flag that predicate reads has
             * been cleared two lines above, so by the time SELECT reaches it the state it is
             * asking about is gone. The bar reads the nav before any of this runs, which is why
             * the rule lives there and the ordering fix lives here.
             */
            if (was_armed && key == MESH_UI_KEY_SELECT) {
                return true;
            }
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
            /* The detail first, because it is the level on top - and when it was opened from
               the map, closing it lands back on the map with the view where it was left rather
               than on the list the reader was never on. The map's own B is taken above. */
            return mesh_ui_nav_close_node_detail(nav) || changed;
        }
        if (nav->screen == MESH_UI_SCREEN_WAYPOINTS) {
            return mesh_ui_nav_close_waypoint(nav) || changed;
        }
        return changed;
    case MESH_UI_KEY_X:
        if (nav->screen == MESH_UI_SCREEN_MESSAGES && !nav->thread_open) {
            /* A conversation list without a way to clear a thread out fills up with every node
               that ever said hello. */
            return mesh_ui_nav_delete_conversation(nav, store, nav->cursor[nav->screen],
                                                   out_action) ||
                   changed;
        }
        if (nav->screen == MESH_UI_SCREEN_MESSAGES && nav->thread_open && !nav->inbox) {
            /* Inside a conversation X is the tapback, on the bubble under the cursor. Not in
               all-traffic: a reaction goes out on the conversation the target belongs to, and
               that view is several of them at once. */
            uint32_t indices[MESH_UI_MAX_MESSAGES];
            const uint32_t count =
                mesh_ui_nav_filter_messages(nav, &store->messages, indices, MESH_UI_MAX_MESSAGES);
            const uint32_t cursor = nav->cursor[nav->screen];
            if (cursor >= count) {
                return changed;
            }
            return mesh_ui_nav_open_reactions(nav,
                                              store->messages.entries[indices[cursor]].packet_id) ||
                   changed;
        }
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
                    : mesh_ui_nav_node_at_row(store, nav->cursor[nav->screen]);
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
            /* In a conversation, write to it - the keyboard, not the canned list A offers.
               On the list (or in the all-traffic view, which has no single destination), pick
               who to write to first. */
            if (nav->thread_open && !nav->inbox) {
                mesh_ui_nav_open_keyboard(nav);
            } else {
                mesh_ui_nav_picker_open(nav, store, MESH_UI_PICKER_FOLLOW_KEYBOARD);
            }
            return true;
        }
        if (nav->screen == MESH_UI_SCREEN_NODES) {
            /* Straight from a contact into writing to it, from the list or from inside the
               node's detail. Which cursor names the node depends on which level is showing. */
            const struct mesh_ui_node_summary *node =
                nav->node_detail_open
                    ? mesh_ui_node_detail_find(&store->handshake, nav->node_detail_node)
                    : mesh_ui_nav_node_at_row(store, nav->cursor[nav->screen]);
            if (node == NULL || node->node_id == 0U || mesh_ui_nav_node_is_self(store, node)) {
                return changed;
            }
            mesh_ui_nav_open_thread(nav, store, node->node_id, 0U, NULL);
            mesh_ui_nav_open_keyboard(nav);
            return true;
        }
        return changed;
    case MESH_UI_KEY_SELECT:
        /* Already offered its chance above, before the overlays. Reaching here means there was
           nothing to explain, so the press is spent - and standing a notice down is the one
           thing it may still have done. */
        return changed;
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
    nav->keyboard_displaced = nav->keyboard_open;
    nav->keyboard_field_displaced = nav->keyboard_open ? nav->keyboard_field : MESH_UI_FIELD_NONE;
    nav->keyboard_field = MESH_UI_FIELD_NONE;
    nav->keyboard_passkey = true;
    nav->pairing_confirm = confirm;
    snprintf(nav->pairing_label, sizeof nav->pairing_label, "%s",
             label != NULL ? label : mesh_str(MESH_STR_PAIRING_NODE_FALLBACK));
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

/* How long a transient notice stands. One number, read by the setter and by the stamp below. */
#define MESH_UI_NAV_TOAST_MS 4000U

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
    nav->toast_until_ms = now_ms + MESH_UI_NAV_TOAST_MS;
}

/*
 * The same notice, raised from inside the nav, where there is no clock to raise it against.
 *
 * A key press is handled wherever the nav is driven from, and the drivers do not share a clock:
 * the app ticks the store with CLOCK_MONOTONIC, and the capture harness ticks it with a
 * synthetic one that starts at 1000 and moves only when a scene says `hold`. A deadline read
 * from the real clock inside a press would therefore mean "four seconds" on the device and
 * "longer than any scene" in a capture - the notice would sit on every frame after the press,
 * or, on a host that had just booted, expire somewhere unpredictable in the middle of one.
 *
 * So a press raises the notice undated and mesh_ui_store_handle_key() dates it from the clock
 * the store was last ticked with, which is by construction the clock driving the frames. The
 * gap is closed inside that one call, before anything is drawn: a frame carrying an undated
 * notice would read to the backend as a *different* notice a frame later - `until_ms` is what
 * tells two of them apart - and restart the entrance it was in the middle of.
 */
void mesh_ui_nav_raise_toast(struct mesh_ui_nav *nav, const char *text) {
    if (nav == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    snprintf(nav->toast, sizeof nav->toast, "%s", text);
    nav->toast_until_ms = 0U;
}

void mesh_ui_nav_date_toast(struct mesh_ui_nav *nav, uint64_t now_ms) {
    if (nav == NULL || nav->toast[0] == '\0' || nav->toast_until_ms != 0U) {
        return;
    }
    nav->toast_until_ms = now_ms + MESH_UI_NAV_TOAST_MS;
}

bool mesh_ui_nav_tick(struct mesh_ui_nav *nav, uint64_t now_ms) {
    if (nav == NULL || nav->toast[0] == '\0' || now_ms < nav->toast_until_ms) {
        return false;
    }
    nav->toast[0] = '\0';
    nav->toast_until_ms = 0U;
    return true;
}
