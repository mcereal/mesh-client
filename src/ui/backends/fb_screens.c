#define _POSIX_C_SOURCE 200809L

/*
 * One renderer per screen, plus the frame that dispatches between them.
 *
 * Each takes the immutable snapshot and a layout, and draws - no state is kept between frames,
 * which is why the scroll window is derived every time (fb_first_visible) rather than
 * remembered. Adding a screen is a fb_render_* here and a case in fb_render_snapshot().
 */

#include "fb_internal.h"

#include "mesh/ui/emoji.h"
#include "mesh/ui/font5x7.h"
#include "mesh/ui/input.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <string.h>
#include "mesh/core/message.h"
#include "mesh/ui/node_detail.h"

#include <stdarg.h>
#include <stdlib.h>
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
    if (snapshot->messages.dropped > 0U) {
        snprintf(title, sizeof title, "Messages (%u, +%u older)", count,
                 (unsigned)snapshot->messages.dropped);
    } else {
        snprintf(title, sizeof title, "Messages (%u)", count);
    }
    fb_draw_title(state, layout, title);
    if (count == 0U) {
        fb_draw_empty(state, layout, "Connect to a node to see conversations.");
        return;
    }

    /* Each conversation is a name line plus a preview line. */
    const uint32_t per_row = 2U;
    const uint32_t visible = layout->rows / per_row > 0U ? layout->rows / per_row : 1U;
    const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_MESSAGES] < count
                                ? nav->cursor[MESH_UI_SCREEN_MESSAGES]
                                : count - 1U;
    const uint32_t first = fb_first_visible(cursor, count, visible);

    int y = layout->body_y;
    char line[300];
    char age[8];
    for (uint32_t i = first; i < count && i < first + visible; ++i) {
        struct mesh_ui_conversation conversation;
        if (!mesh_ui_nav_conversation_at(&view, i, &conversation)) {
            break;
        }
        const bool is_new = (conversation.kind == MESH_UI_CONVERSATION_NEW);
        struct fb_rgb color = k_fb_text;
        if (conversation.kind == MESH_UI_CONVERSATION_CHANNEL ||
            conversation.kind == MESH_UI_CONVERSATION_ALL) {
            color = k_fb_accent;
        } else if (is_new) {
            color = k_fb_dim;
        }

        const bool unread = (conversation.unread > 0U);
        if (is_new) {
            snprintf(line, sizeof line, "+ %s", conversation.name);
        } else {
            /* The right-hand column says one thing only: how many messages are waiting, or
               else how long ago the last one arrived. A bare message count there read as an
               unread badge, and radios with no clock set report rx_time 0, so the age was a
               bare "?" - between them the row said nothing anyone could act on. The total is
               still in the conversation's own title once it is open. */
            char right[24];
            right[0] = '\0';
            if (unread) {
                snprintf(right, sizeof right, "%u new", (unsigned)conversation.unread);
            } else if (conversation.last_time != 0U) {
                fb_format_age(conversation.last_time, age, sizeof age);
                snprintf(right, sizeof right, "%s", age);
            }
            const size_t right_len = fb_width(right);
            snprintf(line, sizeof line, "%s%s", unread ? "* " : "  ", conversation.name);
            fb_fit(line, layout->cols > right_len + 1U ? layout->cols - right_len - 1U : 8U);
            if (right_len > 0U) {
                /* Pad by columns, append at the byte end: a name holding emoji has fewer
                   columns than bytes, and padding on the byte count pushes the metrics off
                   the right edge. */
                const size_t pad = layout->cols > fb_width(line) + right_len
                                       ? layout->cols - fb_width(line) - right_len
                                       : 1U;
                snprintf(line + strlen(line), sizeof line - strlen(line), "%*s%s", (int)pad, "",
                         right);
            }
        }
        fb_fit(line, layout->cols);
        /* Unread is drawn bright; so is the open thread, so B lands somewhere recognisable. */
        if (unread || mesh_ui_nav_conversation_is_open(nav, &conversation)) {
            color = k_fb_white;
        }
        fb_draw_row(state, y, line, color, i == cursor);
        y += layout->line;

        if (!is_new) {
            if (conversation.preview[0] != '\0') {
                snprintf(line, sizeof line, "  %s%s", conversation.preview_outbound ? "> " : "",
                         conversation.preview);
            } else {
                snprintf(line, sizeof line, "%s", "  no messages yet");
            }
            fb_fit(line, layout->cols);
            fb_draw_text(state, FB_MARGIN, y, line, state->scale, k_fb_dim);
        }
        y += layout->line;
    }
}

/* Level two: the messages in the open thread. */
static void fb_render_thread(const struct mesh_ui_backend_fb_state *state,
                             const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_message_list *messages = &snapshot->messages;
    const struct mesh_ui_nav *nav = &snapshot->nav;

    uint32_t indices[MESH_UI_MAX_MESSAGES];
    const uint32_t count =
        mesh_ui_nav_filter_messages(nav, messages, indices, MESH_UI_MAX_MESSAGES);

    char convo[MESH_UI_NAV_TARGET_NAME_MAX];
    mesh_ui_nav_conversation_name(nav, convo, sizeof convo);
    char title[96];
    if (nav->inbox && messages->dropped > 0U) {
        snprintf(title, sizeof title, "%s (%u, +%u older)", convo, count,
                 (unsigned)messages->dropped);
    } else if (nav->inbox) {
        snprintf(title, sizeof title, "%s (%u)", convo, count);
    } else {
        snprintf(title, sizeof title, "%s (%u)  %s", convo, count,
                 nav->target_node == MESH_MESSAGE_BROADCAST_ADDR ? "channel" : "direct");
    }
    fb_draw_title(state, layout, title);

    if (count == 0U) {
        fb_draw_empty(state, layout,
                      nav->inbox ? "No messages yet. B goes back to the list."
                                 : "Nothing here yet. Y writes one, B goes back.");
        return;
    }

    /* The bottom of the body is a detail pane for the selected message: full text, sender,
       channel and time, since the list rows are clipped to one line. */
    const int detail_lines = 3;
    uint32_t list_rows = layout->rows > (uint32_t)detail_lines + 1U
                             ? layout->rows - (uint32_t)detail_lines - 1U
                             : 1U;
    const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_MESSAGES] < count
                                ? nav->cursor[MESH_UI_SCREEN_MESSAGES]
                                : count - 1U;
    const uint32_t first = fb_first_visible(cursor, count, list_rows);

    int y = layout->body_y;
    /* Text (233 bytes at most), the peer, and now a failure reason; fb_fit clips it after. */
    char line[400];
    for (uint32_t i = first; i < count && i < first + list_rows; ++i) {
        const struct mesh_ui_message *message = &messages->entries[indices[i]];
        const bool outbound = (message->direction == MESH_MESSAGE_OUTBOUND);
        const char *peer = message->peer_name[0] != '\0' ? message->peer_name : "?";
        /* A failed message says why: "!!" alone leaves the user with no idea whether to move,
           retry, or fix a key, and those are different problems. */
        char tag[48] = "";
        if (outbound && message->ack == MESH_MESSAGE_ACK_FAILED) {
            snprintf(tag, sizeof tag, " !! %s",
                     mesh_message_ack_error_to_string(message->ack_error));
        } else if (outbound && message->ack != MESH_MESSAGE_ACK_NONE) {
            snprintf(tag, sizeof tag, " %s",
                     message->ack == MESH_MESSAGE_ACK_DELIVERED ? "ok" : "..");
        }
        /* In the inbox, say where a line belongs; inside a conversation that is the title. */
        char where[16] = "";
        if (nav->inbox) {
            if (message->broadcast) {
                snprintf(where, sizeof where, " #%u", (unsigned)message->channel);
            } else {
                snprintf(where, sizeof where, " dm");
            }
        }
        snprintf(line, sizeof line, "%s%s%s%s: %s", outbound ? ">" : "<", peer, where, tag,
                 message->text);
        fb_fit(line, layout->cols);
        fb_draw_row(state, y, line, outbound ? k_fb_outbound : k_fb_inbound, i == cursor);
        y += layout->line;
    }

    /* Detail pane. */
    const struct mesh_ui_message *selected = &messages->entries[indices[cursor]];
    y = layout->body_y + (int)list_rows * layout->line + layout->line / 2;
    fb_fill_rect(state, FB_MARGIN, y - state->scale, (int)state->var.xres - 2 * FB_MARGIN,
                 state->scale / 2 > 0 ? state->scale / 2 : 1, k_fb_cursor_bg);
    y += state->scale * 2;

    char clock[8];
    fb_format_clock(selected->rx_time, clock, sizeof clock);
    snprintf(line, sizeof line, "%s %s%s ch%u%s%s",
             selected->direction == MESH_MESSAGE_OUTBOUND ? "To" : "From",
             selected->peer_name[0] != '\0' ? selected->peer_name : "?",
             selected->broadcast ? " (all)" : "", (unsigned)selected->channel,
             clock[0] != '\0' ? " " : "", clock);
    fb_fit(line, layout->cols);
    fb_draw_text(state, FB_MARGIN, y, line, state->scale, k_fb_dim);
    y += layout->line;
    fb_draw_wrapped(state, y, selected->text, layout->cols, detail_lines - 1, k_fb_text);
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

    const uint32_t cursor =
        nav->cursor[MESH_UI_SCREEN_NODES] < count ? nav->cursor[MESH_UI_SCREEN_NODES] : count - 1U;
    const uint32_t first = fb_first_visible(cursor, count, layout->rows);

    size_t label_cols = 16U;
    if (layout->cols < 40U) {
        label_cols = layout->cols / 2U;
    }

    int y = layout->body_y;
    char line[160];
    for (uint32_t i = first; i < count && i < first + layout->rows; ++i) {
        const struct mesh_ui_node_item *item = &items[i];
        struct fb_rgb color = k_fb_text;
        if (item->kind == MESH_UI_NODE_ROW_HEADING) {
            snprintf(line, sizeof line, "%s", item->label);
            color = k_fb_dim;
        } else if (item->kind == MESH_UI_NODE_ROW_ACTION) {
            snprintf(line, sizeof line, "> %s", item->label);
            color = k_fb_accent;
        } else {
            /* Pad by columns, not bytes: a name or short name in the value can be emoji. */
            snprintf(line, sizeof line, "%s", item->label);
            for (size_t width = fb_width(line); width < label_cols; ++width) {
                const size_t used = strlen(line);
                if (used + 2U >= sizeof line) {
                    break;
                }
                line[used] = ' ';
                line[used + 1U] = '\0';
            }
            snprintf(line + strlen(line), sizeof line - strlen(line), "  %s", item->value);
        }
        fb_fit(line, layout->cols);
        fb_draw_row(state, y, line, color, i == cursor);
        y += layout->line;
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
        snprintf(title, sizeof title, "Nodes (%u)", count);
    }
    fb_draw_title(state, layout, title);

    const uint32_t cursor =
        nav->cursor[MESH_UI_SCREEN_NODES] < count ? nav->cursor[MESH_UI_SCREEN_NODES] : count - 1U;
    const uint32_t first = fb_first_visible(cursor, count, layout->rows);
    const uint32_t me = hs->has_my_info ? hs->my_info.node_num : 0U;

    int y = layout->body_y;
    char line[160];
    char right[32];
    char age[8];
    for (uint32_t i = first; i < count && i < first + layout->rows; ++i) {
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

        /* Left part is clipped so the right-aligned metrics always fit. */
        const size_t right_len = fb_width(right);
        size_t left_cols = layout->cols > right_len + 1U ? layout->cols - right_len - 1U : 8U;
        /* The marker column: ourselves, then pinned. A star sprite rather than an ASCII
           stand-in because the row is already measured in cells and drawn through the emoji
           walker, so it costs one column exactly like the '*' does. */
        const char *marker = (me != 0U && node->node_id == me) ? "*"
                             : node->is_favorite               ? "\xE2\xAD\x90"
                                                               : " ";
        /* "%-4s" pads to four bytes, so an emoji short name - four bytes, one column - comes
           out of it a column wide instead of four. Pad the field by columns instead. */
        snprintf(line, sizeof line, "%s%s", marker, short_name);
        for (size_t width = fb_width(short_name); width < 4U; ++width) {
            const size_t used = strlen(line);
            if (used + 2U >= sizeof line) {
                break;
            }
            line[used] = ' ';
            line[used + 1U] = '\0';
        }
        snprintf(line + strlen(line), sizeof line - strlen(line), " %s", long_name);
        fb_fit(line, left_cols);
        const size_t pad = layout->cols > fb_width(line) + right_len
                               ? layout->cols - fb_width(line) - right_len
                               : 1U;
        snprintf(line + strlen(line), sizeof line - strlen(line), "%*s%s", (int)pad, "", right);

        fb_draw_row(state, y, line, (node->node_id == nav->target_node) ? k_fb_accent : k_fb_text,
                    i == cursor);
        y += layout->line;
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

    const uint32_t count = mesh_ui_nav_compose_row_count();
    const uint32_t cursor = nav->compose_cursor < count ? nav->compose_cursor : count - 1U;
    const uint32_t first = fb_first_visible(cursor, count, layout->rows);

    int y = layout->body_y;
    char line[300];
    for (uint32_t i = first; i < count && i < first + layout->rows; ++i) {
        if (i == MESH_UI_COMPOSE_ROW_DRAFT) {
            if (nav->draft[0] != '\0') {
                snprintf(line, sizeof line, "Draft: %s", nav->draft);
            } else {
                snprintf(line, sizeof line, "%s", "[ Type a message ]");
            }
            fb_fit(line, layout->cols);
            fb_draw_row(state, y, line, k_fb_accent, i == cursor);
        } else {
            snprintf(line, sizeof line, "  %s",
                     mesh_ui_canned_text(i - MESH_UI_COMPOSE_FIRST_CANNED));
            fb_fit(line, layout->cols);
            fb_draw_row(state, y, line, k_fb_text, i == cursor);
        }
        y += layout->line;
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
    snprintf(title, sizeof title, "Send to (%u)", count);
    fb_draw_title(state, layout, title);
    if (count == 0U) {
        fb_draw_empty(state, layout, "No channels or nodes known yet.");
        return;
    }

    const uint32_t cursor = nav->picker_cursor < count ? nav->picker_cursor : count - 1U;
    const uint32_t first = fb_first_visible(cursor, count, layout->rows);
    int y = layout->body_y;
    char name[96];
    char line[160];
    for (uint32_t i = first; i < count && i < first + layout->rows; ++i) {
        uint32_t node = 0U;
        uint8_t channel = 0U;
        if (!mesh_ui_nav_picker_row(&view, i, &node, &channel, name, sizeof name)) {
            break;
        }
        const bool is_channel = (node == MESH_MESSAGE_BROADCAST_ADDR);
        const bool current =
            (node == nav->target_node) && (!is_channel || channel == nav->target_channel);
        snprintf(line, sizeof line, "%c %s%s", current ? '*' : ' ', name,
                 is_channel ? "  (channel)" : "");
        fb_fit(line, layout->cols);
        fb_draw_row(state, y, line, is_channel ? k_fb_accent : k_fb_text, i == cursor);
        y += layout->line;
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
    fb_fit(title, layout->cols);
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
    const size_t draft_width = fb_width(draft);
    if (draft_width > visible) {
        shown = draft + mesh_ui_text_cell_offset(draft, draft_width - visible);
    }
    fb_draw_wrapped(state, y, shown, layout->cols, box_lines, k_fb_white);
    y += box_lines * line;

    char meter[32];
    snprintf(meter, sizeof meter, "%zu/%zu", strlen(nav->draft), draft_cap);
    fb_draw_text(
        state, (int)state->var.xres - FB_MARGIN - (int)fb_width(meter) * fb_char_adv(layout->small),
        y, meter, layout->small, k_fb_dim);
    y += fb_line_adv(layout->small) + scale;

    /* Grid. */
    const int grid_w = (int)state->var.xres - 2 * FB_MARGIN;
    const int cell_w = grid_w / (int)MESH_UI_KB_COLS;
    const int cell_h = line + 2 * scale;
    const int adv = fb_char_adv(scale);
    for (unsigned row = 0; row < MESH_UI_KB_CHAR_ROWS; ++row) {
        for (unsigned col = 0; col < MESH_UI_KB_COLS; ++col) {
            const char ch = mesh_ui_kb_char((enum mesh_ui_kb_layer)nav->kb_layer, row, col);
            const int x = FB_MARGIN + (int)col * cell_w;
            const bool selected = (nav->kb_row == row && nav->kb_col == col);
            if (selected) {
                fb_fill_rect(state, x, y, cell_w - scale, cell_h - scale, k_fb_tab_active_bg);
            }
            if (ch != '\0') {
                fb_draw_glyph(state, x + (cell_w - adv) / 2, y + scale, (uint32_t)(unsigned char)ch,
                              scale, selected ? k_fb_white : k_fb_text);
            }
        }
        y += cell_h;
    }

    /* Action row: five wide keys. */
    const int action_w = grid_w / (int)MESH_UI_KB_ACTIONS;
    for (unsigned col = 0; col < MESH_UI_KB_ACTIONS; ++col) {
        const char *label = mesh_ui_kb_action_label(nav, (enum mesh_ui_kb_action)col);
        const int x = FB_MARGIN + (int)col * action_w;
        const bool selected = (nav->kb_row == MESH_UI_KB_CHAR_ROWS && nav->kb_col == col);
        fb_fill_rect(state, x, y, action_w - scale, cell_h - scale,
                     selected ? k_fb_tab_active_bg : k_fb_cursor_bg);
        const int text_w = (int)fb_width(label) * adv;
        fb_draw_text(state, x + (action_w - text_w) / 2, y + scale, label, scale,
                     selected ? k_fb_white : k_fb_text);
    }
}

static void fb_render_devices(const struct mesh_ui_backend_fb_state *state,
                              const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    char title[96];
    snprintf(title, sizeof title, "Devices (%zu)", snapshot->device_count);
    fb_draw_title(state, layout, title);

    if (snapshot->device_count == 0U) {
        fb_draw_empty(state, layout, "Scanning for Meshtastic nodes...");
        return;
    }

    const uint32_t count = (uint32_t)snapshot->device_count;
    const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_DEVICES] < count
                                ? nav->cursor[MESH_UI_SCREEN_DEVICES]
                                : count - 1U;
    const uint32_t first = fb_first_visible(cursor, count, layout->rows);

    int y = layout->body_y;
    char line[160];
    for (uint32_t i = first; i < count && i < first + layout->rows; ++i) {
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

        /* A USB port has no RSSI to show; the badge is what tells the two kinds apart. */
        if (device->kind == (uint8_t)MESH_UI_DEVICE_SERIAL) {
            snprintf(line, sizeof line, "%c %s  USB%s", device->connected ? '*' : ' ', name, badge);
        } else {
            snprintf(line, sizeof line, "%c %s  %ddBm%s", device->connected ? '*' : ' ', name,
                     (int)device->rssi, badge);
        }
        fb_fit(line, layout->cols);
        struct fb_rgb colour = k_fb_text;
        if (device->connected) {
            colour = k_fb_good;
        } else if (nav->devices_forget_armed && nav->devices_forget_row == i) {
            colour = k_fb_bad;
        }
        fb_draw_row(state, y, line, colour, i == cursor);
        y += layout->line;
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

/*
 * One Status row, dropped silently once the body is full. The screen packs a variable number
 * of lines - the mesh-health block only exists once the radio has reported - and the body fits
 * eleven rows at the largest scale, so the alternative to clipping here is a layout that
 * overwrites its own footer on somebody's device.
 */
static void fb_status_line(const struct mesh_ui_backend_fb_state *state,
                           const struct fb_layout *layout, int *y, struct fb_rgb color,
                           const char *label, const char *fmt, ...) {
    if (*y + layout->line > layout->footer_y) {
        return;
    }
    char line[192];
    int used = snprintf(line, sizeof line, "%-11s ", label);
    if (used < 0 || (size_t)used >= sizeof line) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(line + used, sizeof line - (size_t)used, fmt, args);
    va_end(args);
    fb_fit(line, layout->cols);
    fb_draw_text(state, FB_MARGIN, *y, line, state->scale, color);
    *y += layout->line;
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

    fb_status_line(state, layout, &y, k_fb_text, "Transport", "%s",
                   snapshot->transport_status[0] != '\0' ? snapshot->transport_status : "starting");

    const struct mesh_ui_device *connected = NULL;
    for (size_t i = 0; i < snapshot->device_count; ++i) {
        if (snapshot->devices[i].connected) {
            connected = &snapshot->devices[i];
            break;
        }
    }
    fb_status_line(state, layout, &y, connected != NULL ? k_fb_good : k_fb_bad, "Radio", "%s",
                   connected != NULL
                       ? (connected->name[0] != '\0' ? connected->name : connected->identifier)
                       : "not connected");

    if (snapshot->handshake_valid) {
        const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
        fb_status_line(state, layout, &y, k_fb_text, "Sync", "%s%s",
                       hs->config_complete ? "complete"
                                           : (hs->request_in_flight ? "in progress" : "idle"),
                       hs->cached ? " (cached)" : "");
        if (hs->has_my_info) {
            fb_status_line(state, layout, &y, k_fb_text, "My node", "%s !%08x", hs->my_short_name,
                           hs->my_info.node_num);
        }
        /* One line for the NodeDB, and LocalStats' online count when the radio has sent it:
           "132 nodes" alone says nothing about how much of that mesh is still alive. */
        const struct mesh_ui_radio_stats *stats = &snapshot->settings.stats;
        if (hs->has_my_info && stats->valid && stats->num_online_nodes > 0U) {
            fb_status_line(state, layout, &y, k_fb_text, "NodeDB", "%u nodes, %u online",
                           hs->my_info.nodedb_entries, stats->num_online_nodes);
        } else if (hs->has_my_info) {
            fb_status_line(state, layout, &y, k_fb_text, "NodeDB", "%u nodes, %u reboots",
                           hs->my_info.nodedb_entries, hs->my_info.reboot_count);
        }
        if (hs->primary_channel[0] != '\0') {
            fb_status_line(state, layout, &y, k_fb_text, "Channel", "%s", hs->primary_channel);
        }
    } else {
        fb_status_line(state, layout, &y, k_fb_dim, "Sync", "%s", "waiting for a radio");
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
        struct fb_rgb air_color = k_fb_text;
        if (have_util) {
            air_color = util_value >= 50.0f ? k_fb_bad : util_value >= 25.0f ? k_fb_accent : k_fb_good;
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
            fb_status_line(state, layout, &y, air_color, "Airtime", "%s busy, %s tx, %d dBm floor",
                           util, tx, stats->noise_floor);
        } else {
            fb_status_line(state, layout, &y, air_color, "Airtime", "%s busy, %s tx", util, tx);
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
        struct fb_rgb battery_color =
            (have_battery && metrics->battery_level <= 20U) ? k_fb_bad : k_fb_text;
        fb_status_line(state, layout, &y, battery_color, "Battery", "%s%s",
                       buffer[0] != '\0' ? buffer : "unknown", second);
    }

    if (stats->valid) {
        fb_status_line(state, layout, &y, k_fb_text, "Packets", "%u tx, %u rx, %u relayed",
                       stats->num_packets_tx, stats->num_packets_rx, stats->num_tx_relay);
        /* Bad and dropped packets are the two numbers that explain a mesh that "works but
           loses messages", so they get their own row instead of being folded into Packets. */
        const bool losing = stats->num_packets_rx_bad > 0U || stats->num_tx_dropped > 0U;
        fb_status_line(state, layout, &y, losing ? k_fb_accent : k_fb_dim, "Dropped",
                       "%u bad rx, %u dupe, %u tx", stats->num_packets_rx_bad, stats->num_rx_dupe,
                       stats->num_tx_dropped);
        if (stats->has_heap) {
            fb_status_line(state, layout, &y, stats->heap_free_bytes < 20480U ? k_fb_accent : k_fb_dim,
                           "Heap", "%u KB free of %u KB", stats->heap_free_bytes / 1024U,
                           stats->heap_total_bytes / 1024U);
        }
    } else if (snapshot->handshake_valid) {
        fb_status_line(state, layout, &y, k_fb_dim, "Mesh", "%s",
                       "waiting for the radio's first report");
    }

    y += layout->line / 2;
    fb_status_line(state, layout, &y, k_fb_text, "Messages", "%u kept, %u dropped",
                   (unsigned)snapshot->messages.count, (unsigned)snapshot->messages.dropped);
    fb_status_line(state, layout, &y, k_fb_text, "Devices", "%zu in range", snapshot->device_count);

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
    int y = layout->body_y;
    const int text_lines = 4;
    fb_draw_wrapped(state, y, text, layout->cols, text_lines, k_fb_text);
    y += text_lines * layout->line + layout->line / 2;

    const char *const rows[] = {mesh_ui_settings_confirm_accept(confirmed), "Cancel"};
    for (unsigned i = 0; i < 2U; ++i) {
        fb_draw_row(state, y, rows[i], i == 0U ? k_fb_accent : k_fb_text, nav->confirm_cursor == i);
        y += layout->line;
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

    char title[96];
    if (section_open && nav->settings_channel != MESH_UI_SETTINGS_NO_CHANNEL) {
        snprintf(title, sizeof title, "Settings > Channel %u%s", (unsigned)nav->settings_channel,
                 nav->settings_edit_count > 0U ? " (unsaved)" : "");
    } else if (section_open) {
        snprintf(title, sizeof title, "Settings > %s%s", mesh_ui_settings_section_name(section),
                 nav->settings_edit_count > 0U ? " (unsaved)" : "");
    } else {
        snprintf(title, sizeof title, "%s", "Settings");
    }
    fb_draw_title(state, layout, title);

    /* Every other section describes the radio, but About describes this client, so the tab
       stays usable with nothing connected: the section list still draws (About is the only
       row not greyed out) and opening About still works. */
    if (!settings->loaded && (handshake == NULL || !handshake->has_my_info) && section_open &&
        section != MESH_UI_SETTINGS_ABOUT) {
        fb_draw_empty(state, layout, "Connect to a radio to read its settings");
        return;
    }

    const uint32_t count = section_open ? mesh_ui_settings_item_count(settings, handshake, section,
                                                                      nav->settings_channel)
                                        : (uint32_t)MESH_UI_SETTINGS_SECTION_COUNT;
    if (count == 0U) {
        fb_draw_empty(state, layout, "Not sent by the radio yet; X to refresh");
        return;
    }
    const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_SETTINGS] < count
                                ? nav->cursor[MESH_UI_SCREEN_SETTINGS]
                                : count - 1U;
    const uint32_t first = fb_first_visible(cursor, count, layout->rows);

    /* Label column: a fixed width so values line up, capped for narrow scales. */
    size_t label_cols = 20U;
    if (layout->cols < 40U) {
        label_cols = layout->cols / 2U;
    }

    int y = layout->body_y;
    char line[160];
    for (uint32_t i = first; i < count && i < first + layout->rows; ++i) {
        struct fb_rgb color = k_fb_text;
        if (section_open) {
            struct mesh_ui_settings_item item;
            if (!mesh_ui_settings_item(settings, handshake, nav->settings_edits,
                                       nav->settings_edit_count, section, nav->settings_channel, i,
                                       &item)) {
                break;
            }
            /* Editable rows carry a marker so the eye can tell what Left/Right will act on;
               channel rows open with A. */
            const char *marker = item.dirty                            ? "* "
                                 : item.field != MESH_UI_FIELD_NONE    ? "> "
                                 : item.kind == MESH_UI_SETTING_ACTION ? "> "
                                                                       : "  ";
            snprintf(line, sizeof line, "%-*.*s %s%s", (int)label_cols, (int)label_cols, item.label,
                     marker, item.value);
            if (item.dirty) {
                color = k_fb_white;
            }
        } else {
            const enum mesh_ui_settings_section row = (enum mesh_ui_settings_section)i;
            const bool loaded = mesh_ui_settings_section_loaded(settings, handshake, row);
            snprintf(line, sizeof line, "%-*.*s %s", (int)label_cols, (int)label_cols,
                     mesh_ui_settings_section_name(row), loaded ? "" : "not loaded");
            if (!loaded) {
                color = k_fb_dim;
            }
        }
        fb_fit(line, layout->cols);
        fb_draw_row(state, y, line, color, i == cursor);
        y += layout->line;
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
