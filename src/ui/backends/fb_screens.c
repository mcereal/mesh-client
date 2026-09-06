#define _POSIX_C_SOURCE 200809L

/*
 * One renderer per screen, plus the chrome and the frame that dispatches between them.
 *
 * Each takes the immutable snapshot and a layout, and draws - no state is kept between frames,
 * which is why the scroll window is derived every time (struct fb_list) rather than remembered.
 * Adding a screen is a fb_render_* here and a case in fb_render_snapshot().
 *
 * Nothing in this file computes a pixel coordinate or a padding width. A row is built with
 * struct mesh_ui_line (which measures in cells) and handed to a component in fb_widgets.h,
 * so a renderer reads as a description of its content rather than as arithmetic.
 */

#include "fb_widgets.h"

#include "mesh/core/message.h"
#include "mesh/ui/emoji.h"
#include "mesh/ui/font5x7.h"
#include "mesh/ui/input.h"
#include "mesh/ui/layout.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/settings.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void fb_store_view(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_store *view) {
    memset(view, 0, sizeof *view);
    memcpy(view->devices, snapshot->devices, sizeof view->devices);
    view->device_count = snapshot->device_count;
    view->handshake = snapshot->handshake;
    view->handshake_valid = snapshot->handshake_valid;
    view->messages = snapshot->messages;
    view->read_state = snapshot->read_state;
    view->event_fd = -1;
}

/* ---- chrome ------------------------------------------------------------------------------ */

/* The tab strip: one chip per screen, then the rule that separates it from the body. */
static void fb_draw_tabs(const struct mesh_ui_backend_fb_state *state,
                         const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const int small = layout->small;
    const int y = FB_MARGIN / 2 + small;
    int x = FB_MARGIN / 2;

    for (int i = 0; i < MESH_UI_SCREEN_COUNT; ++i) {
        const enum mesh_ui_screen screen = (enum mesh_ui_screen)i;
        x = fb_draw_chip(state, x, y, mesh_ui_screen_name(screen), snapshot->nav.screen == screen,
                         small);
    }

    const int line = fb_line_adv(small);
    fb_draw_rule(state, 0, y + line, (int)state->var.xres, small, k_fb_tab_active_bg);
    layout->body_y = y + line + 2 * small + FB_MARGIN / 2;
}

/* Two lines under the body: what the buttons do here, then a toast or the link summary. */
static void fb_draw_footer(const struct mesh_ui_backend_fb_state *state,
                           const struct mesh_ui_snapshot *snapshot, const struct fb_layout *layout,
                           const char *hint) {
    const int small = layout->small;
    const size_t cols = fb_cols(state, small);

    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", hint);
    mesh_ui_line_fit(&line, cols);
    fb_draw_text(state, FB_MARGIN, layout->footer_y, mesh_ui_line_text(&line), small, k_fb_dim);

    const struct mesh_ui_nav *nav = &snapshot->nav;
    enum fb_tone tone = FB_TONE_DIM;
    mesh_ui_line_reset(&line);
    if (nav->toast[0] != '\0') {
        mesh_ui_line_printf(&line, "%s", nav->toast);
        tone = FB_TONE_ACCENT;
    } else {
        const char *status =
            snapshot->transport_status[0] != '\0' ? snapshot->transport_status : "starting";
        const char *connected = NULL;
        for (size_t i = 0; i < snapshot->device_count; ++i) {
            if (snapshot->devices[i].connected) {
                connected = snapshot->devices[i].name[0] != '\0' ? snapshot->devices[i].name
                                                                 : snapshot->devices[i].identifier;
                break;
            }
        }
        if (connected != NULL) {
            mesh_ui_line_printf(&line, "%s: %s", status, connected);
            tone = FB_TONE_GOOD;
        } else {
            mesh_ui_line_printf(&line, "%s | %s", status, mesh_ui_input_quit_hint());
        }
    }
    mesh_ui_line_fit(&line, cols);
    fb_draw_text(state, FB_MARGIN, layout->footer_y + fb_line_adv(small), mesh_ui_line_text(&line),
                 small, fb_tone_color(tone));
}

/* ---- screens ----------------------------------------------------------------------------- */

/* Level one of the Messages tab: all traffic, the channels, whoever we have direct messages
   with, and the way to start a new one. Two lines a row - name and newest message. */
static void fb_render_conversations(const struct mesh_ui_backend_fb_state *state,
                                    const struct mesh_ui_snapshot *snapshot,
                                    struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    struct mesh_ui_store view;
    fb_store_view(snapshot, &view);

    const uint32_t count = mesh_ui_nav_conversation_count(&view);
    char title[96];
    fb_title_count(title, sizeof title, "Messages", count, snapshot->messages.dropped);
    fb_draw_title(state, layout, title);
    if (count == 0U) {
        fb_draw_empty(state, layout, "Connect to a node to see conversations.");
        return;
    }

    /*
     * Each conversation is two rows: who it is with and when it last spoke, then what was said
     * last. The unread count is a filled badge on the preview row rather than the words "3 new"
     * in the metric column - it is the one thing on this screen you look for without reading,
     * and the column it used to share with the age meant you got one or the other.
     */
    struct fb_list list =
        fb_list_begin_rows(layout, count, nav->cursor[MESH_UI_SCREEN_MESSAGES], 2U);
    struct mesh_ui_line line;
    char age[8];
    char badge[12];
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        struct mesh_ui_conversation conversation;
        if (!mesh_ui_nav_conversation_at(&view, i, &conversation)) {
            break;
        }
        const bool is_new = (conversation.kind == MESH_UI_CONVERSATION_NEW);
        const bool unread = (conversation.unread > 0U);
        enum fb_tone tone = FB_TONE_NORMAL;
        if (conversation.kind == MESH_UI_CONVERSATION_CHANNEL ||
            conversation.kind == MESH_UI_CONVERSATION_ALL) {
            tone = FB_TONE_ACCENT;
        } else if (is_new) {
            tone = FB_TONE_DIM;
        }
        /* Unread is drawn bright; so is the open thread, so B lands somewhere recognisable. */
        if (unread || mesh_ui_nav_conversation_is_open(nav, &conversation)) {
            tone = FB_TONE_STRONG;
        }

        badge[0] = '\0';
        if (unread) {
            /* Past two figures a badge stops being a number and becomes a width, which is what
               every messenger's "99+" is for. */
            if (conversation.unread > 99U) {
                snprintf(badge, sizeof badge, "%s", " 99+ ");
            } else {
                snprintf(badge, sizeof badge, " %u ", (unsigned)conversation.unread);
            }
        }

        mesh_ui_line_reset(&line);
        if (is_new) {
            mesh_ui_line_printf(&line, "+ %s", conversation.name);
            fb_list_row_line(state, &list, i, &line, tone);
            mesh_ui_line_reset(&line);
            fb_list_sub_row(state, &list, mesh_ui_line_text(&line), FB_TONE_DIM);
            continue;
        }

        /* The name row carries the age on the right, the way a messenger dates a conversation.
           A radio with no clock set reports rx_time 0, so the column is simply empty rather
           than a bare "?" nobody can act on. */
        age[0] = '\0';
        if (conversation.last_time != 0U) {
            fb_format_age(conversation.last_time, age, sizeof age);
        }
        mesh_ui_line_printf(&line, "%s", conversation.name);
        mesh_ui_line_right(&line, layout->cols, age);
        fb_list_row_line(state, &list, i, &line, tone);

        mesh_ui_line_reset(&line);
        if (conversation.preview[0] != '\0') {
            /* "> " says the last word was ours, which is what tells you whether a quiet thread
               is waiting on you or on them. */
            mesh_ui_line_printf(&line, "%s%s", conversation.preview_outbound ? "> " : "",
                                conversation.preview);
        } else {
            mesh_ui_line_printf(&line, "%s", "no messages yet");
        }
        /* Both rows highlight together: a conversation is one item, not two rows that happen to
           be adjacent. */
        fb_list_row_line_badge(state, &list, i, &line, unread ? FB_TONE_NORMAL : FB_TONE_DIM,
                               badge);
    }
}

/* ---- the thread ---------------------------------------------------------------------------- */

/*
 * Level two: the open conversation, drawn as a transcript of bubbles.
 *
 * The shape is the one every messenger has: theirs on the left, ours on the right, the newest
 * against the bottom, the sender said once per run rather than once per line, and the clock
 * tucked into the message it belongs to. It replaced a list of clipped one-line rows with a
 * detail pane underneath - which meant the only way to read a message in full was to select it,
 * and reading the one before it meant losing the one you had.
 *
 * Nothing here computes a pixel: fb_bubble_rows() says how tall a message is and
 * mesh_ui_transcript_window() says which of them are on screen.
 */

/* A message as the screen describes it, with the strings the bubble points at. Built twice per
   frame - once to measure, once to draw - rather than kept, because a per-message cache is a
   second source of truth for what the bubble says. */
struct fb_thread_row {
    struct fb_bubble bubble;
    char separator[24];
    char name[48];
    char meta[64];
};

/* "Today" / "Yesterday" / "Mon 3 Sep", or nothing when the radio has no clock set. */
static void fb_format_day(uint32_t rx_time, char *out, size_t out_len) {
    out[0] = '\0';
    if (rx_time == 0U) {
        return;
    }
    const time_t stamp = (time_t)rx_time;
    struct tm when;
    if (localtime_r(&stamp, &when) == NULL) {
        return;
    }
    const time_t now = time(NULL);
    struct tm today;
    if (now != (time_t)-1 && localtime_r(&now, &today) != NULL) {
        if (when.tm_year == today.tm_year && when.tm_yday == today.tm_yday) {
            snprintf(out, out_len, "%s", "Today");
            return;
        }
        if (when.tm_year == today.tm_year && when.tm_yday + 1 == today.tm_yday) {
            snprintf(out, out_len, "%s", "Yesterday");
            return;
        }
    }
    /* "%e" pads a single-digit day with a space, which reads as a typo in a centred label. */
    char month[8];
    (void)strftime(month, sizeof month, "%b", &when);
    char weekday[8];
    (void)strftime(weekday, sizeof weekday, "%a", &when);
    snprintf(out, out_len, "%s %d %s", weekday, when.tm_mday, month);
}

/* A day apart, or a long enough silence, is a break in the conversation; anything closer is the
   same exchange and gets no furniture between the messages. */
#define FB_THREAD_GAP_SECONDS 1800U /* 30 minutes: a new separator */
#define FB_THREAD_RUN_SECONDS 300U  /* 5 minutes: still the same run, so the name is not repeated  \
                                     */

static bool fb_thread_same_day(uint32_t a, uint32_t b) {
    if (a == 0U || b == 0U) {
        return a == b;
    }
    const time_t ta = (time_t)a;
    const time_t tb = (time_t)b;
    struct tm ma;
    struct tm mb;
    if (localtime_r(&ta, &ma) == NULL || localtime_r(&tb, &mb) == NULL) {
        return false;
    }
    return ma.tm_year == mb.tm_year && ma.tm_yday == mb.tm_yday;
}

static uint32_t fb_thread_elapsed(uint32_t earlier, uint32_t later) {
    return (earlier == 0U || later == 0U || later < earlier) ? 0U : later - earlier;
}

/* Everything the screen decides about one message: what furniture it gets and what it says. */
static void fb_thread_row_build(const struct mesh_ui_snapshot *snapshot, const uint32_t *indices,
                                uint32_t position, struct fb_thread_row *row) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_message *message = &snapshot->messages.entries[indices[position]];
    const struct mesh_ui_message *previous =
        position > 0U ? &snapshot->messages.entries[indices[position - 1U]] : NULL;
    const bool outbound = (message->direction == MESH_MESSAGE_OUTBOUND);

    memset(row, 0, sizeof *row);
    row->bubble.text = message->text;
    row->bubble.outbound = outbound;
    row->bubble.failed = outbound && message->ack == MESH_MESSAGE_ACK_FAILED;
    row->bubble.separator = row->separator;
    row->bubble.name = row->name;
    row->bubble.meta = row->meta;

    /* A separator opens the transcript and marks every day boundary and every long silence, so
       "when was this" is answered by the shape of the screen rather than by reading timestamps. */
    if (previous == NULL || !fb_thread_same_day(previous->rx_time, message->rx_time)) {
        fb_format_day(message->rx_time, row->separator, sizeof row->separator);
    } else if (fb_thread_elapsed(previous->rx_time, message->rx_time) >= FB_THREAD_GAP_SECONDS) {
        fb_format_clock(message->rx_time, row->separator, sizeof row->separator);
    }

    /*
     * Who sent it, said once per run. In a direct conversation the title already answers it, so
     * naming every bubble would be repeating the screen's own heading at the user; in a channel
     * and in all-traffic it is the only thing that says which of thirty nodes is talking.
     */
    const bool names_needed = nav->inbox || nav->target_node == MESH_MESSAGE_BROADCAST_ADDR;
    const bool starts_run =
        previous == NULL || row->separator[0] != '\0' ||
        (previous->direction == MESH_MESSAGE_OUTBOUND) != outbound ||
        previous->peer != message->peer || previous->broadcast != message->broadcast ||
        previous->channel != message->channel ||
        fb_thread_elapsed(previous->rx_time, message->rx_time) >= FB_THREAD_RUN_SECONDS;

    if (starts_run && (names_needed || (outbound && nav->inbox))) {
        const char *peer = message->peer_name[0] != '\0' ? message->peer_name : "?";
        struct mesh_ui_line line;
        mesh_ui_line_reset(&line);
        if (outbound) {
            /* Ours in all-traffic still needs a destination: "sent" alone does not say to whom,
               and a broadcast and a DM look identical without it. */
            mesh_ui_line_printf(&line, message->broadcast ? "%s" : "to %s",
                                message->broadcast ? "sent" : peer);
        } else {
            mesh_ui_line_printf(&line, "%s", peer);
        }
        /* All-traffic is several conversations at once, so each bubble says which one it is. */
        if (nav->inbox) {
            if (message->broadcast) {
                mesh_ui_line_printf(&line, "  #%u", (unsigned)message->channel);
            } else {
                mesh_ui_line_printf(&line, "%s", "  dm");
            }
        }
        mesh_str_copy(row->name, sizeof row->name, mesh_ui_line_text(&line));
    }

    /* The clock, and for ours what became of it. A failure says why: "!!" alone leaves the user
       with no idea whether to move, retry or fix a key, and those are different problems. */
    struct mesh_ui_line meta;
    mesh_ui_line_reset(&meta);
    char clock[8];
    fb_format_clock(message->rx_time, clock, sizeof clock);
    if (clock[0] != '\0') {
        mesh_ui_line_printf(&meta, "%s", clock);
    }
    if (outbound && message->ack != MESH_MESSAGE_ACK_NONE) {
        const char *space = mesh_ui_line_width(&meta) > 0U ? " " : "";
        if (message->ack == MESH_MESSAGE_ACK_FAILED) {
            /* Routing_Error NONE reads as "delivered", which on a failed message is a straight
               contradiction. It should not reach us - a failure carries a reason - but a bubble
               is the wrong place to find out that it did. */
            mesh_ui_line_printf(&meta, "%s!! %s", space,
                                message->ack_error != 0U
                                    ? mesh_message_ack_error_to_string(message->ack_error)
                                    : "failed");
        } else {
            mesh_ui_line_printf(&meta, "%s%s", space,
                                message->ack == MESH_MESSAGE_ACK_DELIVERED ? "ok" : "..");
        }
    }
    mesh_str_copy(row->meta, sizeof row->meta, mesh_ui_line_text(&meta));
}

static void fb_render_thread(const struct mesh_ui_backend_fb_state *state,
                             const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;

    uint32_t indices[MESH_UI_MAX_MESSAGES];
    const uint32_t count =
        mesh_ui_nav_filter_messages(nav, &snapshot->messages, indices, MESH_UI_MAX_MESSAGES);

    char convo[MESH_UI_NAV_TARGET_NAME_MAX];
    mesh_ui_nav_conversation_name(nav, convo, sizeof convo);
    char title[96];
    if (nav->inbox) {
        fb_title_count(title, sizeof title, convo, count, snapshot->messages.dropped);
    } else if (snapshot->messages.dropped > 0U) {
        snprintf(title, sizeof title, "%s  %s  (+%u older)", convo,
                 nav->target_node == MESH_MESSAGE_BROADCAST_ADDR ? "channel" : "direct",
                 (unsigned)snapshot->messages.dropped);
    } else {
        snprintf(title, sizeof title, "%s  %s", convo,
                 nav->target_node == MESH_MESSAGE_BROADCAST_ADDR ? "channel" : "direct");
    }
    fb_draw_title(state, layout, title);

    if (count == 0U) {
        fb_draw_empty(state, layout,
                      nav->inbox ? "No messages yet. B goes back to the list."
                                 : "Nothing here yet. Y writes one, B goes back.");
        return;
    }

    /* Measure every message, then let the transcript say which of them are on screen. Heights
       come from the same component that draws them, so the window can never be a row out. */
    uint8_t heights[MESH_UI_MAX_MESSAGES];
    struct fb_thread_row row;
    for (uint32_t i = 0; i < count; ++i) {
        fb_thread_row_build(snapshot, indices, i, &row);
        const uint32_t rows = fb_bubble_rows(layout, &row.bubble);
        heights[i] = rows > 0xFFU ? 0xFFU : (uint8_t)rows;
    }

    const struct mesh_ui_transcript window = mesh_ui_transcript_window(
        heights, count, nav->cursor[MESH_UI_SCREEN_MESSAGES], layout->rows);

    int y = layout->body_y + (int)window.pad * layout->line;
    for (uint32_t i = window.first; i < window.first + window.count && i < count; ++i) {
        fb_thread_row_build(snapshot, indices, i, &row);
        row.bubble.selected = (i == nav->cursor[MESH_UI_SCREEN_MESSAGES]);
        fb_draw_bubble(state, layout, y, &row.bubble);
        y += (int)heights[i] * layout->line;
    }
}

/*
 * One node's detail: the same list-of-rows shape the Settings tab draws, so the two screens
 * scroll and clip identically. Headings are dimmed and get no value column; the action row
 * carries the "> " marker an editable settings row uses, for the same reason - it is the only
 * thing on the screen A does anything to.
 */
static void fb_render_node_detail(const struct mesh_ui_backend_fb_state *state,
                                  const struct mesh_ui_snapshot *snapshot,
                                  struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    const struct mesh_ui_node_summary *node = mesh_ui_node_detail_find(hs, nav->node_detail_node);
    if (node == NULL) {
        fb_draw_title(state, layout, "Nodes");
        fb_draw_empty(state, layout, "That node is no longer in the list.");
        return;
    }

    const bool is_self = hs->has_my_info && node->node_id == hs->my_info.node_num;
    char title[96];
    const char *name = node->long_name[0] != '\0'    ? node->long_name
                       : node->short_name[0] != '\0' ? node->short_name
                                                     : NULL;
    if (name != NULL) {
        snprintf(title, sizeof title, "Nodes > %s", name);
    } else {
        snprintf(title, sizeof title, "Nodes > !%08x", node->node_id);
    }
    fb_draw_title(state, layout, title);

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count =
        mesh_ui_node_detail_build(node, is_self, (uint32_t)time(NULL), &snapshot->traceroute,
                                  nav->node_remove_armed, items, MESH_UI_NODE_ITEMS_MAX);
    if (count == 0U) {
        fb_draw_empty(state, layout, "Nothing reported for this node yet.");
        return;
    }

    const size_t label_cols = fb_field_label_cols(layout, 16U);
    struct fb_list list = fb_list_begin(layout, count, nav->cursor[MESH_UI_SCREEN_NODES]);
    struct mesh_ui_line line;
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        const struct mesh_ui_node_item *item = &items[i];
        if (item->kind == MESH_UI_NODE_ROW_HEADING) {
            mesh_ui_line_reset(&line);
            mesh_ui_line_printf(&line, "%s", item->label);
            fb_list_row_line(state, &list, i, &line, FB_TONE_DIM);
        } else if (item->kind == MESH_UI_NODE_ROW_ACTION) {
            mesh_ui_line_reset(&line);
            mesh_ui_line_printf(&line, "> %s", item->label);
            fb_list_row_line(state, &list, i, &line, FB_TONE_ACCENT);
        } else {
            fb_list_field_row(state, &list, i, item->label, label_cols, " ", item->value,
                              FB_TONE_NORMAL);
        }
    }
}

static void fb_render_nodes(const struct mesh_ui_backend_fb_state *state,
                            const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    if (nav->node_detail_open) {
        fb_render_node_detail(state, snapshot, layout);
        return;
    }
    if (!snapshot->handshake_valid || snapshot->handshake.node_count == 0U) {
        fb_draw_title(state, layout, "Nodes");
        fb_draw_empty(state, layout,
                      snapshot->handshake_valid ? "Waiting for the node list..."
                                                : "Connect to a node to see the mesh.");
        return;
    }

    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    const uint32_t count =
        hs->node_count > MESH_UI_MAX_HANDSHAKE_NODES ? MESH_UI_MAX_HANDSHAKE_NODES : hs->node_count;
    char title[96];
    if (hs->has_my_info && hs->my_info.nodedb_entries > count) {
        snprintf(title, sizeof title, "Nodes (%u of %u)", count, hs->my_info.nodedb_entries);
    } else {
        fb_title_count(title, sizeof title, "Nodes", count, 0U);
    }
    fb_draw_title(state, layout, title);

    const uint32_t me = hs->has_my_info ? hs->my_info.node_num : 0U;
    struct fb_list list = fb_list_begin(layout, count, nav->cursor[MESH_UI_SCREEN_NODES]);
    struct mesh_ui_line line;
    char right[32];
    char age[8];
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        const struct mesh_ui_node_summary *node = &hs->nodes[i];
        const char *short_name = node->short_name[0] != '\0' ? node->short_name : "----";
        const char *long_name = node->long_name[0] != '\0' ? node->long_name : "";
        fb_format_age(node->last_heard, age, sizeof age);

        if (node->has_hops_away && node->hops_away > 0U) {
            snprintf(right, sizeof right, "%uhop %s", (unsigned)node->hops_away, age);
        } else if (node->via_mqtt) {
            snprintf(right, sizeof right, "mqtt %s", age);
        } else {
            snprintf(right, sizeof right, "%.1fdB %s", (double)node->snr, age);
        }

        /* The marker column: ourselves, then pinned. A star sprite rather than an ASCII
           stand-in because the row is measured in cells, so it costs one column exactly like
           the '*' does. */
        const char *marker = (me != 0U && node->node_id == me) ? "*"
                             : node->is_favorite               ? "\xE2\xAD\x90"
                                                               : " ";
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%s", marker);
        mesh_ui_line_column(&line, short_name, 4U);
        mesh_ui_line_printf(&line, " %s", long_name);
        mesh_ui_line_right(&line, layout->cols, right);
        fb_list_row_line(state, &list, i, &line,
                         (node->node_id == nav->target_node) ? FB_TONE_ACCENT : FB_TONE_NORMAL);
    }
}

/* Compose overlay: it writes to the open thread, so the destination is a heading rather than
   an editable row. */
static void fb_render_compose(const struct mesh_ui_backend_fb_state *state,
                              const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    char title[96];
    snprintf(title, sizeof title, "To: %s%s", nav->target_name,
             nav->target_node == MESH_MESSAGE_BROADCAST_ADDR ? "  (channel)" : "  (direct)");
    fb_draw_title(state, layout, title);

    struct fb_list list =
        fb_list_begin(layout, mesh_ui_nav_compose_row_count(), nav->compose_cursor);
    struct mesh_ui_line line;
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        mesh_ui_line_reset(&line);
        if (i == MESH_UI_COMPOSE_ROW_DRAFT) {
            if (nav->draft[0] != '\0') {
                mesh_ui_line_printf(&line, "Draft: %s", nav->draft);
            } else {
                mesh_ui_line_printf(&line, "%s", "[ Type a message ]");
            }
            fb_list_row_line(state, &list, i, &line, FB_TONE_ACCENT);
        } else {
            mesh_ui_line_printf(&line, "  %s",
                                mesh_ui_canned_text(i - MESH_UI_COMPOSE_FIRST_CANNED));
            fb_list_row_line(state, &list, i, &line, FB_TONE_NORMAL);
        }
    }
}

/* "Send to" list: channels, then nodes, cursor on the current target. */
static void fb_render_picker(const struct mesh_ui_backend_fb_state *state,
                             const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    struct mesh_ui_store view;
    fb_store_view(snapshot, &view);

    const uint32_t count = mesh_ui_nav_picker_count(&view);
    char title[96];
    fb_title_count(title, sizeof title, "Send to", count, 0U);
    fb_draw_title(state, layout, title);
    if (count == 0U) {
        fb_draw_empty(state, layout, "No channels or nodes known yet.");
        return;
    }

    struct fb_list list = fb_list_begin(layout, count, nav->picker_cursor);
    struct mesh_ui_line line;
    char name[96];
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        uint32_t node = 0U;
        uint8_t channel = 0U;
        if (!mesh_ui_nav_picker_row(&view, i, &node, &channel, name, sizeof name)) {
            break;
        }
        const bool is_channel = (node == MESH_MESSAGE_BROADCAST_ADDR);
        const bool current =
            (node == nav->target_node) && (!is_channel || channel == nav->target_channel);
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%c %s%s", current ? '*' : ' ', name,
                            is_channel ? "  (channel)" : "");
        fb_list_row_line(state, &list, i, &line, is_channel ? FB_TONE_ACCENT : FB_TONE_NORMAL);
    }
}

/* The on-screen keyboard takes the whole body: target, the draft so far, then the grid. */
static void fb_render_keyboard(const struct mesh_ui_backend_fb_state *state,
                               const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const bool for_passkey = nav->keyboard_passkey;
    const bool for_setting = (!for_passkey && nav->keyboard_field != MESH_UI_FIELD_NONE);
    const size_t draft_cap =
        for_passkey
            ? 6U
            : (for_setting
                   ? mesh_ui_settings_text_max((enum mesh_ui_setting_field)nav->keyboard_field)
                   : MESH_UI_DRAFT_MAX - 1U);
    char title[96];
    if (for_passkey) {
        /* The one prompt the user cannot act on without being told what to look at: the digits
           are on the node's own screen, not anywhere on this one. */
        snprintf(title, sizeof title,
                 nav->pairing_confirm ? "Does %s show this?" : "PIN shown on %s",
                 nav->pairing_label[0] != '\0' ? nav->pairing_label : "the node");
    } else if (for_setting) {
        snprintf(title, sizeof title, "%s",
                 mesh_ui_settings_field_label((enum mesh_ui_setting_field)nav->keyboard_field));
    } else {
        snprintf(title, sizeof title, "To: %s", nav->target_name);
    }
    fb_draw_title(state, layout, title);

    const int scale = state->scale;
    const int line = layout->line;
    int y = layout->body_y;

    /* Draft box: two wrapped lines plus a cursor and a byte count. */
    const int box_lines = 2;
    fb_fill_rect(state, FB_MARGIN / 2, y - scale, (int)state->var.xres - FB_MARGIN,
                 box_lines * line + scale, (struct fb_rgb){0x14, 0x22, 0x32});
    char draft[MESH_UI_DRAFT_MAX + 2U];
    snprintf(draft, sizeof draft, "%s_", nav->draft);
    /* Show the tail when the draft outgrows the box. */
    const size_t visible = layout->cols * (size_t)box_lines;
    const char *shown = draft;
    const size_t draft_width = mesh_ui_text_cells(draft);
    if (draft_width > visible) {
        shown = draft + mesh_ui_text_cell_offset(draft, draft_width - visible);
    }
    fb_draw_wrapped(state, y, shown, layout->cols, box_lines, k_fb_white);
    y += box_lines * line;

    char meter[32];
    snprintf(meter, sizeof meter, "%zu/%zu", strlen(nav->draft), draft_cap);
    fb_draw_text(state,
                 (int)state->var.xres - FB_MARGIN -
                     (int)mesh_ui_text_cells(meter) * fb_char_adv(layout->small),
                 y, meter, layout->small, k_fb_dim);
    y += fb_line_adv(layout->small) + scale;

    /* The character grid and the action row are the same button, sized differently. */
    const int grid_w = (int)state->var.xres - 2 * FB_MARGIN;
    const int cell_w = grid_w / (int)MESH_UI_KB_COLS;
    const int cell_h = line + 2 * scale;
    for (unsigned row = 0; row < MESH_UI_KB_CHAR_ROWS; ++row) {
        for (unsigned col = 0; col < MESH_UI_KB_COLS; ++col) {
            const char ch = mesh_ui_kb_char((enum mesh_ui_kb_layer)nav->kb_layer, row, col);
            const char key[2] = {ch, '\0'};
            const struct fb_button button = {
                .rect = {.x = FB_MARGIN + (int)col * cell_w,
                         .y = y,
                         .w = cell_w - scale,
                         .h = cell_h - scale},
                .label = key,
                .selected = (nav->kb_row == row && nav->kb_col == col),
                .filled = false,
                .idle_tone = FB_TONE_NORMAL,
                .scale = scale,
            };
            fb_draw_button(state, &button);
        }
        y += cell_h;
    }

    const int action_w = grid_w / (int)MESH_UI_KB_ACTIONS;
    for (unsigned col = 0; col < MESH_UI_KB_ACTIONS; ++col) {
        const struct fb_button button = {
            .rect = {.x = FB_MARGIN + (int)col * action_w,
                     .y = y,
                     .w = action_w - scale,
                     .h = cell_h - scale},
            .label = mesh_ui_kb_action_label(nav, (enum mesh_ui_kb_action)col),
            .selected = (nav->kb_row == MESH_UI_KB_CHAR_ROWS && nav->kb_col == col),
            .filled = true,
            .idle_tone = FB_TONE_NORMAL,
            .scale = scale,
        };
        fb_draw_button(state, &button);
    }
}

static void fb_render_devices(const struct mesh_ui_backend_fb_state *state,
                              const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    char title[96];
    fb_title_count(title, sizeof title, "Devices", (uint32_t)snapshot->device_count, 0U);
    fb_draw_title(state, layout, title);

    if (snapshot->device_count == 0U) {
        fb_draw_empty(state, layout, "Scanning for Meshtastic nodes...");
        return;
    }

    struct fb_list list = fb_list_begin(layout, (uint32_t)snapshot->device_count,
                                        nav->cursor[MESH_UI_SCREEN_DEVICES]);
    struct mesh_ui_line line;
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        const struct mesh_ui_device *device = &snapshot->devices[i];
        const char *name = device->name[0] != '\0' ? device->name : device->identifier;
        if (name[0] == '\0') {
            name = "<unknown>";
        }
        /* What pressing A on this row would do. An unpaired BLE node is the case worth
           calling out: it connects and then fails on StartNotify unless it is bonded first,
           which is exactly what A now does for it. */
        const char *badge = "";
        if (device->connected) {
            badge = "  connected";
        } else if (device->busy) {
            badge = "  working...";
        } else if (device->kind == (uint8_t)MESH_UI_DEVICE_BLE && !device->paired) {
            badge = "  needs pairing";
        } else if (device->kind == (uint8_t)MESH_UI_DEVICE_BLE) {
            badge = "  paired";
        }

        mesh_ui_line_reset(&line);
        /* A USB port has no RSSI to show; the badge is what tells the two kinds apart. */
        if (device->kind == (uint8_t)MESH_UI_DEVICE_SERIAL) {
            mesh_ui_line_printf(&line, "%c %s  USB%s", device->connected ? '*' : ' ', name, badge);
        } else {
            mesh_ui_line_printf(&line, "%c %s  %ddBm%s", device->connected ? '*' : ' ', name,
                                (int)device->rssi, badge);
        }

        enum fb_tone tone = FB_TONE_NORMAL;
        if (device->connected) {
            tone = FB_TONE_GOOD;
        } else if (nav->devices_forget_armed && nav->devices_forget_row == i) {
            tone = FB_TONE_BAD;
        }
        fb_list_row_line(state, &list, i, &line, tone);
    }
}

/* "3d 4h", "5h 12m", "40m" - a radio's uptime, which is a duration rather than an age. */
static void fb_format_uptime(uint32_t seconds, char *out, size_t out_len) {
    if (seconds >= 86400U) {
        snprintf(out, out_len, "%ud %uh", seconds / 86400U, (seconds % 86400U) / 3600U);
    } else if (seconds >= 3600U) {
        snprintf(out, out_len, "%uh %um", seconds / 3600U, (seconds % 3600U) / 60U);
    } else {
        snprintf(out, out_len, "%um", seconds / 60U);
    }
}

/* Our own node's record, which is where the connected radio's battery and airtime live: those
   arrive as ordinary DeviceMetrics telemetry, not in LocalStats. NULL before the sync. */
static const struct mesh_ui_node_summary *fb_self_node(const struct mesh_ui_snapshot *snapshot) {
    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    if (!snapshot->handshake_valid || !hs->has_my_info) {
        return NULL;
    }
    for (uint32_t i = 0; i < hs->node_count && i < MESH_UI_MAX_HANDSHAKE_NODES; ++i) {
        if (hs->nodes[i].node_id == hs->my_info.node_num) {
            return &hs->nodes[i];
        }
    }
    return NULL;
}

static void fb_render_status(const struct mesh_ui_backend_fb_state *state,
                             const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    fb_draw_title(state, layout, "Status");

    int y = layout->body_y;
    char buffer[64];
    char second[64];

    fb_draw_status_row(state, layout, &y, FB_TONE_NORMAL, "Transport", "%s",
                       snapshot->transport_status[0] != '\0' ? snapshot->transport_status
                                                             : "starting");

    const struct mesh_ui_device *connected = NULL;
    for (size_t i = 0; i < snapshot->device_count; ++i) {
        if (snapshot->devices[i].connected) {
            connected = &snapshot->devices[i];
            break;
        }
    }
    fb_draw_status_row(
        state, layout, &y, connected != NULL ? FB_TONE_GOOD : FB_TONE_BAD, "Radio", "%s",
        connected != NULL ? (connected->name[0] != '\0' ? connected->name : connected->identifier)
                          : "not connected");

    if (snapshot->handshake_valid) {
        const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
        fb_draw_status_row(state, layout, &y, FB_TONE_NORMAL, "Sync", "%s%s",
                           hs->config_complete ? "complete"
                                               : (hs->request_in_flight ? "in progress" : "idle"),
                           hs->cached ? " (cached)" : "");
        if (hs->has_my_info) {
            fb_draw_status_row(state, layout, &y, FB_TONE_NORMAL, "My node", "%s !%08x",
                               hs->my_short_name, hs->my_info.node_num);
        }
        /* One line for the NodeDB, and LocalStats' online count when the radio has sent it:
           "132 nodes" alone says nothing about how much of that mesh is still alive. */
        const struct mesh_ui_radio_stats *stats = &snapshot->settings.stats;
        if (hs->has_my_info && stats->valid && stats->num_online_nodes > 0U) {
            fb_draw_status_row(state, layout, &y, FB_TONE_NORMAL, "NodeDB", "%u nodes, %u online",
                               hs->my_info.nodedb_entries, stats->num_online_nodes);
        } else if (hs->has_my_info) {
            fb_draw_status_row(state, layout, &y, FB_TONE_NORMAL, "NodeDB", "%u nodes, %u reboots",
                               hs->my_info.nodedb_entries, hs->my_info.reboot_count);
        }
        if (hs->primary_channel[0] != '\0') {
            fb_draw_status_row(state, layout, &y, FB_TONE_NORMAL, "Channel", "%s",
                               hs->primary_channel);
        }
    } else {
        fb_draw_status_row(state, layout, &y, FB_TONE_DIM, "Sync", "%s", "waiting for a radio");
    }

    /*
     * Mesh health, from the two sources that carry it. LocalStats is the radio's own live
     * view, sent to the attached client on its own schedule; DeviceMetrics is what our node
     * last *broadcast* about itself, on the telemetry interval, which is half an hour by
     * default. Both carry the airtime pair, so LocalStats wins it when it has arrived and
     * DeviceMetrics only fills the gap before the first report - reading the broadcast copy
     * by preference means the row can sit on a half-hour-old 0.0% while the radio is busy.
     * Battery and uptime have only the one source: LocalStats has no battery at all.
     */
    const struct mesh_ui_node_summary *self = fb_self_node(snapshot);
    const struct mesh_ui_node_metrics *metrics =
        (self != NULL && self->metrics.valid) ? &self->metrics : NULL;
    const struct mesh_ui_radio_stats *stats = &snapshot->settings.stats;

    if (metrics != NULL || stats->valid) {
        y += layout->line / 2;
    }

    /* LocalStats' airtime fields are plain scalars the firmware always fills, so `valid` is
       the whole test; DeviceMetrics' are optional and carry their own has_*. */
    const bool air_from_stats = stats->valid;
    const bool have_util = air_from_stats || (metrics != NULL && metrics->has_channel_utilization);
    const bool have_tx = air_from_stats || (metrics != NULL && metrics->has_air_util_tx);
    if (have_util || have_tx) {
        const float util_value = air_from_stats
                                     ? stats->channel_utilization
                                     : (metrics != NULL ? metrics->channel_utilization : 0.0f);
        const float tx_value =
            air_from_stats ? stats->air_util_tx : (metrics != NULL ? metrics->air_util_tx : 0.0f);
        /* Above ~25% channel utilization the mesh is saturated and hop delivery collapses, so
           the number is coloured rather than left as one more figure to interpret. */
        enum fb_tone air_tone = FB_TONE_NORMAL;
        if (have_util) {
            air_tone = util_value >= 50.0f   ? FB_TONE_BAD
                       : util_value >= 25.0f ? FB_TONE_ACCENT
                                             : FB_TONE_GOOD;
        }
        char util[32] = "?";
        if (have_util) {
            snprintf(util, sizeof util, "%.1f%%", (double)util_value);
        }
        char tx[32] = "?";
        if (have_tx) {
            snprintf(tx, sizeof tx, "%.1f%%", (double)tx_value);
        }
        if (stats->valid && stats->has_noise_floor) {
            fb_draw_status_row(state, layout, &y, air_tone, "Airtime",
                               "%s busy, %s tx, %d dBm floor", util, tx, stats->noise_floor);
        } else {
            fb_draw_status_row(state, layout, &y, air_tone, "Airtime", "%s busy, %s tx", util, tx);
        }
    }

    /* Uptime is in both, like the airtime pair above, so LocalStats wins it for the same
       reason - and without this the row vanishes entirely when LocalStats has arrived but our
       node has not broadcast DeviceMetrics yet. Battery really does have only the one source. */
    const bool have_battery = metrics != NULL && metrics->has_battery;
    const bool have_uptime = stats->valid || (metrics != NULL && metrics->has_uptime);
    const uint32_t uptime_value = stats->valid        ? stats->uptime_seconds
                                  : (metrics != NULL) ? metrics->uptime_seconds
                                                      : 0U;
    if (have_battery || have_uptime) {
        buffer[0] = '\0';
        if (have_battery) {
            /* 101 is upstream's "running off USB", not a 101% battery. */
            if (metrics->battery_level > 100U) {
                snprintf(buffer, sizeof buffer, "plugged in");
            } else {
                snprintf(buffer, sizeof buffer, "%u%%", (unsigned)metrics->battery_level);
            }
        }
        second[0] = '\0';
        if (have_uptime) {
            char uptime[32];
            fb_format_uptime(uptime_value, uptime, sizeof uptime);
            snprintf(second, sizeof second, "%sup %s", buffer[0] != '\0' ? ", " : "", uptime);
        }
        const enum fb_tone battery_tone =
            (have_battery && metrics->battery_level <= 20U) ? FB_TONE_BAD : FB_TONE_NORMAL;
        fb_draw_status_row(state, layout, &y, battery_tone, "Battery", "%s%s",
                           buffer[0] != '\0' ? buffer : "unknown", second);
    }

    if (stats->valid) {
        fb_draw_status_row(state, layout, &y, FB_TONE_NORMAL, "Packets", "%u tx, %u rx, %u relayed",
                           stats->num_packets_tx, stats->num_packets_rx, stats->num_tx_relay);
        /* Bad and dropped packets are the two numbers that explain a mesh that "works but
           loses messages", so they get their own row instead of being folded into Packets. */
        const bool losing = stats->num_packets_rx_bad > 0U || stats->num_tx_dropped > 0U;
        fb_draw_status_row(state, layout, &y, losing ? FB_TONE_ACCENT : FB_TONE_DIM, "Dropped",
                           "%u bad rx, %u dupe, %u tx", stats->num_packets_rx_bad,
                           stats->num_rx_dupe, stats->num_tx_dropped);
        if (stats->has_heap) {
            fb_draw_status_row(state, layout, &y,
                               stats->heap_free_bytes < 20480U ? FB_TONE_ACCENT : FB_TONE_DIM,
                               "Heap", "%u KB free of %u KB", stats->heap_free_bytes / 1024U,
                               stats->heap_total_bytes / 1024U);
        }
    } else if (snapshot->handshake_valid) {
        fb_draw_status_row(state, layout, &y, FB_TONE_DIM, "Mesh", "%s",
                           "waiting for the radio's first report");
    }

    y += layout->line / 2;
    fb_draw_status_row(state, layout, &y, FB_TONE_NORMAL, "Messages", "%u kept, %u dropped",
                       (unsigned)snapshot->messages.count, (unsigned)snapshot->messages.dropped);
    fb_draw_status_row(state, layout, &y, FB_TONE_NORMAL, "Devices", "%zu in range",
                       snapshot->device_count);

    y += layout->line / 2;
    if (y + layout->line <= layout->footer_y) {
        fb_draw_text(state, FB_MARGIN, y, mesh_ui_input_quit_hint(), state->scale, k_fb_dim);
    }
}

/* "Save <section>?" for the sections whose write can cut this client off, and "Reboot the
   radio?" and its siblings for the Radio actions section. Which of the two it is standing in
   front of is nav->confirm_action; all three strings come from settings.c. */
static void fb_render_confirm(const struct mesh_ui_backend_fb_state *state,
                              const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const enum mesh_ui_settings_section section =
        (enum mesh_ui_settings_section)nav->settings_section;
    const enum mesh_ui_settings_action confirmed =
        (enum mesh_ui_settings_action)nav->confirm_action;
    char title[96];
    mesh_ui_settings_confirm_title(section, nav->settings_channel, confirmed, title, sizeof title);
    fb_draw_title(state, layout, title);

    char text[256];
    mesh_ui_settings_confirm_text(section, confirmed, text, sizeof text);
    const int text_lines = 4;
    fb_draw_wrapped(state, layout->body_y, text, layout->cols, text_lines, k_fb_text);

    /* The two choices are a list of their own, below the wrapped body text. */
    struct fb_layout choices = *layout;
    choices.body_y = layout->body_y + text_lines * layout->line + layout->line / 2;
    struct fb_list list = fb_list_begin(&choices, 2U, nav->confirm_cursor);
    const char *const rows[] = {mesh_ui_settings_confirm_accept(confirmed), "Cancel"};
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        fb_list_row(state, &list, i, rows[i], i == 0U ? FB_TONE_ACCENT : FB_TONE_NORMAL);
    }
}

/* Settings: the section list, or one section's label/value rows. Editable rows show a
   pending edit in place of the radio's value with a marker until Y saves it. */
static void fb_render_settings(const struct mesh_ui_backend_fb_state *state,
                               const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_settings *settings = &snapshot->settings;
    const struct mesh_ui_handshake_state *handshake =
        snapshot->handshake_valid ? &snapshot->handshake : NULL;
    const bool section_open = (nav->settings_section != MESH_UI_SETTINGS_NO_SECTION);
    const enum mesh_ui_settings_section section =
        (enum mesh_ui_settings_section)nav->settings_section;

    /* The breadcrumb names every level that is open, so a module reads
       "Settings > Modules > Telemetry" and B has a visible target. */
    const char *const trail = nav->settings_parent == MESH_UI_SETTINGS_MODULES ? "Modules > " : "";
    char title[96];
    if (section_open && nav->settings_channel != MESH_UI_SETTINGS_NO_CHANNEL) {
        snprintf(title, sizeof title, "Settings > Channel %u%s", (unsigned)nav->settings_channel,
                 nav->settings_edit_count > 0U ? " (unsaved)" : "");
    } else if (section_open) {
        snprintf(title, sizeof title, "Settings > %s%s%s", trail,
                 mesh_ui_settings_section_name(section),
                 nav->settings_edit_count > 0U ? " (unsaved)" : "");
    } else {
        snprintf(title, sizeof title, "%s", "Settings");
    }
    fb_draw_title(state, layout, title);

    /* Every other section describes the radio, but About describes this client, so the tab
       stays usable with nothing connected: the section list still draws (About is the only
       row not greyed out) and opening About still works. Modules is let through for the
       reason the section list itself is - it is a list of what exists, not a read of the
       radio, and each of its rows says "not loaded" on its own. */
    if (!settings->loaded && (handshake == NULL || !handshake->has_my_info) && section_open &&
        section != MESH_UI_SETTINGS_ABOUT && section != MESH_UI_SETTINGS_MODULES) {
        fb_draw_empty(state, layout, "Connect to a radio to read its settings");
        return;
    }

    const uint32_t count = section_open ? mesh_ui_settings_item_count(settings, handshake, section,
                                                                      nav->settings_channel)
                                        : mesh_ui_settings_root_count();
    if (count == 0U) {
        fb_draw_empty(state, layout, "Not sent by the radio yet; X to refresh");
        return;
    }

    /* Label column: a fixed width so values line up, capped for narrow scales. */
    const size_t label_cols = fb_field_label_cols(layout, 20U);
    struct fb_list list = fb_list_begin(layout, count, nav->cursor[MESH_UI_SCREEN_SETTINGS]);
    struct mesh_ui_line line;
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        if (section_open) {
            struct mesh_ui_settings_item item;
            if (!mesh_ui_settings_item(settings, handshake, nav->settings_edits,
                                       nav->settings_edit_count, section, nav->settings_channel, i,
                                       &item)) {
                break;
            }
            /* A heading names the group below it: dimmed, no marker, and no value column -
               the same row the node detail draws, so the two screens stay identical. */
            if (item.kind == MESH_UI_SETTING_HEADING) {
                mesh_ui_line_reset(&line);
                mesh_ui_line_printf(&line, "%s", item.label);
                fb_list_row_line(state, &list, i, &line, FB_TONE_DIM);
                continue;
            }
            /* Editable rows carry a marker so the eye can tell what Left/Right will act on;
               channel rows open with A. */
            const char *marker = item.dirty                            ? "* "
                                 : item.field != MESH_UI_FIELD_NONE    ? "> "
                                 : item.kind == MESH_UI_SETTING_ACTION ? "> "
                                                                       : "  ";
            fb_list_field_row(state, &list, i, item.label, label_cols, marker, item.value,
                              item.dirty ? FB_TONE_STRONG : FB_TONE_NORMAL);
        } else {
            const enum mesh_ui_settings_section row = mesh_ui_settings_root_at(i);
            const bool loaded = mesh_ui_settings_section_loaded(settings, handshake, row);
            fb_list_field_row(state, &list, i, mesh_ui_settings_section_name(row), label_cols, "",
                              loaded ? "" : "not loaded", loaded ? FB_TONE_NORMAL : FB_TONE_DIM);
        }
    }
}

void fb_render_snapshot(struct mesh_ui_backend_fb_state *state,
                        const struct mesh_ui_snapshot *snapshot) {
    fb_clear(state, k_fb_bg);

    struct fb_layout layout;
    memset(&layout, 0, sizeof layout);
    layout.small = state->scale > FB_MIN_SCALE ? state->scale - 1 : FB_MIN_SCALE;
    layout.line = fb_line_adv(state->scale);
    layout.cols = fb_cols(state, state->scale);

    fb_draw_tabs(state, snapshot, &layout);

    const int footer_height = 2 * fb_line_adv(layout.small) + FB_MARGIN;
    layout.footer_y = (int)state->var.yres - footer_height;
    const int body_height = layout.footer_y - layout.body_y - FB_MARGIN / 2;
    layout.rows = body_height > 0 ? (uint32_t)(body_height / layout.line) : 0U;

    const char *hint = "A select  B back  Left/Right tabs";
    if (snapshot->nav.confirm_open) {
        hint = "Up/Down choose  A confirm  B cancel";
        fb_render_confirm(state, snapshot, &layout);
        fb_draw_footer(state, snapshot, &layout, hint);
        return;
    }
    if (snapshot->nav.picker_open) {
        hint = "A choose  B cancel  Up/Down move  L/R jump 10";
        fb_render_picker(state, snapshot, &layout);
        fb_draw_footer(state, snapshot, &layout, hint);
        return;
    }
    if (snapshot->nav.keyboard_open) {
        if (snapshot->nav.keyboard_passkey) {
            hint = snapshot->nav.pairing_confirm ? "START confirm  B cancel pairing"
                                                 : "A type digits  START pair  B cancel";
        } else {
            hint = snapshot->nav.keyboard_field != MESH_UI_FIELD_NONE
                       ? "A type  B delete  X shift  Y space  START done"
                       : "A type  B delete  X shift  Y space  START send";
        }
        fb_render_keyboard(state, snapshot, &layout);
        fb_draw_footer(state, snapshot, &layout, hint);
        return;
    }
    if (snapshot->nav.compose_open) {
        hint = "A send / type  B back to the conversation";
        fb_render_compose(state, snapshot, &layout);
        fb_draw_footer(state, snapshot, &layout, hint);
        return;
    }
    switch (snapshot->nav.screen) {
    case MESH_UI_SCREEN_MESSAGES:
        if (!snapshot->nav.thread_open) {
            hint = "A open  Y new message  L/R tabs";
            fb_render_conversations(state, snapshot, &layout);
        } else {
            hint = snapshot->nav.inbox ? "A open conversation  B back  L/R tabs"
                                       : "A reply  Y write  B back  L/R tabs";
            fb_render_thread(state, snapshot, &layout);
        }
        break;
    case MESH_UI_SCREEN_NODES:
        hint = snapshot->nav.node_remove_armed  ? "A again to remove this node  B cancel"
               : snapshot->nav.node_detail_open ? "A select  B back  X pin  Y write  L/R tabs"
                                                : "A open node  X pin  Y write  L/R tabs";
        fb_render_nodes(state, snapshot, &layout);
        break;
    case MESH_UI_SCREEN_DEVICES:
        hint = snapshot->nav.devices_forget_armed ? "Y again to forget this node  B cancel"
                                                  : "A connect  X disconnect  Y forget  L/R tabs";
        fb_render_devices(state, snapshot, &layout);
        break;
    case MESH_UI_SCREEN_SETTINGS:
        if (snapshot->nav.settings_section == MESH_UI_SETTINGS_NO_SECTION) {
            hint = "A open  X refresh  L/R tabs";
        } else if (snapshot->nav.settings_discard_armed) {
            hint = "B again to discard  Y save";
        } else if (snapshot->nav.settings_edit_count > 0U) {
            hint = "Left/Right/A edit  Y save  B discard  L1/R1 tabs";
        } else if (snapshot->nav.settings_section == MESH_UI_SETTINGS_CHANNELS &&
                   snapshot->nav.settings_channel == MESH_UI_SETTINGS_NO_CHANNEL) {
            hint = "A open channel  B back  X refresh  L1/R1 tabs";
        } else if (snapshot->nav.settings_section == MESH_UI_SETTINGS_MODULES) {
            /* A list, not a section: nothing on it is editable, so the edit keys would be
               advertising a press that does nothing. The same branch the channel list has. */
            hint = "A open module  B back  X refresh  L1/R1 tabs";
        } else if (snapshot->nav.settings_section == MESH_UI_SETTINGS_ABOUT) {
            /* Nothing here is editable and nothing here comes from the radio, so neither the
               edit keys nor X mean anything. */
            hint = "A run the highlighted row  B back  L1/R1 tabs";
        } else {
            hint = "Left/Right/A edit  B back  X refresh  L1/R1 tabs";
        }
        fb_render_settings(state, snapshot, &layout);
        break;
    case MESH_UI_SCREEN_STATUS:
    default:
        hint = "L/R tabs";
        fb_render_status(state, snapshot, &layout);
        break;
    }

    fb_draw_footer(state, snapshot, &layout, hint);
}
