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
#include "mesh/ui/devices.h"
#include "mesh/ui/help.h"
#include "mesh/ui/history.h"
#include "mesh/ui/map.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/nodes.h"
#include "mesh/ui/reactions.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/status.h"
#include "mesh/ui/trend.h"
#include "mesh/ui/units.h"
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
    nav->thread_unread_from = mesh_ui_store_conversation_read_mark(
        store,
        node_id == MESH_MESSAGE_BROADCAST_ADDR ? (uint8_t)MESH_UI_CONVERSATION_CHANNEL
                                               : (uint8_t)MESH_UI_CONVERSATION_DIRECT,
        node_id, channel);
    mesh_ui_nav_set_target(nav, store, node_id, channel, name_hint);
    nav->thread_open = true;
    nav->screen = MESH_UI_SCREEN_MESSAGES;
    nav->cursor[MESH_UI_SCREEN_MESSAGES] = 0U;
    /* A reply names a packet id, and a packet id from the conversation we just left is not a
       message in this one. */
    nav->reply_to = 0U;
    /* Nor is a spent retry: the bubble it was spent on is not in this thread. Cleared here as
       well as on the next press, because the app opens a thread without one - from the picker,
       and from "Message this node" on the Nodes tab. */
    nav->resend_spent = false;
}

void mesh_ui_nav_open_all_traffic(struct mesh_ui_nav *nav) {
    if (!nav->thread_open) {
        nav->conversation_list_cursor = nav->cursor[MESH_UI_SCREEN_MESSAGES];
    }
    nav->inbox = true;
    nav->thread_open = true;
    /* All traffic keeps no mark of its own - opening it marks nothing read - so there is no
       "where you were" for it to rule a line under. */
    nav->thread_unread_from = 0U;
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
    nav->thread_unread_from = 0U;
    nav->messages_seen = 0U;
    nav->reply_to = 0U;
    nav->reaction_open = false;
    nav->resend_spent = false;
    nav->cursor[MESH_UI_SCREEN_MESSAGES] = nav->conversation_list_cursor;
    return true;
}

/*
 * Left and Right on either chart: the span picker, one segment along.
 *
 * The one screen in the client where the d-pad's Left and Right are not the tab switch, and it
 * is the map's split arriving at the other picture - with the same thing paying for it. The
 * shoulders are *not* taken, so the tab strip above the body stays alive and the action bar goes
 * on saying "L/R tabs" and meaning it; what the d-pad gets is the control that is actually on the
 * screen. Before this it got nothing at all: a chart has no cursor, so Left and Right fell
 * through to the tabs and the two halves of the same gesture did different things.
 *
 * Both charts, and one span between them - see `trend_span`. The step itself is
 * mesh_ui_trend_span_step(), because the order the segments draw in is the strip's rather than
 * this file's.
 */
static bool mesh_ui_nav_step_trend_span(struct mesh_ui_nav *nav, int delta) {
    const uint8_t next = mesh_ui_trend_span_step(nav->trend_span, delta);
    if (next == nav->trend_span) {
        return false;
    }
    nav->trend_span = next;
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
    /* The chart is a level *of* the detail, so it cannot outlive it. Left set, it would be the
       map's `map_open` bug one screen along: the next node opened would land straight on a chart
       of whichever reading the last one was showing. */
    nav->node_trend = MESH_UI_HISTORY_NONE;
    nav->cursor[MESH_UI_SCREEN_NODES] = nav->node_list_cursor;
    return true;
}

/*
 * The node a Nodes-list row is about, or NULL when the row is not about a node.
 *
 * One place that knows the list has lead rows on the front of it, and a filter and a sort over
 * the rest, so the four presses the list offers - A, X, Y and the detail's own opening - cannot
 * disagree about which node row 4 is. Every one of them went through mesh_ui_node_detail_at()
 * with the raw cursor before the map row existed, and every one of them would have been off by
 * one after it; the filter was the same mistake waiting a second time and the sort a third, and
 * both are worse than the off-by-one, because a wrong answer there is a *plausible* node rather
 * than an obviously shifted one.
 *
 * The view is built here and thrown away, which is a sort of the roster per press. That is the
 * right side of the trade: a press is not a frame, and the alternative is a view cached on the
 * nav that would have to be invalidated by every publish, every chip step and every pin - three
 * writers and one reader, for a hundred and twenty-eight elements.
 */
static const struct mesh_ui_node_summary *mesh_ui_nav_node_at_row(const struct mesh_ui_nav *nav,
                                                                  const struct mesh_ui_store *store,
                                                                  uint32_t cursor) {
    if (nav == NULL || store == NULL || cursor < MESH_UI_NODES_LEAD_ROWS) {
        return NULL;
    }
    struct mesh_ui_node_view view;
    mesh_ui_node_view_build(&store->handshake, (enum mesh_ui_node_filter)nav->node_filter,
                            (enum mesh_ui_node_sort)nav->node_sort, &view);
    return mesh_ui_node_view_at(&store->handshake, &view, cursor - MESH_UI_NODES_LEAD_ROWS);
}

/*
 * The two control rows at the top of the Nodes list, stepped by one.
 *
 * Left and Right, which is the Settings tab's rule arriving on the only other screen in this
 * client that has a field row. mesh_ui_nav_settings_edit_key() reads Left as -1 and everything
 * else as +1; this reads it the same way, from the same shaped call, so A still steps forward
 * and the two screens cannot drift into meaning different things by the same press.
 *
 * It used to be A and nothing else, and the strip it stepped carried no mark saying so. That is
 * the whole of what made this screen hard to start using: the pencil in a row's gutter is how
 * everything else in the client says "this is set here", the action bar's keycap is how it names
 * the press, and a filter that used neither was a control a reader had to find by trying every
 * button on the case.
 *
 * The shoulders are deliberately not taken. L1/R1 still walk the tabs, which is the whole reason
 * the d-pad's axis could be spent here - the same split the map, the trend chart and the node
 * detail already make, and the reason the action bar can go on saying "L/R tabs" and meaning it.
 *
 * The cursor stays where it is on both rows, and that is the property that makes the press safe
 * rather than a convention: they are the only two rows of this list that what they change cannot
 * re-number.
 */
static bool mesh_ui_nav_nodes_control_step(struct mesh_ui_nav *nav, uint32_t cursor,
                                           enum mesh_ui_key key) {
    const int delta = (key == MESH_UI_KEY_LEFT) ? -1 : +1;
    if (cursor == MESH_UI_NODES_FILTER_ROW) {
        nav->node_filter =
            (uint8_t)mesh_ui_node_filter_step((enum mesh_ui_node_filter)nav->node_filter, delta);
        return true;
    }
    if (cursor == MESH_UI_NODES_SORT_ROW) {
        nav->node_sort =
            (uint8_t)mesh_ui_node_sort_step((enum mesh_ui_node_sort)nav->node_sort, delta);
        return true;
    }
    return false;
}

/* Whether the Nodes list itself is what the reader is looking at, which is what decides that a
   press belongs to a control row rather than to the map or the detail drawn over it. The screen
   is checked as well as the two flags for `map_open`'s reason: both say where the Nodes tab is
   standing, not what is on the panel. */
static bool mesh_ui_nav_nodes_list_showing(const struct mesh_ui_nav *nav) {
    return nav != NULL && nav->screen == MESH_UI_SCREEN_NODES && !nav->node_detail_open &&
           !nav->map_open;
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
    /* Not zero, which is the narrowest span: a chart opens on everything it has, which is what
       it drew before there was a picker - see `trend_span`. */
    nav->trend_span = (uint8_t)MESH_UI_TREND_SPAN_ALL;
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

const struct mesh_ui_message *mesh_ui_nav_resendable(const struct mesh_ui_nav *nav,
                                                     const struct mesh_ui_message_list *messages) {
    if (nav == NULL || messages == NULL || !nav->thread_open) {
        return NULL;
    }
    /* Already spent on this press - see the field. Answered here rather than at the two call
       sites so the keycap goes with the press: the bar stops naming START the moment it stops
       doing anything, which is the whole reason both ask this one function. */
    if (nav->resend_spent) {
        return NULL;
    }
    /*
     * Not in the all-traffic view, for the tapback's reason: it is a transcript of several
     * conversations at once and the verbs about one bubble live in the conversation itself.
     * The failed message is still there to open and retry from.
     */
    if (nav->inbox) {
        return NULL;
    }
    uint32_t indices[MESH_UI_MAX_MESSAGES];
    const uint32_t count =
        mesh_ui_nav_filter_messages(nav, messages, indices, MESH_UI_MAX_MESSAGES);
    const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_MESSAGES];
    if (cursor >= count) {
        return NULL;
    }
    const struct mesh_ui_message *message = &messages->entries[indices[cursor]];
    if (message->direction != MESH_MESSAGE_OUTBOUND || message->ack != MESH_MESSAGE_ACK_FAILED) {
        return NULL;
    }
    /*
     * A message restored from the card before packet ids were kept has none, and it is still
     * resendable: everything the retry needs - who it was for, on which channel, and what it
     * said - is on the record itself. The id only names the attempt in the log line.
     */
    return message;
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

/*
 * The failed bubble, restated as the send that will replace it.
 *
 * Everything comes off the record rather than off the nav, including the destination: a thread
 * is filtered on nav->target_node, so the two agree here by construction, but the message is
 * the thing that failed and it is the thing that says where it was going. `reply_id` travels
 * with it because a retry of a reply is still an answer to the same message - re-aiming it at
 * the attempt that failed would thread the conversation to its own ghost.
 *
 * `text` is MESH_UI_DRAFT_MAX and a message's is MESH_UI_MESSAGE_TEXT_MAX, which are the same
 * number for the same reason: both are what one Data payload holds.
 */
static void mesh_ui_nav_fill_resend(struct mesh_ui_action *action,
                                    const struct mesh_ui_message *message) {
    if (action == NULL || message == NULL) {
        return;
    }
    action->type = MESH_UI_ACTION_RESEND;
    action->dest = message->peer;
    action->channel = message->channel;
    action->number = message->packet_id;
    action->reply_id = message->reply_id;
    snprintf(action->text, sizeof action->text, "%s", message->text);
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
            /*
             * The filter row, the sort row, the map row, and then whichever nodes the filter
             * keeps. A roster with nothing in it draws an empty state instead of a list, so none
             * of the lead rows is offered: the screen the map row would open is the same nothing
             * one level in, and a filter or a sort over an empty roster is a control with
             * nothing to do.
             *
             * The sort is absent from this arithmetic on purpose, and that is a claim worth
             * stating: it permutes the rows the filter kept and never selects among them, so
             * mesh_ui_node_filter_count() is this list's length under every sort. A test holds
             * it against the built view's own count, because a sort that dropped a row would
             * show up here as a cursor that walks off the end.
             *
             * A filter that keeps *none* of a roster that has something in it is the opposite
             * case and the list stays: the lead rows are how the reader gets back out, and a
             * screen that emptied itself would have taken the chip that emptied it away with the
             * rows. The renderer says so in words on the row where the nodes would be.
             */
            if (nodes == 0U) {
                return 0U;
            }
            return mesh_ui_node_filter_count(&store->handshake,
                                             nav == NULL
                                                 ? MESH_UI_NODE_FILTER_ALL
                                                 : (enum mesh_ui_node_filter)nav->node_filter) +
                   MESH_UI_NODES_LEAD_ROWS;
        }
        /*
         * The detail's own rows, whether or not a chart is open over them - which is where this
         * parts company with the map two branches up, and the difference is what the level
         * underneath does with its cursor.
         *
         * The map parks the list position in `node_list_cursor` and puts it back on the way out,
         * so the list's own cursor being zeroed by the clamp below costs nothing. A chart parks
         * nothing: the cursor it is standing on *is* the detail's, and it is what puts the reader
         * back on the row they pressed. Answering 0 here hands that cursor to the clamp's
         * empty-list arm, which sets it to 0 on the very next publish - so B out of a chart
         * landed on "Message this node" every time.
         *
         * A count with no way to move it is not a contradiction: mesh_ui_nav_handle_key()
         * swallows the d-pad while the chart is up, so the cursor cannot walk underneath it.
         */
        const struct mesh_ui_node_summary *node =
            mesh_ui_node_detail_find(&store->handshake, nav->node_detail_node);
        return mesh_ui_node_detail_count(node, mesh_ui_nav_node_is_self(store, node),
                                         &store->traceroute, &store->handshake);
    }
    case MESH_UI_SCREEN_WAYPOINTS:
        return mesh_ui_nav_waypoint_row_count(nav, store);
    case MESH_UI_SCREEN_DEVICES:
        return mesh_ui_devices_row_count(store->devices, store->device_count);
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
     * And the share sheet when there is no longer a link to show, which is the same clamp again:
     * a radio swap or a dropped channel table leaves a screen whose whole content has gone. The
     * screen says so rather than drawing an empty square, but leaving it standing would be a
     * level the user has to back out of to find out there was nothing in it.
     */
    if (nav->share_open && store->settings.share_url[0] == '\0') {
        nav->share_open = false;
        moved = true;
    }
    /* And the contact code sheet, on the field that feeds it, which is the same clamp once
       more: a radio swap or a dropped owner record leaves a screen whose whole content has
       gone. */
    if (nav->contact_open && store->settings.contact_url[0] == '\0') {
        nav->contact_open = false;
        moved = true;
    }
    /*
     * And the same for a node's chart, which has three ways to empty rather than one: the node
     * can fall out of the list, the history can be forgotten under it, and the *row* can go
     * while the history stays. It runs after the detail's own close above, which has already
     * cleared the reading in the first case and leaves this looking at a closed chart.
     *
     * The third is why this asks for the row rather than for the series, and it is the one that
     * cost a review round. A reading is an optional field of an optional Telemetry variant, so a
     * node that reports temperature and then reports without it takes the row away while the
     * readings the client already kept stay exactly as drawable as they were. Asked of the
     * history the chart stays open over a row that no longer exists - the renderer falls back to
     * the detail while this, the action bar, the help screen and the key handler all still
     * believe a picture is up, so the reader gets a list whose d-pad is swallowed until they
     * press B. The row is the honest question, and it is the same one the press asked.
     */
    if (nav->node_detail_open && nav->node_trend != MESH_UI_HISTORY_NONE) {
        const struct mesh_ui_node_summary *charted =
            mesh_ui_node_detail_find(&store->handshake, nav->node_detail_node);
        if (!mesh_ui_node_detail_trend_row(charted, mesh_ui_nav_node_is_self(store, charted),
                                           &store->traceroute, &store->handshake, &store->history,
                                           (enum mesh_ui_history_reading)nav->node_trend, NULL)) {
            nav->node_trend = MESH_UI_HISTORY_NONE;
            moved = true;
        }
    }

    /*
     * And the Status cursor, which is a verb rather than a position and so is repaired rather
     * than clipped. The generic loop below cannot do it: it holds an index in range, and the
     * verb that goes is not the last one - dropping the link takes disconnect away and can
     * leave the trend behind it.
     *
     * mesh_ui_status_verb_resolve() hands back the verb itself while it is still offered, which
     * is the ordinary case and the whole point: a verb arriving or leaving anywhere in the list
     * no longer moves what A does. With nothing on offer the remembered verb is kept, so a link
     * that drops and comes back lands the reader back on the button they were on.
     */
    {
        struct mesh_ui_status_actions actions;
        mesh_ui_nav_status_actions(store, &actions);
        const uint8_t verb = mesh_ui_status_verb_resolve(&actions, nav->status_verb);
        if (verb != nav->status_verb) {
            nav->status_verb = verb;
            moved = true;
        }
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

/*
 * Whether row `row` of the screen the nav is on is a group heading.
 *
 * Two screens draw them - the node detail and an open settings section - and on both the
 * cursor used to land on one: a full-width highlight under a dimmed word, with A doing nothing
 * and the action bar going on promising "select". That is the keycap-that-does-nothing this
 * client refuses everywhere else, and on the node detail it was also what the reader met
 * *first*, because the actions group now names itself and its heading is row 0.
 *
 * Asked of the built rows rather than of a rule about where headings fall, because which rows
 * exist depends on what the node has reported and what the radio has sent - the same reason
 * every other question about these two screens is answered by building. Both callers below are
 * already in a build's company, and both are a key press rather than a frame.
 *
 * A screen with no headings answers false for every row, which is what makes this a question
 * the mover can ask unconditionally.
 */
static bool mesh_ui_nav_row_is_heading(const struct mesh_ui_nav *nav,
                                       const struct mesh_ui_store *store, uint32_t row) {
    if (nav == NULL || store == NULL) {
        return false;
    }
    if (nav->screen == MESH_UI_SCREEN_NODES && nav->node_detail_open) {
        const struct mesh_ui_node_summary *node =
            mesh_ui_node_detail_find(&store->handshake, nav->node_detail_node);
        if (node == NULL) {
            return false;
        }
        struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
        /* No clock and no arming: neither adds or removes a row, and a row's *existence* is the
           only thing this reads - the same two arguments mesh_ui_node_detail_trend_at() passes
           nothing for, and for the same reason. */
        const uint32_t count = mesh_ui_node_detail_build(
            node, mesh_ui_nav_node_is_self(store, node), 0U, &store->traceroute, false,
            &store->handshake, NULL, false, items, MESH_UI_NODE_ITEMS_MAX);
        return row < count && items[row].kind == MESH_UI_NODE_ROW_HEADING;
    }
    if (nav->screen == MESH_UI_SCREEN_SETTINGS &&
        nav->settings_section != MESH_UI_SETTINGS_NO_SECTION) {
        struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
        const uint32_t count = mesh_ui_settings_items(
            &store->settings, store->handshake_valid ? &store->handshake : NULL,
            nav->settings_edits, nav->settings_edit_count,
            (enum mesh_ui_settings_section)nav->settings_section, nav->settings_channel, items,
            MESH_UI_SETTINGS_ITEMS_MAX);
        return row < count && items[row].kind == MESH_UI_SETTING_HEADING;
    }
    return false;
}

/*
 * The first row at or after `row` the cursor may stand on, walking `delta`'s way.
 *
 * Headings come in ones, so a single step would do for every list here today - but a group with
 * a heading and nothing under it is a list one condition away, and a loop that stops when it
 * runs out of rows costs nothing to write correctly. Answers `row` itself when nothing on that
 * side is selectable, which leaves the caller's own bounds check to refuse the move.
 */
static uint32_t mesh_ui_nav_skip_headings(const struct mesh_ui_nav *nav,
                                          const struct mesh_ui_store *store, uint32_t row,
                                          uint32_t rows, int delta) {
    uint32_t at = row;
    while (at < rows && mesh_ui_nav_row_is_heading(nav, store, at)) {
        if (delta < 0) {
            if (at == 0U) {
                return row;
            }
            at -= 1U;
        } else {
            at += 1U;
        }
    }
    return at < rows ? at : row;
}

/*
 * Puts `screen`'s cursor on the first row it may stand on, which is row 0 on every list whose
 * first row is not a group title.
 *
 * Five places open a level and every one of them used to write 0, which was the same answer
 * until the two screens that draw headings grew one at the top: the node detail's actions group
 * now names itself, and a settings section has opened on a heading since it had them. Reading
 * it rather than writing 1 is what keeps a level whose first group is conditional - our own
 * node offers no actions at all - from opening on a title anyway.
 */
void mesh_ui_nav_cursor_to_first_row(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                     enum mesh_ui_screen screen) {
    nav->cursor[screen] =
        mesh_ui_nav_skip_headings(nav, store, 0U, mesh_ui_nav_row_count(nav, store, screen), +1);
}

static bool mesh_ui_nav_move_cursor(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                    int delta) {
    if (nav->screen == MESH_UI_SCREEN_STATUS) {
        /* Status has no rows to walk. Its cursor is a verb, so a press moves along the list on
           offer and names what it lands on rather than counting how far it got - see
           include/mesh/ui/status.h. */
        struct mesh_ui_status_actions actions;
        mesh_ui_nav_status_actions(store, &actions);
        const uint8_t next = mesh_ui_status_verb_step(&actions, nav->status_verb, delta);
        if (next == nav->status_verb) {
            return false;
        }
        nav->status_verb = next;
        return true;
    }
    const uint32_t rows = mesh_ui_nav_row_count(nav, store, nav->screen);
    if (rows == 0U) {
        return false;
    }
    uint32_t *cursor = &nav->cursor[nav->screen];
    /*
     * One step, then over any heading the step landed on - the step is what the press means and
     * the skip is what keeps it on a row that answers. Going up, a heading at the very top has
     * nothing above it to skip to, so the walk gives the cursor back and the guards below
     * refuse the move: the reader stays on the first real row rather than parking on its title.
     */
    if (delta < 0) {
        if (*cursor == 0U) {
            return false;
        }
        const uint32_t next = mesh_ui_nav_skip_headings(nav, store, *cursor - 1U, rows, -1);
        if (next == *cursor || mesh_ui_nav_row_is_heading(nav, store, next)) {
            return false;
        }
        *cursor = next;
        return true;
    }
    if (*cursor + 1U >= rows) {
        return false;
    }
    const uint32_t next = mesh_ui_nav_skip_headings(nav, store, *cursor + 1U, rows, +1);
    if (next == *cursor || mesh_ui_nav_row_is_heading(nav, store, next)) {
        return false;
    }
    *cursor = next;
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
            /*
             * The filter and the sort, stepped forward.
             *
             * A as well as Left and Right, and in that order of importance: the d-pad is what
             * the row's pencil and the action bar both name, and A is here because it is what
             * steps an enum on a Settings field too (mesh_ui_nav_settings_edit_key), so a
             * reader who has learned either screen has learned both. One helper answers all
             * three keys, which is what keeps the forward step and the backward one from
             * becoming two opinions about the same two rows.
             */
            if (mesh_ui_nav_nodes_control_step(nav, cursor, MESH_UI_KEY_A)) {
                return true;
            }
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
            const struct mesh_ui_node_summary *node = mesh_ui_nav_node_at_row(nav, store, cursor);
            if (node == NULL || node->node_id == 0U) {
                return false;
            }
            nav->node_list_cursor = cursor;
            nav->node_detail_node = node->node_id;
            nav->node_detail_open = true;
            mesh_ui_nav_cursor_to_first_row(nav, store, MESH_UI_SCREEN_NODES);
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
            nav->node_remove_armed, &store->handshake, &store->history,
            mesh_ui_units_imperial(store->settings.units), items, MESH_UI_NODE_ITEMS_MAX);
        /*
         * A meter row carrying a trend opens that trend as a chart, and it is taken ahead of the
         * action-row guard below because it is the one press on this screen that is not an
         * action row: the row is a *reading*, and what A does on it is look closer rather than
         * ask the radio for anything.
         *
         * Which reading is read off the row rather than decided here, so the line the chart
         * draws and the bar the cursor was on are the same statement - see the row's
         * `trend_reading`. A row with no trend falls through to the guard and A means nothing on
         * it, which is what keeps the action bar honest: it names the press from the same field.
         */
        if (cursor < count && items[cursor].kind == MESH_UI_NODE_ROW_METER &&
            items[cursor].trend_reading != MESH_UI_HISTORY_NONE) {
            nav->node_trend = items[cursor].trend_reading;
            return true;
        }
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
        if (items[cursor].action == MESH_UI_NODE_ACTION_ADMIN) {
            /*
             * The press both asks and goes: the app points the admin queue at this node, and the
             * detail closes behind us onto the Settings tab, which is the thing that has just
             * changed meaning.
             *
             * Opened *under* the detail for the reason the map row gives - leaving the card up
             * over its own consequence would make B land back on the row that was pressed. The
             * tab lands on its section list rather than in a section, because which section
             * somebody wants of a radio they have just reached is not a guess this row can make.
             */
            if (action != NULL) {
                action->type = MESH_UI_ACTION_SET_ADMIN_TARGET;
                action->dest = node->node_id;
            }
            mesh_ui_nav_close_node_detail(nav);
            nav->screen = MESH_UI_SCREEN_SETTINGS;
            nav->settings_parent = MESH_UI_SETTINGS_NO_SECTION;
            nav->settings_section = MESH_UI_SETTINGS_NO_SECTION;
            nav->cursor[MESH_UI_SCREEN_SETTINGS] = nav->settings_list_cursor;
            return true;
        }
        if (items[cursor].action == MESH_UI_NODE_ACTION_ADD_CONTACT ||
            items[cursor].action == MESH_UI_NODE_ACTION_VERIFY_KEY) {
            if (action != NULL) {
                action->type = items[cursor].action == MESH_UI_NODE_ACTION_ADD_CONTACT
                                   ? MESH_UI_ACTION_ADD_CONTACT
                                   : MESH_UI_ACTION_VERIFY_KEY;
                action->dest = node->node_id;
            }
            /* Neither redraws anything here. An add-contact is settled by the node's next
               NodeInfo, and a verification by the sheet the app opens when the radio asks
               something - which is a frame this press cannot predict. */
            return false;
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
        struct mesh_ui_devices_row row;
        if (cursor >= rows || !mesh_ui_devices_row(store->devices, store->device_count,
                                                   store->network_host, cursor, &row)) {
            return false;
        }
        if (row.type == (uint8_t)MESH_UI_DEVICES_ROW_NETWORK) {
            /* With an address written down A is the ordinary connect every other row offers;
               with none there is nothing to connect to, so the press is the one that gets
               there - a row whose A did nothing until an address existed would be a row with
               no way of ever acquiring one. */
            if (row.host[0] == '\0') {
                mesh_ui_nav_open_network_keyboard(nav, row.host);
                return true;
            }
            if (action != NULL) {
                action->type = MESH_UI_ACTION_CONNECT;
                action->kind = (uint8_t)MESH_UI_DEVICE_TCP;
                snprintf(action->identifier, sizeof action->identifier, "%s", row.host);
            }
            return false;
        }
        if (!mesh_ui_device_connectable(row.device)) {
            return false;
        }
        if (action != NULL) {
            action->type = MESH_UI_ACTION_CONNECT;
            action->kind = row.device->kind;
            snprintf(action->identifier, sizeof action->identifier, "%s", row.device->identifier);
        }
        return false;
    }
    case MESH_UI_SCREEN_SETTINGS: {
        if (nav->settings_section == MESH_UI_SETTINGS_CHANNELS &&
            nav->settings_channel == MESH_UI_SETTINGS_NO_CHANNEL) {
            /* A on a channel row opens that slot, when the radio's full table is held. */
            const int slot = mesh_ui_settings_channel_at_row(&store->settings,
                                                             mesh_ui_nav_handshake(store), cursor);
            if (slot >= 0) {
                nav->settings_channel_list_cursor = cursor;
                nav->settings_channel = (uint8_t)slot;
                mesh_ui_nav_cursor_to_first_row(nav, store, MESH_UI_SCREEN_SETTINGS);
                return true;
            }
            /* Not a slot. The list's last two rows - share and import - are ordinary ACTION
               rows carrying a verb rather than a slot number, and are answered by the handler
               below with every other section's action rows. */
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
            mesh_ui_nav_cursor_to_first_row(nav, store, MESH_UI_SCREEN_SETTINGS);
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
                /*
                 * The two channel-sharing rows, which reach neither the app nor the radio on
                 * this press: one opens a screen and the other opens the keyboard. What the
                 * radio hears about is the answer to the sheet the keyboard raises.
                 */
                if (which == MESH_UI_SETTINGS_ACTION_SHARE_CHANNELS) {
                    nav->share_open = true;
                    return true;
                }
                if (which == MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS) {
                    mesh_ui_nav_open_channel_url_keyboard(nav);
                    return true;
                }
                /* And the contact pair, which are the same two presses one section over. */
                if (which == MESH_UI_SETTINGS_ACTION_SHARE_CONTACT) {
                    nav->contact_open = true;
                    return true;
                }
                if (which == MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT) {
                    mesh_ui_nav_open_contact_url_keyboard(nav);
                    return true;
                }
                /* The way back from remote administration. Not a radio action - nothing goes
                   over the air - and it carries `dest` 0, which is the verb's spelling of "the
                   radio on the end of the link". */
                if (which == MESH_UI_SETTINGS_ACTION_ADMIN_LOCAL) {
                    if (action != NULL) {
                        action->type = MESH_UI_ACTION_SET_ADMIN_TARGET;
                        action->dest = 0U;
                    }
                    /* The row it was pressed on is about to stop existing, so the cursor is put
                       somewhere that will still be a row when the section redraws. */
                    nav->cursor[MESH_UI_SCREEN_SETTINGS] = 0U;
                    return true;
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
                               (uint32_t)MESH_UI_SETTINGS_ACTION_DISCARD_CRASH_REPORT) {
                        action->type = MESH_UI_ACTION_DISCARD_CRASH_REPORT;
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
        mesh_ui_nav_cursor_to_first_row(nav, store, MESH_UI_SCREEN_SETTINGS);
        return true;
    }
    case MESH_UI_SCREEN_STATUS:
    default: {
        struct mesh_ui_status_actions actions;
        mesh_ui_nav_status_actions(store, &actions);
        /* The verb the cursor is on, asked for by name. A verb the screen is not offering is a
           button nobody can see, so the press does nothing - which is also what makes the check
           the same one the renderer and the action bar make. */
        const struct mesh_ui_status_action *chosen =
            mesh_ui_status_find(&actions, nav->status_verb);
        if (chosen == NULL) {
            return false;
        }
        if (action == NULL) {
            return false;
        }
        switch ((enum mesh_ui_status_verb)chosen->verb) {
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

/*
 * The share sheet, which is a picture and a way out of it.
 *
 * Every key closes it except the ones that would be a surprise. There is nothing here to
 * choose, so a d-pad press that scrolled nothing and a Y that saved nothing would both be the
 * screen pretending to have state; B leaves, the way it leaves help, and the rest are ignored
 * so that a thumb resting on the pad does not dismiss the code somebody is trying to scan.
 */
bool mesh_ui_nav_share_key(struct mesh_ui_nav *nav, enum mesh_ui_key key) {
    if (key != MESH_UI_KEY_B) {
        return false;
    }
    nav->share_open = false;
    return true;
}

/* B on the contact code sheet. The same screen shape as the share sheet and so the same rule:
   there is nothing on it to choose, so every key but the one that leaves is ignored rather than
   dismissing a code somebody is trying to scan. */
bool mesh_ui_nav_contact_key(struct mesh_ui_nav *nav, enum mesh_ui_key key) {
    if (key != MESH_UI_KEY_B) {
        return false;
    }
    nav->contact_open = false;
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
    /* Ahead of the confirm overlay, because this one is the more urgent of the two questions:
       a settings confirm is waiting on the user and will wait, while a verification is waiting
       on two people and a radio that gives up after five minutes. They cannot both be up in
       practice - the app closes the sheet whenever the exchange ends - and this says which wins
       if they ever are. */
    if (nav->verify_open) {
        return mesh_ui_nav_verify_key(nav, store, key, out_action) || changed;
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
    /*
     * The share sheet, under every overlay above and over the tab's own screen.
     *
     * Below them rather than above, which is the opposite of where it is *raised* from: it is
     * opened by a row, so it is a level of the Settings tab rather than a question - and the
     * things above it are the two the *radio* raises at any moment, a pairing PIN and a key
     * verification. A code being scanned is not a reason to make a PIN prompt unanswerable.
     */
    if (nav->share_open) {
        return mesh_ui_nav_share_key(nav, key) || changed;
    }
    if (nav->contact_open) {
        return mesh_ui_nav_contact_key(nav, key) || changed;
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
    /* The retry, which is the opposite arrangement: the thing a second press of the same key
       must not do is happen again. Anything else re-arms it, including the cursor move that
       walks onto another failed bubble - so a deliberate second press costs one other press
       and the kernel's autorepeat, which sends nothing but START, costs the mesh nothing. */
    if (nav->resend_spent && key != MESH_UI_KEY_START) {
        nav->resend_spent = false;
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
     * Left and Right are taken, and the shoulders are not - which is the map's split with the
     * halves swapped over. There is nothing to pan here, but there is something to choose: the
     * span picker over the plot is the only control on the screen, and the d-pad is what aims at
     * it. The shoulders staying the tab switch is what pays for that, exactly as it does on the
     * map. And the screen is checked as well as the flag, because the flag outlives the tab -
     * see `trend_open`.
     */
    if (nav->trend_open && nav->screen == MESH_UI_SCREEN_STATUS) {
        switch (key) {
        case MESH_UI_KEY_LEFT:
        case MESH_UI_KEY_RIGHT:
            return mesh_ui_nav_step_trend_span(nav, key == MESH_UI_KEY_RIGHT ? 1 : -1);
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

    /*
     * And a node's chart, which is the same picture with the same presses.
     *
     * It is a separate arm rather than a condition added to the one above because the two charts
     * are on different tabs and close different things - and because this one sits over a *list*
     * rather than over the Status cards, so what Down would reach through to is the detail's own
     * cursor. Same swallow, same exceptions, same reason B is the only way out.
     */
    if (nav->node_trend != MESH_UI_HISTORY_NONE && nav->screen == MESH_UI_SCREEN_NODES &&
        nav->node_detail_open) {
        switch (key) {
        case MESH_UI_KEY_LEFT:
        case MESH_UI_KEY_RIGHT:
            return mesh_ui_nav_step_trend_span(nav, key == MESH_UI_KEY_RIGHT ? 1 : -1);
        case MESH_UI_KEY_L1:
        case MESH_UI_KEY_R1:
        case MESH_UI_KEY_SELECT:
            break; /* the tabs, and the help press: both mean here what they mean everywhere */
        case MESH_UI_KEY_B:
            nav->node_trend = MESH_UI_HISTORY_NONE;
            return true;
        default:
            return changed;
        }
    }

    /*
     * The open node detail, which takes Left and Right to walk its cards.
     *
     * It is the chart's split and the map's, a third time, and it is paid for the same way: the
     * shoulders are deliberately not taken, so the tab strip above the body never goes dead and
     * the action bar goes on saying "L/R tabs" and meaning it. What buys the d-pad's horizontal
     * axis here is the length of this screen - a repeater reporting everything is a hundred and
     * twenty rows, and Up and Down cross it one row at a time past four dozen facts that no
     * press does anything to. The groups are drawn as cards, so the reader is already navigating
     * by card; this is the d-pad learning what the eye is doing.
     *
     * Below the two chart arms rather than above them, because a chart is drawn *over* this
     * screen and its Left and Right are its span picker: taken here first, the picture's only
     * control would move the list underneath it. Which is the same ordering fb_render_snapshot()
     * draws the two in, and the same reason this file always follows it.
     *
     * The screen is checked as well as the flag for `map_open`'s reason: node_detail_open says
     * where the Nodes tab is standing, not what the reader is looking at, and a shoulder walks
     * off the tab with the detail still open behind it.
     */
    /*
     * And Up and Down, which walk the detail's *stops* rather than its rows: every row A acts on,
     * and a card of facts as one stop - see mesh_ui_node_detail_step(). Taken here rather than
     * in mesh_ui_nav_move_cursor() because the answer needs the history the action bar reads,
     * and because refusing the move has to spend the press the same way Left and Right do.
     */
    if (nav->node_detail_open && nav->screen == MESH_UI_SCREEN_NODES &&
        (key == MESH_UI_KEY_LEFT || key == MESH_UI_KEY_RIGHT || key == MESH_UI_KEY_UP ||
         key == MESH_UI_KEY_DOWN)) {
        const struct mesh_ui_node_summary *node =
            mesh_ui_node_detail_find(&store->handshake, nav->node_detail_node);
        if (node != NULL) {
            const bool is_self = mesh_ui_nav_node_is_self(store, node);
            const uint32_t row = nav->cursor[MESH_UI_SCREEN_NODES];
            const int delta = key == MESH_UI_KEY_RIGHT || key == MESH_UI_KEY_DOWN ? +1 : -1;
            const uint32_t next =
                key == MESH_UI_KEY_LEFT || key == MESH_UI_KEY_RIGHT
                    ? mesh_ui_node_detail_group_step(node, is_self, &store->traceroute,
                                                     &store->handshake, &store->history,
                                                     store->page_rows, row, delta)
                    : mesh_ui_node_detail_step(node, is_self, &store->traceroute, &store->handshake,
                                               &store->history, store->page_rows, row, delta);
            if (next == row) {
                /* No group that way. The press is still spent rather than falling through to
                   the tabs: Left at the top of the first card meaning "leave the node" would be
                   the one screen in the client where the d-pad changes tab from inside a
                   level. */
                return changed;
            }
            nav->cursor[MESH_UI_SCREEN_NODES] = next;
            return true;
        }
    }

    /*
     * The Nodes list's two control rows, before the routing below turns Left and Right into a
     * change of tab - the map's placement and the map's reason, one screen along.
     *
     * Only these two rows take the axis. On every node row under them Left and Right are still
     * the tab switch, which is the objection this arrangement has to answer: a d-pad that means
     * two things on one screen. It is answered the way the Settings tab answers it, because that
     * screen has had exactly this shape since it was written - a field row edits, a row with no
     * field walks the tabs, and the pencil in the gutter is what tells the two apart before the
     * press. The rows that edit here wear the same pencil for the same reason.
     */
    if (mesh_ui_nav_nodes_list_showing(nav) &&
        (key == MESH_UI_KEY_LEFT || key == MESH_UI_KEY_RIGHT) &&
        mesh_ui_nav_nodes_control_step(nav, nav->cursor[MESH_UI_SCREEN_NODES], key)) {
        return true;
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
    case MESH_UI_KEY_START:
        /*
         * The conversation list is the second screen to spend START on something of its own,
         * and it is spent for the map's reason: there is no other key left. A opens, Y writes,
         * X deletes, SELECT explains and the shoulders walk the tabs, which is every button
         * this case has but B - and B means "back" on every screen in the client, which is a
         * meaning worth more than a mute.
         *
         * What it costs is START's usual job of standing in for A, on this one screen. That is
         * the trade the map already made for `fit`, and it is affordable for the same reason:
         * A itself is untouched and the action bar names the press, so the button that opens a
         * conversation is still the button the frame says opens a conversation.
         */
        if (nav->screen == MESH_UI_SCREEN_MESSAGES && !nav->thread_open) {
            return mesh_ui_nav_mute_conversation(nav, store, nav->cursor[nav->screen],
                                                 out_action) ||
                   changed;
        }
        /*
         * And inside a conversation it is the retry, on a bubble the mesh came back to say did
         * not arrive. The other three verbs about a bubble are already spoken for - A answers
         * it, X puts an emoji on it, Y writes to the conversation - and this is what is left.
         *
         * Only on such a bubble: everywhere else in the thread START goes on standing in for A,
         * which is what it does on every screen that has not taken it. That is the same shape
         * as the mute above, where the keycap names the press only on rows that offer one - and
         * it is why the bar asks mesh_ui_nav_resendable() rather than deciding for itself.
         */
        if (nav->screen == MESH_UI_SCREEN_MESSAGES && nav->thread_open) {
            /*
             * A retry already raised on this press goes nowhere, and stops here rather than
             * falling through to A. The fall-through is what makes the rest of the thread
             * behave, but under a held button it would resend once and then open the compose
             * sheet over the conversation - a sheet nobody asked for, from a button nobody
             * pressed twice. The bar is naming no verb on START while this is set, so a press
             * that does nothing is the bar and the nav agreeing.
             */
            if (nav->resend_spent) {
                return changed;
            }
            const struct mesh_ui_message *failed = mesh_ui_nav_resendable(nav, &store->messages);
            if (failed != NULL) {
                mesh_ui_nav_fill_resend(out_action, failed);
                nav->resend_spent = true;
                return true;
            }
        }
        return mesh_ui_nav_confirm(nav, store, out_action) || changed;
    case MESH_UI_KEY_A:
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
                    : mesh_ui_nav_node_at_row(nav, store, nav->cursor[nav->screen]);
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
            struct mesh_ui_devices_row row;
            if (!mesh_ui_devices_row(store->devices, store->device_count, store->network_host,
                                     cursor, &row)) {
                return changed;
            }
            if (row.type == (uint8_t)MESH_UI_DEVICES_ROW_NETWORK) {
                /* Y on the network row edits the address rather than forgetting a bond, and
                   there is no bond here to confuse it with. Clearing the field and pressing
                   Done is what forgetting a host is, so the arming Y needs everywhere else on
                   this screen is already spent one press further in. */
                mesh_ui_nav_open_network_keyboard(nav, row.host);
                return true;
            }
            if (!mesh_ui_device_forgettable(row.device)) {
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
                out_action->kind = row.device->kind;
                snprintf(out_action->identifier, sizeof out_action->identifier, "%s",
                         row.device->identifier);
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
                    : mesh_ui_nav_node_at_row(nav, store, nav->cursor[nav->screen]);
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

/*
 * The key-verification sheet, and the keyboard that collects the security number.
 *
 * Both are opened by the app rather than by a press, for the pairing prompt's reason exactly:
 * what raises them is the radio asking something in the middle of whatever the user was doing.
 * The sheet takes no argument at all - everything it says is in the snapshot, because it is a
 * description of what the radio is doing rather than of where the user is.
 */
bool mesh_ui_nav_open_verify(struct mesh_ui_nav *nav) {
    if (nav == NULL || nav->verify_open) {
        return false;
    }
    /*
     * The BlueZ pairing prompt outranks this, and the ordering is not arbitrary: that prompt is
     * *blocking a bond* on a thirty-second timeout, while a verification has five minutes and a
     * caller that retries. Raising a sheet over it would be worse than a wait - this overlay
     * takes keys ahead of the keyboard (see mesh_ui_nav_handle_key), so the PIN could not be
     * typed at all, and the bond would fail while its prompt sat unreachable underneath.
     */
    if (nav->keyboard_passkey) {
        return false;
    }
    nav->verify_open = true;
    /* On the answer that does not act, which on the comparison is "they match" and on the two
       waiting stages is the one that gets out of the way. Neither costs anything to land on,
       which is what makes 0 the right default here and Cancel the right one on a destructive
       settings confirm. */
    nav->verify_cursor = 0U;
    return true;
}

bool mesh_ui_nav_close_verify(struct mesh_ui_nav *nav) {
    if (nav == NULL || !nav->verify_open) {
        return false;
    }
    nav->verify_open = false;
    nav->verify_cursor = 0U;
    return true;
}

bool mesh_ui_nav_open_verify_number(struct mesh_ui_nav *nav) {
    if (nav == NULL || nav->keyboard_verify) {
        return false;
    }
    /*
     * Defers to the pairing prompt for the reason above, and here the cost of not deferring was
     * sharper still: both flavours are the same keyboard, so setting this one while that one
     * stood left *both* flags true. The dispatch and the renderer both prefer the passkey, and
     * closing it cleared the pair - so the verification prompt was never drawn, never answerable
     * and never reopened, and the ceremony ran out its five minutes behind a screen that had
     * already gone.
     */
    if (nav->keyboard_passkey) {
        return false;
    }
    /* The sheet and the keyboard are never both up: the number is one stage of the ceremony and
       the sheet draws the others. */
    nav->verify_open = false;
    /* Whatever the keyboard was doing is parked and comes back on close, exactly as the pairing
       prompt parks it - see mesh_ui_nav_open_passkey() for why that is a flag rather than a
       field, and mesh_ui_nav_keyboard_close() for the restore. */
    snprintf(nav->draft_saved, sizeof nav->draft_saved, "%s", nav->draft);
    nav->keyboard_displaced = nav->keyboard_open;
    nav->keyboard_field_displaced = nav->keyboard_open ? nav->keyboard_field : MESH_UI_FIELD_NONE;
    nav->keyboard_field = MESH_UI_FIELD_NONE;
    nav->keyboard_verify = true;
    nav->draft[0] = '\0';
    nav->keyboard_open = true;
    nav->kb_row = 0U; /* the digit row */
    nav->kb_col = 0U;
    nav->kb_layer = MESH_UI_KB_LOWER;
    return true;
}

bool mesh_ui_nav_close_verify_number(struct mesh_ui_nav *nav) {
    if (nav == NULL || !nav->keyboard_verify) {
        return false;
    }
    nav->draft[0] = '\0';
    mesh_ui_nav_keyboard_close(nav);
    return true;
}

/*
 * One key while the sheet is up.
 *
 * Which answer each button gives depends on the stage, and the stage is in the store rather
 * than on the nav - so this reads it there, the way every other handler here reads a list
 * length there. The alternative would be a copy of the stage on the nav, which is a second
 * opinion about what the radio is doing and would be wrong for exactly as long as it took the
 * next notification to land.
 *
 * B is not an answer. It closes the sheet and says nothing to the radio, on every stage
 * including the comparison: backing out is how every other overlay in the client behaves, and a
 * B that quietly told the far end "these do not match" would turn a mis-press into a refusal
 * the user never made. The exchange stays open and the core expires it (see
 * mesh/core/key_verification.h), or the user answers it when the sheet comes back.
 */
bool mesh_ui_nav_verify_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                            enum mesh_ui_key key, struct mesh_ui_action *action) {
    const uint8_t stage = store != NULL ? store->verification.stage : 0U;
    switch (key) {
    case MESH_UI_KEY_UP:
    case MESH_UI_KEY_DOWN:
    case MESH_UI_KEY_LEFT:
    case MESH_UI_KEY_RIGHT:
        nav->verify_cursor = nav->verify_cursor == 0U ? 1U : 0U;
        return true;
    case MESH_UI_KEY_A:
    case MESH_UI_KEY_START: {
        const bool comparing = stage == (uint8_t)MESH_UI_VERIFY_COMPARE;
        const bool accepted = nav->verify_cursor == 0U;
        /*
         * Three of the four presses send something and one does not. On the comparison both
         * answers do - yes and no are both answers - while on the two waiting stages the first
         * button only gets out of the way: the exchange is still running and the sheet comes
         * back when the radio next asks for something.
         */
        if (action != NULL && (comparing || !accepted)) {
            action->type = MESH_UI_ACTION_VERIFY_ANSWER;
            action->number = (comparing && accepted) ? 1U : 0U;
        }
        (void)mesh_ui_nav_close_verify(nav);
        return true;
    }
    case MESH_UI_KEY_B:
        (void)mesh_ui_nav_close_verify(nav);
        return true;
    default:
        return false;
    }
}

/* How long a transient notice stands. One number, read by the setter and by the stamp below. */
#define MESH_UI_NAV_TOAST_MS 4000U

/*
 * Takes a notice that cannot be said yet, or says why it need not be.
 *
 * Returns true when the caller has nothing more to do - the notice is queued, or is a repeat of
 * one the user is already looking at. False means the snackbar is free and the caller should put
 * the notice straight on it.
 *
 * The repeat test is against what is *showing* and against the newest thing waiting, which is
 * the shape the duplicate actually takes: one event reported twice in a row - a link dropping,
 * a request refused again - rather than the same sentence coming back around after two others.
 * Two identical notices in a row are one notice that stood for eight seconds, which is a
 * snackbar with a stuck button rather than news.
 */
static bool mesh_ui_nav_queue_toast(struct mesh_ui_nav *nav, const char *text) {
    if (nav->toast[0] == '\0') {
        return false; /* nothing is up; say it now */
    }
    const char *newest =
        nav->toast_queued > 0U ? nav->toast_queue[nav->toast_queued - 1U] : nav->toast;
    if (strcmp(newest, text) == 0) {
        return true;
    }
    if (nav->toast_queued >= MESH_UI_NAV_TOAST_QUEUE) {
        /* Drop the oldest waiting one and close the gap - see the field for why it is that end. */
        memmove(&nav->toast_queue[0], &nav->toast_queue[1],
                (MESH_UI_NAV_TOAST_QUEUE - 1U) * sizeof nav->toast_queue[0]);
        nav->toast_queued = MESH_UI_NAV_TOAST_QUEUE - 1U;
    }
    snprintf(nav->toast_queue[nav->toast_queued++], MESH_UI_NAV_TOAST_MAX, "%s", text);
    return true;
}

void mesh_ui_nav_set_toast(struct mesh_ui_nav *nav, uint64_t now_ms, const char *text) {
    if (nav == NULL) {
        return;
    }
    if (text == NULL || text[0] == '\0') {
        /* Clearing clears the backlog with it: an empty notice means "say nothing", and a queue
           that outlived it would start talking again a moment later. */
        nav->toast[0] = '\0';
        nav->toast_until_ms = 0U;
        nav->toast_queued = 0U;
        return;
    }
    snprintf(nav->toast, sizeof nav->toast, "%s", text);
    nav->toast_until_ms = now_ms + MESH_UI_NAV_TOAST_MS;
}

/*
 * A notice nothing the user did has asked for: something arrived.
 *
 * The difference from the setter above is which one yields, and it is the whole reason there are
 * two. A notice raised by a *press* supersedes whatever is on the snackbar, because it is the
 * client answering the button that was just pressed and the thing it replaces is usually the
 * earlier half of the same story - "Connecting to NodeSeven" giving way to "NodeSeven needs
 * pairing" is one sentence finishing, not two events, and making the user watch the optimistic
 * half for four seconds before the true one is worse than losing it.
 *
 * A notice about something that *arrived* has no such claim. It is news the user did not ask
 * for, it is not superseding anything, and overwriting the answer to a press with it is how a
 * button comes to look as though it did nothing. So this one waits its turn - and waits behind
 * the other notifications already waiting, which is what stops a burst of arrivals showing the
 * user only whichever happened to be last.
 */
void mesh_ui_nav_post_toast(struct mesh_ui_nav *nav, uint64_t now_ms, const char *text) {
    if (nav == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    if (mesh_ui_nav_queue_toast(nav, text)) {
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
    if (nav->toast_queued > 0U) {
        /*
         * The next one takes the snackbar, dated from this tick rather than from whenever it was
         * raised: it is starting to stand now. That also gives it a `until_ms` the backend has
         * not seen, which is how the widget tells one notice from the next - so it slides in
         * rather than appearing to be the same notice with different words.
         */
        snprintf(nav->toast, sizeof nav->toast, "%s", nav->toast_queue[0]);
        nav->toast_until_ms = now_ms + MESH_UI_NAV_TOAST_MS;
        memmove(&nav->toast_queue[0], &nav->toast_queue[1],
                (MESH_UI_NAV_TOAST_QUEUE - 1U) * sizeof nav->toast_queue[0]);
        nav->toast_queued--;
        return true;
    }
    nav->toast[0] = '\0';
    nav->toast_until_ms = 0U;
    return true;
}
