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
#include "mesh/i18n/strings.h"
#include "mesh/ui/emoji.h"
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

/* The radio this frame is attached to, or NULL. Three places asked it and each wrote the loop
   out again; the footer and the Status card also have to agree, because one of them saying
   "connected" while the other says "not connected" is the worst way to answer the question. */
static const struct mesh_ui_device *fb_connected_device(const struct mesh_ui_snapshot *snapshot) {
    for (size_t i = 0; i < snapshot->device_count; ++i) {
        if (snapshot->devices[i].connected) {
            return &snapshot->devices[i];
        }
    }
    return NULL;
}

/* What to call it: the advertised name when it has one, otherwise whatever we addressed it by. */
static const char *fb_device_label(const struct mesh_ui_device *device) {
    return device->name[0] != '\0' ? device->name : device->identifier;
}

/* ---- chrome ------------------------------------------------------------------------------ */

/*
 * The tab strip: a bar of its own, one chip per screen, then the rule that closes it off.
 *
 * The bar is the point. The strip used to float on the body's own ground, which left the tabs
 * reading as the first row of content rather than as the frame around it; a recessed tier
 * behind them says "this is chrome" before a word of it is read, which is what every phone's
 * navigation bar is doing. It is the theme's lowest surface, so a palette decides how far from
 * the ground that is - on the high-contrast theme it is barely anywhere, which is correct.
 */
static void fb_draw_tabs(const struct mesh_ui_backend_fb_state *state,
                         const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const int small = layout->small;
    const int margin = fb_margin(state);
    const int y = margin / 2 + small;
    const int line = fb_line_adv(state, small);
    const int bar_h = y + line;
    int x = margin / 2;

    fb_fill_rect(state, 0, 0, (int)state->var.xres, bar_h,
                 fb_color(state, MESH_UI_COLOR_SURFACE_LOW));

    for (int i = 0; i < MESH_UI_SCREEN_COUNT; ++i) {
        const enum mesh_ui_screen screen = (enum mesh_ui_screen)i;
        x = fb_draw_chip(state, x, y, mesh_ui_screen_name(screen), snapshot->nav.screen == screen,
                         small);
    }

    fb_draw_rule(state, 0, bar_h, (int)state->var.xres, small, MESH_UI_COLOR_RULE_STRONG);
    layout->body_y = bar_h + 2 * small + margin / 2;
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
    fb_draw_text(state, fb_margin(state), layout->footer_y, mesh_ui_line_text(&line), small,
                 fb_tone_color(state, MESH_UI_TONE_DIM));

    const struct mesh_ui_nav *nav = &snapshot->nav;
    enum mesh_ui_tone tone = MESH_UI_TONE_DIM;
    mesh_ui_line_reset(&line);
    if (nav->toast[0] != '\0') {
        mesh_ui_line_printf(&line, "%s", nav->toast);
        tone = MESH_UI_TONE_ACCENT;
    } else {
        const char *status = snapshot->transport_status[0] != '\0'
                                 ? snapshot->transport_status
                                 : mesh_str(MESH_STR_HEADER_TRANSPORT_STARTING);
        const struct mesh_ui_device *device = fb_connected_device(snapshot);
        if (device != NULL) {
            mesh_ui_line_str(&line, MESH_STR_HEADER_STATUS_CONNECTED, status,
                             fb_device_label(device));
            tone = MESH_UI_TONE_GOOD;
        } else {
            mesh_ui_line_str(&line, MESH_STR_HEADER_STATUS_QUIT, status, mesh_ui_input_quit_hint());
        }
    }
    mesh_ui_line_fit(&line, cols);
    fb_draw_text(state, fb_margin(state), layout->footer_y + fb_line_adv(state, small),
                 mesh_ui_line_text(&line), small, fb_tone_color(state, tone));
}

/* ---- screens ----------------------------------------------------------------------------- */

/* Level one of the Messages tab: all traffic, the channels, whoever we have direct messages
   with, and the way to start a new one. One conversation cell a row - see fb_widgets.h. */
static void fb_render_conversations(struct mesh_ui_backend_fb_state *state,
                                    const struct mesh_ui_snapshot *snapshot,
                                    struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    struct mesh_ui_store view;
    fb_store_view(snapshot, &view);

    const uint32_t count = mesh_ui_nav_conversation_count(&view);
    char title[96];
    fb_title_count(title, sizeof title, mesh_str(MESH_STR_TAB_MESSAGES), count,
                   snapshot->messages.dropped);
    fb_draw_title(state, layout, title);
    if (count == 0U) {
        fb_draw_empty(state, layout, mesh_str(MESH_STR_MESSAGES_EMPTY));
        return;
    }

    /*
     * Each conversation is one cell two body rows tall: the avatar, the name and the age, then
     * the last thing said with the unread count as a pill. The cell owns every pixel of that -
     * this loop only says which strings go in it and what each one means.
     */
    struct fb_list list =
        fb_list_begin_rows(layout, count, nav->cursor[MESH_UI_SCREEN_MESSAGES], 2U);
    char age[8];
    char badge[8];
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        struct mesh_ui_conversation conversation;
        if (!mesh_ui_nav_conversation_at(&view, i, &conversation)) {
            break;
        }
        const bool is_new = (conversation.kind == MESH_UI_CONVERSATION_NEW);
        const bool is_view = is_new || (conversation.kind == MESH_UI_CONVERSATION_ALL);

        /* A radio with no clock set reports rx_time 0, so the age column is simply empty
           rather than a bare "?" nobody can act on. */
        age[0] = '\0';
        if (conversation.last_time != 0U) {
            fb_format_age(conversation.last_time, age, sizeof age);
        }
        badge[0] = '\0';
        if (conversation.unread > 0U) {
            /* Past two figures a badge stops being a number and becomes a width, which is what
               every messenger's "99+" is for. */
            if (conversation.unread > 99U) {
                snprintf(badge, sizeof badge, "%s", mesh_str(MESH_STR_MESSAGES_UNREAD_OVERFLOW));
            } else {
                snprintf(badge, sizeof badge, "%u", (unsigned)conversation.unread);
            }
        }

        const struct fb_conversation cell = {
            .avatar = conversation.initials,
            .tint = conversation.tint,
            /* The two rows that are not somebody: a view over the others, and a button. */
            .accent = is_view,
            .name = conversation.name,
            .age = age,
            /* The one row that is a button rather than a conversation says what it does
               instead of what was last said in it. */
            .preview = is_new ? mesh_str(MESH_STR_MESSAGES_NEW_PREVIEW) : conversation.preview,
            .preview_outbound = conversation.preview_outbound,
            .badge = badge,
            .unread = (conversation.unread > 0U),
            .armed = mesh_ui_nav_conversation_is_armed(nav, &conversation),
            /* All traffic is accented because it is a view rather than somebody; a channel
               used to be too, and no longer needs to be now that its avatar carries the '#'.
               That frees the strong tone to mean what it means everywhere else on this
               screen: there is something here you have not read. */
            .name_tone = is_new                                            ? MESH_UI_TONE_DIM
                         : (conversation.kind == MESH_UI_CONVERSATION_ALL) ? MESH_UI_TONE_ACCENT
                         : (conversation.unread > 0U)                      ? MESH_UI_TONE_STRONG
                                                                           : MESH_UI_TONE_NORMAL,
        };
        fb_draw_conversation(state, &list, i, &cell);
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
            snprintf(out, out_len, "%s", mesh_str(MESH_STR_DATE_TODAY));
            return;
        }
        if (when.tm_year == today.tm_year && when.tm_yday + 1 == today.tm_yday) {
            snprintf(out, out_len, "%s", mesh_str(MESH_STR_DATE_YESTERDAY));
            return;
        }
    }
    /* "%e" pads a single-digit day with a space, which reads as a typo in a centred label. */
    char month[8];
    (void)strftime(month, sizeof month, "%b", &when);
    char weekday[8];
    (void)strftime(weekday, sizeof weekday, "%a", &when);
    mesh_str_format(out, out_len, MESH_STR_DATE_WEEKDAY_DAY_MONTH, weekday, when.tm_mday, month);
}

/* A day apart, or a long enough silence, is a break in the conversation; anything closer is the
   same exchange and gets no furniture between the messages. */
#define FB_THREAD_GAP_SECONDS 1800U /* 30 minutes: a new separator */
#define FB_THREAD_RUN_SECONDS                                                                      \
    300U /* 5 minutes: still the same run, so the name is not repeated                             \
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

/*
 * The reactions attached to one message, as a short chip like "\U0001F44D 3 \U0001F602".
 *
 * Gathered from the whole message list rather than from the filtered transcript, because they
 * were filtered out of it on purpose: a reaction is an annotation on its target, and the
 * transcript would otherwise carry a bubble containing nothing but an emoji. Identical emoji
 * are counted rather than repeated, so a channel where twenty nodes agree is one chip and a
 * number instead of twenty bubbles.
 *
 * Only the first character of the payload is taken. A reaction is one emoji by definition, and
 * the payload is radio text - a sender that puts a paragraph in one must not push the clock
 * off the end of the line.
 */
#define FB_THREAD_REACTION_KINDS 4U

static void fb_thread_reactions(const struct mesh_ui_snapshot *snapshot, uint32_t packet_id,
                                char *out, size_t out_len) {
    out[0] = '\0';
    if (packet_id == 0U) {
        return;
    }

    struct {
        char glyph[8];
        uint16_t count;
    } seen[FB_THREAD_REACTION_KINDS];
    size_t kinds = 0U;

    const uint32_t total = snapshot->messages.count > MESH_UI_MAX_MESSAGES
                               ? MESH_UI_MAX_MESSAGES
                               : snapshot->messages.count;
    for (uint32_t i = 0; i < total; ++i) {
        const struct mesh_ui_message *reaction = &snapshot->messages.entries[i];
        if (!reaction->is_reaction || reaction->reply_id != packet_id ||
            reaction->text[0] == '\0') {
            continue;
        }
        char glyph[8];
        const size_t take = mesh_ui_text_cell_offset(reaction->text, 1U);
        if (take == 0U || take >= sizeof glyph) {
            continue;
        }
        memcpy(glyph, reaction->text, take);
        glyph[take] = '\0';

        size_t slot = 0U;
        while (slot < kinds && strcmp(seen[slot].glyph, glyph) != 0) {
            ++slot;
        }
        if (slot == kinds) {
            if (kinds == FB_THREAD_REACTION_KINDS) {
                continue; /* a fifth kind; the four already shown are the story */
            }
            mesh_str_copy(seen[kinds].glyph, sizeof seen[kinds].glyph, glyph);
            seen[kinds].count = 0U;
            kinds++;
        }
        seen[slot].count++;
    }

    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    for (size_t i = 0; i < kinds; ++i) {
        /* The count is left off a lone reaction: "\U0001F44D 1" reads as a score. */
        if (seen[i].count > 1U) {
            mesh_ui_line_printf(&line, "%s%s%u", i > 0U ? " " : "", seen[i].glyph,
                                (unsigned)seen[i].count);
        } else {
            mesh_ui_line_printf(&line, "%s%s", i > 0U ? " " : "", seen[i].glyph);
        }
    }
    mesh_str_copy(out, out_len, mesh_ui_line_text(&line));
}

/*
 * Everything the screen decides about one message: what furniture it gets and what it says.
 *
 * `force_name` names the sender on a bubble that would otherwise inherit the name from the
 * message above it - which is what the first bubble on screen has to do, because the message
 * above it is not on screen to have said it.
 */
static void fb_thread_row_build(const struct mesh_ui_snapshot *snapshot, const uint32_t *indices,
                                uint32_t position, bool force_name, struct fb_thread_row *row) {
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

    /*
     * An alert and a detection arrive as text on a channel and are otherwise indistinguishable
     * from anything else said there, so they are always headed - whatever the screen would
     * have decided about naming, and even in a direct conversation where the title already
     * says who is talking. A run of identical-looking bubbles is exactly what a critical alert
     * must not be.
     */
    const bool labelled = message->kind != (uint8_t)MESH_MESSAGE_KIND_TEXT;
    row->bubble.alert = (message->kind == (uint8_t)MESH_MESSAGE_KIND_ALERT);

    if (labelled || ((starts_run || force_name) && (names_needed || (outbound && nav->inbox)))) {
        const char *peer = message->peer_name[0] != '\0' ? message->peer_name
                                                         : mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT);
        struct mesh_ui_line line;
        mesh_ui_line_reset(&line);
        if (outbound) {
            /* Ours in all-traffic still needs a destination: "sent" alone does not say to whom,
               and a broadcast and a DM look identical without it. */
            if (message->broadcast) {
                mesh_ui_line_printf(&line, "%s", mesh_str(MESH_STR_BUBBLE_SENT));
            } else {
                mesh_ui_line_str(&line, MESH_STR_BUBBLE_SENT_TO, peer);
            }
        } else {
            mesh_ui_line_printf(&line, "%s", peer);
        }
        /* All-traffic is several conversations at once, so each bubble says which one it is. */
        if (nav->inbox) {
            if (message->broadcast) {
                mesh_ui_line_str(&line, MESH_STR_BUBBLE_CHANNEL, (unsigned)message->channel);
            } else {
                mesh_ui_line_printf(&line, "%s", mesh_str(MESH_STR_BUBBLE_DIRECT));
            }
        }
        if (message->kind == (uint8_t)MESH_MESSAGE_KIND_ALERT) {
            mesh_ui_line_printf(&line, "%s", mesh_str(MESH_STR_BUBBLE_ALERT));
        } else if (message->kind == (uint8_t)MESH_MESSAGE_KIND_DETECTION) {
            mesh_ui_line_printf(&line, "%s", mesh_str(MESH_STR_BUBBLE_SENSOR));
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
            mesh_ui_line_printf(&meta, "%s%s %s", space, mesh_str(MESH_STR_BUBBLE_FAILED_MARK),
                                message->ack_error != 0U
                                    ? mesh_message_ack_error_to_string(message->ack_error)
                                    : mesh_str(MESH_STR_BUBBLE_STATE_FAILED));
        } else {
            mesh_ui_line_printf(&meta, "%s%s", space,
                                mesh_str(message->ack == MESH_MESSAGE_ACK_DELIVERED
                                             ? MESH_STR_BUBBLE_DELIVERED
                                             : MESH_STR_BUBBLE_PENDING));
        }
    }
    /*
     * A padlock on a message the radio decrypted with our key pair rather than with a channel
     * PSK. It only means anything on a direct message, and it is worth saying there: on a
     * channel still using the default key every node on the mesh holds that key, so a DM that
     * did *not* go out PKI-encrypted was readable by all of them, and nothing else on the
     * screen distinguishes the two.
     */
    if (message->pki_encrypted && !message->broadcast) {
        mesh_ui_line_printf(&meta, "%s\U0001F512", mesh_ui_line_width(&meta) > 0U ? " " : "");
    }

    /* Reactions ride on the meta line rather than taking a row: they are an annotation on this
       bubble, and a row of their own is the bubble they were filtered out of being. */
    char reactions[40];
    fb_thread_reactions(snapshot, message->packet_id, reactions, sizeof reactions);
    if (reactions[0] != '\0') {
        mesh_ui_line_printf(&meta, "%s%s", mesh_ui_line_width(&meta) > 0U ? " " : "", reactions);
    }

    mesh_str_copy(row->meta, sizeof row->meta, mesh_ui_line_text(&meta));
}

/* A bubble's height, clamped into the byte the transcript window measures in. */
static uint8_t fb_thread_height(const struct mesh_ui_backend_fb_state *state,
                                const struct fb_layout *layout, const struct fb_thread_row *row) {
    const uint32_t rows = fb_bubble_rows(state, layout, &row->bubble);
    return rows > 0xFFU ? 0xFFU : (uint8_t)rows;
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
        mesh_str_format(title, sizeof title, MESH_STR_THREAD_TITLE_OLDER, convo,
                        mesh_str(nav->target_node == MESH_MESSAGE_BROADCAST_ADDR
                                     ? MESH_STR_THREAD_KIND_CHANNEL
                                     : MESH_STR_THREAD_KIND_DIRECT),
                        (unsigned)snapshot->messages.dropped);
    } else {
        mesh_str_format(title, sizeof title, MESH_STR_THREAD_TITLE, convo,
                        mesh_str(nav->target_node == MESH_MESSAGE_BROADCAST_ADDR
                                     ? MESH_STR_THREAD_KIND_CHANNEL
                                     : MESH_STR_THREAD_KIND_DIRECT));
    }
    fb_draw_title(state, layout, title);

    if (count == 0U) {
        fb_draw_empty(state, layout,
                      mesh_str(nav->inbox ? MESH_STR_THREAD_EMPTY_INBOX : MESH_STR_THREAD_EMPTY));
        return;
    }

    /* Measure every message, then let the transcript say which of them are on screen. Heights
       come from the same component that draws them, so the window can never be a row out. */
    const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_MESSAGES];
    uint8_t heights[MESH_UI_MAX_MESSAGES];
    struct fb_thread_row row;
    for (uint32_t i = 0; i < count; ++i) {
        fb_thread_row_build(snapshot, indices, i, false, &row);
        heights[i] = fb_thread_height(state, layout, &row);
    }
    struct mesh_ui_transcript window =
        mesh_ui_transcript_window(heights, count, cursor, layout->rows);

    /*
     * The first bubble on screen always names its sender.
     *
     * A run that began above the window would otherwise arrive with its name suppressed - and so
     * would every bubble behind it, because each one only looks at the message before it in the
     * filtered log rather than at what is actually drawn. A channel viewport filled by one node's
     * burst then said nothing at all about who was talking, which is the one thing a channel
     * transcript is for.
     *
     * Naming it costs a row, and a taller first bubble can push the window's start later, so the
     * window and the named bubble are settled together rather than in sequence. It terminates: a
     * taller first item only ever moves `first` later, and the window's other two branches (the
     * whole transcript fits; the cursor has scrolled above it) do not depend on the heights at
     * all. Two passes is the normal case and the cap is a bound, not an expectation.
     */
    uint32_t named = count; /* count means "nothing forced yet" */
    for (uint32_t pass = 0U; pass < 4U && window.first != named; ++pass) {
        if (named < count) {
            fb_thread_row_build(snapshot, indices, named, false, &row);
            heights[named] =
                fb_thread_height(state, layout, &row); /* it was not the first after all */
        }
        named = window.first;
        fb_thread_row_build(snapshot, indices, named, true, &row);
        heights[named] = fb_thread_height(state, layout, &row);
        window = mesh_ui_transcript_window(heights, count, cursor, layout->rows);
    }
    /* Only force what the heights were settled against, so the draw can never disagree with the
       measure even if the loop ran out of passes. */
    const bool settled = (named == window.first);

    int y = layout->body_y + (int)window.pad * layout->line;
    for (uint32_t i = window.first; i < window.first + window.count && i < count; ++i) {
        fb_thread_row_build(snapshot, indices, i, settled && i == named, &row);
        row.bubble.selected = (i == cursor);
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
/* Mutable state, as every screen drawing a fb_list_item is: an item may carry a control
   that animates, and where such a control has got to is kept on the backend. */
static void fb_render_node_detail(struct mesh_ui_backend_fb_state *state,
                                  const struct mesh_ui_snapshot *snapshot,
                                  struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    const struct mesh_ui_node_summary *node = mesh_ui_node_detail_find(hs, nav->node_detail_node);
    if (node == NULL) {
        fb_draw_title(state, layout, mesh_str(MESH_STR_TAB_NODES));
        fb_draw_empty(state, layout, mesh_str(MESH_STR_NODES_GONE));
        return;
    }

    const bool is_self = hs->has_my_info && node->node_id == hs->my_info.node_num;
    char title[96];
    const char *name = node->long_name[0] != '\0'    ? node->long_name
                       : node->short_name[0] != '\0' ? node->short_name
                                                     : NULL;
    if (name != NULL) {
        mesh_str_format(title, sizeof title, MESH_STR_NODES_TITLE_DETAIL, name);
    } else {
        char fallback[24];
        mesh_str_format(fallback, sizeof fallback, MESH_STR_NODE_VAL_USER_ID_HEX, node->node_id);
        mesh_str_format(title, sizeof title, MESH_STR_NODES_TITLE_DETAIL, fallback);
    }
    fb_draw_title(state, layout, title);

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(
        node, is_self, (uint32_t)time(NULL), &snapshot->traceroute, nav->node_remove_armed,
        &snapshot->handshake, items, MESH_UI_NODE_ITEMS_MAX);
    if (count == 0U) {
        fb_draw_empty(state, layout, mesh_str(MESH_STR_NODES_DETAIL_EMPTY));
        return;
    }

    const size_t label_cols = fb_field_label_cols(state, layout, 16U);
    struct fb_list list = fb_list_begin(layout, count, nav->cursor[MESH_UI_SCREEN_NODES]);
    struct mesh_ui_line line;
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        const struct mesh_ui_node_item *item = &items[i];
        if (item->kind == MESH_UI_NODE_ROW_HEADING) {
            mesh_ui_line_reset(&line);
            mesh_ui_line_printf(&line, "%s", item->label);
            fb_list_row_line(state, &list, i, &line, MESH_UI_TONE_DIM);
        } else if (item->kind == MESH_UI_NODE_ROW_ACTION) {
            mesh_ui_line_reset(&line);
            mesh_ui_line_printf(&line, "> %s", item->label);
            fb_list_row_line(state, &list, i, &line, MESH_UI_TONE_ACCENT);
        } else {
            const struct fb_list_item row = {
                .label = item->label,
                .label_cols = label_cols,
                .marker = " ",
                .value = item->value,
                .tone = MESH_UI_TONE_NORMAL,
            };
            fb_list_item(state, &list, i, &row);
        }
    }
}

static void fb_render_nodes(struct mesh_ui_backend_fb_state *state,
                            const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    if (nav->node_detail_open) {
        fb_render_node_detail(state, snapshot, layout);
        return;
    }
    if (!snapshot->handshake_valid || snapshot->handshake.node_count == 0U) {
        fb_draw_title(state, layout, mesh_str(MESH_STR_TAB_NODES));
        fb_draw_empty(state, layout,
                      mesh_str(snapshot->handshake_valid ? MESH_STR_NODES_EMPTY_WAITING
                                                         : MESH_STR_NODES_EMPTY_DISCONNECTED));
        return;
    }

    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    const uint32_t count =
        hs->node_count > MESH_UI_MAX_HANDSHAKE_NODES ? MESH_UI_MAX_HANDSHAKE_NODES : hs->node_count;
    char title[96];
    /* Counted from the rows this screen is about to draw, so the two numbers are always in the
       same scope: the session roster holds twice what the UI carries, and a title reading
       "128 nodes, 200 off radio" would be arithmetic no screen should show. */
    const uint32_t off_radio = mesh_ui_handshake_off_radio(hs);
    if (hs->has_my_info && hs->my_info.nodedb_entries > count) {
        mesh_str_format(title, sizeof title, MESH_STR_NODES_TITLE_OF, count,
                        hs->my_info.nodedb_entries);
    } else if (off_radio > 0U) {
        /* The count the Status screen shows is the radio's; this one is ours, and after a
           NodeDB reset the two are nothing alike. Saying how much of the gap is nodes only we
           remember is what keeps "81 here, 2 there" from reading as a bug. */
        mesh_str_format(title, sizeof title, MESH_STR_NODES_TITLE_OFF_RADIO, count, off_radio);
    } else {
        fb_title_count(title, sizeof title, mesh_str(MESH_STR_TAB_NODES), count, 0U);
    }
    fb_draw_title(state, layout, title);

    const uint32_t me = hs->has_my_info ? hs->my_info.node_num : 0U;
    /* The discs come from the nav layer, which wants a store rather than the handshake alone -
       the same view the conversation list and the picker build, so all three ask one function. */
    struct mesh_ui_store view;
    fb_store_view(snapshot, &view);
    struct fb_list list = fb_list_begin(layout, count, nav->cursor[MESH_UI_SCREEN_NODES]);
    struct mesh_ui_line line;
    char right[32];
    char age[8];
    char initials[MESH_UI_CONVERSATION_INITIALS_MAX];
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        const struct mesh_ui_node_summary *node = &hs->nodes[i];
        const char *short_name =
            node->short_name[0] != '\0' ? node->short_name : mesh_str(MESH_STR_NODES_NO_SHORT_NAME);
        const char *long_name = node->long_name[0] != '\0' ? node->long_name : "";
        fb_format_age(node->last_heard, age, sizeof age);

        /*
         * A node the radio's NodeDB no longer carries says so in the column that would
         * otherwise hold its signal, because that is the more useful fact: the SNR is from
         * whenever we last heard it, while "off radio" is why a DM to it may never leave -
         * there is no stored key to encrypt with. The detail screen spells the same thing out.
         */
        if (!node->in_nodedb) {
            mesh_str_format(right, sizeof right, MESH_STR_NODES_ROW_OFF_RADIO, age);
        } else if (node->has_hops_away && node->hops_away > 0U) {
            mesh_str_format(right, sizeof right, MESH_STR_NODES_ROW_HOPS, (unsigned)node->hops_away,
                            age);
        } else if (node->via_mqtt) {
            mesh_str_format(right, sizeof right, MESH_STR_NODES_ROW_MQTT, age);
        } else {
            mesh_str_format(right, sizeof right, MESH_STR_NODES_ROW_SNR, (double)node->snr, age);
        }

        /*
         * The disc carries the node's initials and is tinted by node number, both answered by
         * the nav layer - so a node the user has learned to find by colour in Messages is the
         * same two cells and the same colour here.
         *
         * It resolves the *node*, not `short_name`: a node with no short name is shown as the
         * "----" placeholder, which has no letters in it and would give an empty disc, while
         * Messages falls through to the long name and then to the "!hex" id. What a row
         * displays and what identifies it are different questions.
         *
         * Ourselves is the one row that takes a stated fill instead of a tint. That is what the
         * '*' in the marker column used to say, and a disc says it without spending a cell of
         * the name: a node list is read by scanning the left edge, which is exactly where the
         * disc already is.
         */
        const bool is_me = (me != 0U && node->node_id == me);
        uint32_t tint = 0U;
        mesh_ui_nav_target_avatar(&view, node->node_id, 0U, initials, sizeof initials, &tint);

        /*
         * The star stays in the text: being pinned is a fact about the node rather than about
         * what it is, so it belongs beside the name and not in place of the identity the disc
         * is carrying.
         *
         * Never on ourselves, which is what the old marker column got right by ordering the two
         * cases. A radio can carry a stale `is_favorite` on its own NodeDB entry, and nav.c and
         * node_detail.c both refuse to pin our own node - so a star there would advertise a
         * preference that no press can clear.
         */
        mesh_ui_line_reset(&line);
        if (node->is_favorite && !is_me) {
            /* The glyph stays a literal of its own and the space is format glue, which is how
               scripts/check-strings.py already knows the star is drawn rather than read. */
            mesh_ui_line_printf(&line, "%s ", "\xE2\xAD\x90");
        }
        mesh_ui_line_column(&line, short_name, 4U);
        if (long_name[0] != '\0') {
            mesh_ui_line_printf(&line, " %s", long_name);
        }

        /* Dim behind the words, so a list that is mostly off-radio reads as one at a glance.
           The open thread's node keeps the accent whatever its NodeDB state: which node you
           are talking to is the one thing the cursor colour is for. */
        enum mesh_ui_tone tone = MESH_UI_TONE_NORMAL;
        if (node->node_id == nav->target_node) {
            tone = MESH_UI_TONE_ACCENT;
        } else if (!node->in_nodedb) {
            tone = MESH_UI_TONE_DIM;
        }

        const struct fb_list_item row = {
            .leading =
                {
                    .kind = FB_LEADING_AVATAR,
                    .label = initials,
                    .tint = tint,
                    .role = is_me ? MESH_UI_COLOR_ACCENT : MESH_UI_COLOR_COUNT,
                },
            .text = mesh_ui_line_text(&line),
            .tone = tone,
            .trailing = {.kind = FB_TRAILING_TEXT, .text = right},
            .divider = true,
        };
        fb_list_item(state, &list, i, &row);
    }
}

/* Compose overlay: it writes to the open thread, so the destination is a heading rather than
   an editable row. */
static void fb_render_compose(struct mesh_ui_backend_fb_state *state,
                              const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    char title[96];
    mesh_str_format(title, sizeof title, MESH_STR_COMPOSE_TO_KIND, nav->target_name,
                    mesh_str(nav->target_node == MESH_MESSAGE_BROADCAST_ADDR
                                 ? MESH_STR_COMPOSE_SUFFIX_CHANNEL
                                 : MESH_STR_COMPOSE_SUFFIX_DIRECT));
    fb_draw_title(state, layout, title);

    struct fb_list list =
        fb_list_begin(layout, mesh_ui_nav_compose_row_count(), nav->compose_cursor);
    struct mesh_ui_line line;
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        mesh_ui_line_reset(&line);
        /* The draft is the row that opens the keyboard and the rest are texts to send as they
           stand, which is a difference in kind rather than in indentation - so it is the
           accent tone and the accent edge that say so, and the two spaces the canned rows used
           to be pushed over by are gone. */
        const bool is_draft = (i == MESH_UI_COMPOSE_ROW_DRAFT);
        if (is_draft) {
            if (nav->draft[0] != '\0') {
                mesh_ui_line_str(&line, MESH_STR_COMPOSE_DRAFT, nav->draft);
            } else {
                mesh_ui_line_printf(&line, "%s", mesh_str(MESH_STR_COMPOSE_DRAFT_EMPTY));
            }
        } else {
            mesh_ui_line_printf(&line, "%s", mesh_ui_canned_text(i - MESH_UI_COMPOSE_FIRST_CANNED));
        }
        const struct fb_list_item row = {
            .text = mesh_ui_line_text(&line),
            .tone = is_draft ? MESH_UI_TONE_ACCENT : MESH_UI_TONE_NORMAL,
            .accent_edge = is_draft,
            .divider = true,
        };
        fb_list_item(state, &list, i, &row);
    }
}

/* "Send to" list: channels, then nodes, cursor on the current target. Mutable state, like
   every fb_list_item() caller. */
static void fb_render_picker(struct mesh_ui_backend_fb_state *state,
                             const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    struct mesh_ui_store view;
    fb_store_view(snapshot, &view);

    const uint32_t count = mesh_ui_nav_picker_count(&view);
    char title[96];
    fb_title_count(title, sizeof title, mesh_str(MESH_STR_PICKER_TITLE), count, 0U);
    fb_draw_title(state, layout, title);
    if (count == 0U) {
        fb_draw_empty(state, layout, mesh_str(MESH_STR_PICKER_EMPTY));
        return;
    }

    struct fb_list list = fb_list_begin(layout, count, nav->picker_cursor);
    char name[96];
    char initials[MESH_UI_CONVERSATION_INITIALS_MAX];
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

        /*
         * The same discs the conversation list draws, for the same rows. The nav layer answers
         * both the cells and the tint (mesh_ui_nav_target_avatar), which is what keeps a node
         * the same two letters and the same colour in both lists - this row's *name* is
         * "BRVO  Bravo Creek", and initials taken from that would read "BB".
         *
         * Between them the disc carries both of the things this row used to spell out in
         * characters: the "(channel)" suffix that the '#' already says, and the '*' marking the
         * current target, which is a stated accent fill here.
         */
        uint32_t tint = 0U;
        mesh_ui_nav_target_avatar(&view, node, channel, initials, sizeof initials, &tint);
        const struct fb_list_item row = {
            .leading =
                {
                    .kind = FB_LEADING_AVATAR,
                    .label = initials,
                    .tint = tint,
                    .role = current ? MESH_UI_COLOR_ACCENT : MESH_UI_COLOR_COUNT,
                },
            .text = name,
            .tone = is_channel ? MESH_UI_TONE_ACCENT : MESH_UI_TONE_NORMAL,
            .divider = true,
        };
        fb_list_item(state, &list, i, &row);
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
        mesh_str_format(title, sizeof title,
                        nav->pairing_confirm ? MESH_STR_PAIRING_CONFIRM : MESH_STR_PAIRING_SHOWN_ON,
                        nav->pairing_label[0] != '\0' ? nav->pairing_label
                                                      : mesh_str(MESH_STR_PAIRING_NODE_FALLBACK));
    } else if (for_setting) {
        snprintf(title, sizeof title, "%s",
                 mesh_ui_settings_field_label((enum mesh_ui_setting_field)nav->keyboard_field));
    } else {
        mesh_str_format(title, sizeof title, MESH_STR_COMPOSE_TO, nav->target_name);
    }
    fb_draw_title(state, layout, title);

    const int scale = state->scale;
    const int line = layout->line;
    int y = layout->body_y;

    /*
     * Draft box: two wrapped lines plus a cursor and a byte count.
     *
     * A container in its own right, so it is drawn as one - the raised surface tier, the panel
     * shape, and an edge in OUTLINE laid down first with the fill inside it, exactly as a card
     * is built. What it holds is the thing being typed, which is the one piece of text on this
     * screen that has to stay legible while it changes; the tier is what lifts it off the
     * keyboard below rather than leaving it as another row on the same ground.
     */
    const int box_lines = 2;
    const int margin = fb_margin(state);
    const int box_x = margin / 2;
    const int box_w = (int)state->var.xres - margin;
    const int box_h = box_lines * line + scale;
    const int edge = fb_edge(state);
    const int radius = fb_radius(state, MESH_UI_SHAPE_MD);
    fb_fill_round_rect(state, box_x, y - scale, box_w, box_h, radius + edge,
                       fb_color(state, MESH_UI_COLOR_OUTLINE));
    fb_fill_round_rect(state, box_x + edge, y - scale + edge, box_w - 2 * edge, box_h - 2 * edge,
                       radius, fb_color(state, MESH_UI_COLOR_SURFACE_HIGH));
    char draft[MESH_UI_DRAFT_MAX + 2U];
    snprintf(draft, sizeof draft, "%s_", nav->draft);
    /* Show the tail when the draft outgrows the box. */
    const size_t visible = layout->cols * (size_t)box_lines;
    const char *shown = draft;
    const size_t draft_width = mesh_ui_text_cells(draft);
    if (draft_width > visible) {
        shown = draft + mesh_ui_text_cell_offset(draft, draft_width - visible);
    }
    fb_draw_wrapped(state, y, shown, layout->cols, box_lines,
                    fb_tone_color(state, MESH_UI_TONE_STRONG));
    y += box_lines * line;

    char meter[32];
    snprintf(meter, sizeof meter, "%zu/%zu", strlen(nav->draft), draft_cap);
    fb_draw_text(state,
                 (int)state->var.xres - margin -
                     (int)mesh_ui_text_cells(meter) * fb_char_adv(state, layout->small),
                 y, meter, layout->small, fb_tone_color(state, MESH_UI_TONE_DIM));
    y += fb_line_adv(state, layout->small) + scale;

    /* The character grid and the action row are the same button, sized differently. */
    const int grid_w = (int)state->var.xres - 2 * margin;
    const int cell_w = grid_w / (int)MESH_UI_KB_COLS;
    const int cell_h = line + 2 * scale;
    for (unsigned row = 0; row < MESH_UI_KB_CHAR_ROWS; ++row) {
        for (unsigned col = 0; col < MESH_UI_KB_COLS; ++col) {
            const char ch = mesh_ui_kb_char((enum mesh_ui_kb_layer)nav->kb_layer, row, col);
            const char key[2] = {ch, '\0'};
            const struct fb_button button = {
                .rect = {.x = margin + (int)col * cell_w,
                         .y = y,
                         .w = cell_w - scale,
                         .h = cell_h - scale},
                .label = key,
                .selected = (nav->kb_row == row && nav->kb_col == col),
                .variant = FB_BUTTON_TEXT,
                .shape = MESH_UI_SHAPE_SM,
                .idle_tone = MESH_UI_TONE_NORMAL,
                .scale = scale,
            };
            fb_draw_button(state, &button);
        }
        y += cell_h;
    }

    const int action_w = grid_w / (int)MESH_UI_KB_ACTIONS;
    for (unsigned col = 0; col < MESH_UI_KB_ACTIONS; ++col) {
        const struct fb_button button = {
            .rect = {.x = margin + (int)col * action_w,
                     .y = y,
                     .w = action_w - scale,
                     .h = cell_h - scale},
            .label = mesh_ui_kb_action_label(nav, (enum mesh_ui_kb_action)col),
            .selected = (nav->kb_row == MESH_UI_KB_CHAR_ROWS && nav->kb_col == col),
            .variant = FB_BUTTON_FILLED,
            .shape = MESH_UI_SHAPE_SM,
            .idle_tone = MESH_UI_TONE_NORMAL,
            .scale = scale,
        };
        fb_draw_button(state, &button);
    }
}

/* Takes the state mutably, like every fb_list_item() caller: the item is the component that
   can carry an animated slot, so the whole entry point takes the table it would step. */
static void fb_render_devices(struct mesh_ui_backend_fb_state *state,
                              const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    char title[96];
    fb_title_count(title, sizeof title, mesh_str(MESH_STR_TAB_DEVICES),
                   (uint32_t)snapshot->device_count, 0U);
    fb_draw_title(state, layout, title);

    if (snapshot->device_count == 0U) {
        fb_draw_empty(state, layout, mesh_str(MESH_STR_DEVICES_EMPTY));
        return;
    }

    /*
     * Two body rows an item: the radio's name with how it is attached against the right edge,
     * then what it is doing under that. The list is short - a handful of radios in range - so
     * the rows are affordable here in a way they would not be on the node list, and the state
     * is the thing this screen exists to answer. It used to be one line with the state
     * concatenated onto the end of the name, where a long name pushed it off the panel.
     */
    struct fb_list list = fb_list_begin_rows(layout, (uint32_t)snapshot->device_count,
                                             nav->cursor[MESH_UI_SCREEN_DEVICES], 2U);
    char trailing[16];
    char initials[MESH_UI_CONVERSATION_INITIALS_MAX];
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        const struct mesh_ui_device *device = &snapshot->devices[i];
        const char *name = device->name[0] != '\0' ? device->name : device->identifier;
        if (name[0] == '\0') {
            name = mesh_str(MESH_STR_DEVICES_UNNAMED);
        }
        /* What pressing A on this row would do. An unpaired BLE node is the case worth
           calling out: it connects and then fails on StartNotify unless it is bonded first,
           which is exactly what A now does for it. */
        const char *status = "";
        if (device->connected) {
            status = mesh_str(MESH_STR_DEVICES_BADGE_CONNECTED);
        } else if (device->busy) {
            status = mesh_str(MESH_STR_DEVICES_BADGE_WORKING);
        } else if (device->kind == (uint8_t)MESH_UI_DEVICE_BLE && !device->paired) {
            status = mesh_str(MESH_STR_DEVICES_BADGE_NEEDS_PAIR);
        } else if (device->kind == (uint8_t)MESH_UI_DEVICE_BLE) {
            status = mesh_str(MESH_STR_DEVICES_BADGE_PAIRED);
        }

        /* A USB port has no RSSI to show, so it says which bus it is instead - the trailing
           slot answers "how is this attached" either way. */
        if (device->kind == (uint8_t)MESH_UI_DEVICE_SERIAL) {
            mesh_str_copy(trailing, sizeof trailing, mesh_str(MESH_STR_DEVICES_TRAILING_USB));
        } else {
            mesh_str_format(trailing, sizeof trailing, MESH_STR_DEVICES_TRAILING_RSSI,
                            (int)device->rssi);
        }

        const bool armed = nav->devices_forget_armed && nav->devices_forget_row == i;
        enum mesh_ui_tone tone = MESH_UI_TONE_NORMAL;
        if (device->connected) {
            tone = MESH_UI_TONE_GOOD;
        } else if (armed) {
            tone = MESH_UI_TONE_BAD;
        }

        /* The disc states its fill rather than taking a tint: a device list is four rows about
           one question - which of these am I on - and six hues would be answering a question
           nobody asked. Connected is the good tone, armed to be forgotten is the bad one. */
        mesh_ui_nav_initials(name, initials, sizeof initials);
        const struct fb_list_item row = {
            .leading =
                {
                    .kind = FB_LEADING_AVATAR,
                    .label = initials,
                    .tint = i,
                    .role = device->connected ? MESH_UI_COLOR_GOOD
                            : armed           ? MESH_UI_COLOR_BAD
                                              : MESH_UI_COLOR_COUNT,
                },
            .text = name,
            .tone = tone,
            .trailing = {.kind = FB_TRAILING_TEXT, .text = trailing},
            .supporting = status,
            .supporting_tone = MESH_UI_TONE_DIM,
            /* The state stays quiet under the cursor - it is a fact about the row, not the
               row's own words - except when it is the warning, which has to stay loud. */
            .supporting_quiet = !armed,
            .divider = true,
        };
        fb_list_item(state, &list, i, &row);
    }
}

/* "3d 4h", "5h 12m", "40m" - a radio's uptime, which is a duration rather than an age. */
static void fb_format_uptime(uint32_t seconds, char *out, size_t out_len) {
    if (seconds >= 86400U) {
        mesh_str_format(out, out_len, MESH_STR_TIME_DAYS_HOURS, seconds / 86400U,
                        (seconds % 86400U) / 3600U);
    } else if (seconds >= 3600U) {
        mesh_str_format(out, out_len, MESH_STR_TIME_HOURS_MINUTES, seconds / 3600U,
                        (seconds % 3600U) / 60U);
    } else {
        mesh_str_format(out, out_len, MESH_STR_TIME_MINUTES_SHORT, seconds / 60U);
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

/*
 * The Status tab, as three cards.
 *
 * It used to be eighteen label/value lines on the bare ground, in one column, and nothing in it
 * said that Transport, Radio and Sync are one subject and Packets and Dropped are another - the
 * only grouping was a half-line of extra space every so often, which is not a grouping so much
 * as a hope. Each card names its subject and reports on it in its own heading colour, so "is
 * anything wrong" is answered by the shape and the colour before a number has been read.
 *
 * There is no screen title: the tab strip already says Status and every card names itself, so a
 * title would be the third time. The rows it frees are the ones the cards spend on their
 * headings.
 *
 * Cards are declared and then drawn (see fb_widgets.h), so a row that only exists when the
 * radio has reported something is an `if` around one call. Nothing here guards the footer
 * either: fb_draw_card() drops what does not fit and refuses a card outright when nothing does,
 * which is the check this screen used to write out per row, and in two different ways.
 */
static void fb_render_status(const struct mesh_ui_backend_fb_state *state,
                             const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    int y = layout->body_y;
    struct fb_card card;
    char buffer[64];
    char second[64];

    /* ---- the link: what we are talking to, and whether it has told us who it is ---- */

    const struct mesh_ui_device *connected = fb_connected_device(snapshot);

    fb_card_begin(&card, MESH_STR_STATUS_CARD_LINK,
                  connected != NULL ? MESH_UI_TONE_GOOD : MESH_UI_TONE_BAD);
    fb_card_row_text(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_TRANSPORT,
                     snapshot->transport_status[0] != '\0'
                         ? snapshot->transport_status
                         : mesh_str(MESH_STR_HEADER_TRANSPORT_STARTING));
    fb_card_row_text(&card, connected != NULL ? MESH_UI_TONE_GOOD : MESH_UI_TONE_BAD,
                     MESH_STR_STATUS_LABEL_RADIO,
                     connected != NULL ? fb_device_label(connected)
                                       : mesh_str(MESH_STR_STATUS_NOT_CONNECTED));
    if (snapshot->handshake_valid) {
        const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
        fb_card_row(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_SYNC,
                    MESH_STR_STATUS_SYNC_VALUE,
                    mesh_str(hs->config_complete     ? MESH_STR_STATUS_SYNC_COMPLETE
                             : hs->request_in_flight ? MESH_STR_STATUS_SYNC_IN_PROGRESS
                                                     : MESH_STR_STATUS_SYNC_IDLE),
                    hs->cached ? mesh_str(MESH_STR_STATUS_SYNC_CACHED) : "");
        if (hs->has_my_info) {
            fb_card_row(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_MY_NODE,
                        MESH_STR_STATUS_MY_NODE, hs->my_short_name, hs->my_info.node_num);
        }
        if (hs->primary_channel[0] != '\0') {
            fb_card_row_text(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_CHANNEL,
                             hs->primary_channel);
        }
    } else {
        fb_card_row_text(&card, MESH_UI_TONE_DIM, MESH_STR_STATUS_LABEL_SYNC,
                         mesh_str(MESH_STR_STATUS_SYNC_WAITING));
    }
    /* What else is within reach, which is the same subject as what we are attached to - and on
       a screen where the radio is gone it is the row that says whether anything is there. */
    fb_card_row(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_DEVICES,
                MESH_STR_STATUS_DEVICES_IN_RANGE, snapshot->device_count);
    (void)fb_draw_card(state, layout, &y, &card);

    /*
     * ---- the mesh: how many nodes, and how much of the air they are using ----
     *
     * Mesh health comes from the two sources that carry it. LocalStats is the radio's own live
     * view, sent to the attached client on its own schedule; DeviceMetrics is what our node
     * last *broadcast* about itself, on the telemetry interval, which is half an hour by
     * default. Both carry the airtime pair, so LocalStats wins it when it has arrived and
     * DeviceMetrics only fills the gap before the first report - reading the broadcast copy by
     * preference means the row can sit on a half-hour-old 0.0% while the radio is busy.
     */
    const struct mesh_ui_node_summary *self = fb_self_node(snapshot);
    const struct mesh_ui_node_metrics *metrics =
        (self != NULL && self->metrics.valid) ? &self->metrics : NULL;
    const struct mesh_ui_radio_stats *stats = &snapshot->settings.stats;

    /* LocalStats' airtime fields are plain scalars the firmware always fills, so `valid` is the
       whole test; DeviceMetrics' are optional and carry their own has_*. */
    const bool air_from_stats = stats->valid;
    const bool have_util = air_from_stats || (metrics != NULL && metrics->has_channel_utilization);
    const bool have_tx = air_from_stats || (metrics != NULL && metrics->has_air_util_tx);
    const float util_value = air_from_stats
                                 ? stats->channel_utilization
                                 : (metrics != NULL ? metrics->channel_utilization : 0.0f);
    /* Above ~25% channel utilization the mesh is saturated and hop delivery collapses, so the
       number is coloured rather than left as one more figure to interpret - and the card's
       heading takes the same tone, which is what makes a saturated mesh visible from the shape
       of the screen rather than from reading a percentage. */
    enum mesh_ui_tone air_tone = MESH_UI_TONE_NORMAL;
    if (have_util) {
        air_tone = util_value >= 50.0f   ? MESH_UI_TONE_BAD
                   : util_value >= 25.0f ? MESH_UI_TONE_ACCENT
                                         : MESH_UI_TONE_GOOD;
    }

    fb_card_begin(&card, MESH_STR_STATUS_CARD_MESH,
                  air_tone != MESH_UI_TONE_NORMAL ? air_tone : MESH_UI_TONE_ACCENT);
    if (snapshot->handshake_valid) {
        const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
        /* One line for the NodeDB, and LocalStats' online count when the radio has sent it:
           "132 nodes" alone says nothing about how much of that mesh is still alive. */
        if (hs->has_my_info && stats->valid && stats->num_online_nodes > 0U) {
            fb_card_row(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_NODEDB,
                        MESH_STR_STATUS_NODEDB_ONLINE, hs->my_info.nodedb_entries,
                        stats->num_online_nodes);
        } else if (hs->has_my_info) {
            fb_card_row(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_NODEDB,
                        MESH_STR_STATUS_NODEDB_REBOOTS, hs->my_info.nodedb_entries,
                        hs->my_info.reboot_count);
        }
        /* Ours, next to the radio's, and only while the two differ. The row above counts the
           radio's database; this one counts the roster, which outlives it on purpose - so after
           a NodeDB reset one says 2 and the other 81 with nothing to explain it. Both numbers
           here are the published rows, so the second can never exceed the first. */
        const uint32_t off_radio = mesh_ui_handshake_off_radio(hs);
        if (off_radio > 0U) {
            fb_card_row(&card, MESH_UI_TONE_DIM, MESH_STR_STATUS_LABEL_CACHED_HERE,
                        MESH_STR_STATUS_CACHED_OFF_RADIO, hs->node_count, off_radio);
        }
    }

    if (have_util || have_tx) {
        const float tx_value =
            air_from_stats ? stats->air_util_tx : (metrics != NULL ? metrics->air_util_tx : 0.0f);
        char util[32];
        mesh_str_copy(util, sizeof util, mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT));
        if (have_util) {
            mesh_str_format(util, sizeof util, MESH_STR_STATUS_PERCENT, (double)util_value);
        }
        char tx[32];
        mesh_str_copy(tx, sizeof tx, mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT));
        if (have_tx) {
            mesh_str_format(tx, sizeof tx, MESH_STR_STATUS_PERCENT, (double)tx_value);
        }
        if (stats->valid && stats->has_noise_floor) {
            fb_card_row(&card, air_tone, MESH_STR_STATUS_LABEL_AIRTIME,
                        MESH_STR_STATUS_AIRTIME_FLOOR, util, tx, stats->noise_floor);
        } else {
            fb_card_row(&card, air_tone, MESH_STR_STATUS_LABEL_AIRTIME, MESH_STR_STATUS_AIRTIME,
                        util, tx);
        }
    }

    if (stats->valid) {
        fb_card_row(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_PACKETS,
                    MESH_STR_STATUS_PACKETS, stats->num_packets_tx, stats->num_packets_rx,
                    stats->num_tx_relay);
        /* Bad and dropped packets are the two numbers that explain a mesh that "works but loses
           messages", so they get their own row instead of being folded into Packets. */
        const bool losing = stats->num_packets_rx_bad > 0U || stats->num_tx_dropped > 0U;
        fb_card_row(&card, losing ? MESH_UI_TONE_ACCENT : MESH_UI_TONE_DIM,
                    MESH_STR_STATUS_LABEL_DROPPED, MESH_STR_STATUS_DROPPED,
                    stats->num_packets_rx_bad, stats->num_rx_dupe, stats->num_tx_dropped);
    } else if (snapshot->handshake_valid) {
        /* Standing in for the two rows above, so it carries their label rather than one naming
           the card it is already inside - "Mesh: no report yet" on a card headed Mesh says the
           word twice and the subject once. Short enough for the value gutter, too: the long
           form was cut mid-word, which reads as a bug rather than as a radio that has simply
           not reported yet. */
        fb_card_row_text(&card, MESH_UI_TONE_DIM, MESH_STR_STATUS_LABEL_PACKETS,
                         mesh_str(MESH_STR_STATUS_MESH_NO_REPORT));
    }
    /* How much of that traffic this client is still holding. It is the one row on the card that
       counts something of ours rather than the radio's, and it sits here because what the ring
       holds is mesh traffic - a card of its own for two client-side numbers is a heading and two
       insets spent on the least-read rows of the screen. */
    fb_card_row(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_MESSAGES,
                MESH_STR_STATUS_MESSAGES_KEPT, (unsigned)snapshot->messages.count,
                (unsigned)snapshot->messages.dropped);
    (void)fb_draw_card(state, layout, &y, &card);

    /* ---- the radio itself: its battery, its queue, and what it last said about itself ---- */

    /* Uptime is in both sources, like the airtime pair above, so LocalStats wins it for the same
       reason - and without this the row vanishes entirely when LocalStats has arrived but our
       node has not broadcast DeviceMetrics yet. Battery really does have only the one source. */
    const bool have_battery = metrics != NULL && metrics->has_battery;
    const bool have_uptime = stats->valid || (metrics != NULL && metrics->has_uptime);
    const uint32_t uptime_value = stats->valid        ? stats->uptime_seconds
                                  : (metrics != NULL) ? metrics->uptime_seconds
                                                      : 0U;
    const bool low_battery = have_battery && metrics->battery_level <= 20U;

    /*
     * The last thing the radio said in its own words, and how many times it has restarted under
     * us. Both are the answers to "why is this not working" that no counter above can give: the
     * counters describe traffic, and a duty-cycle refusal or a key mismatch is not traffic.
     *
     * Levels are python logging's scale: 40 is ERROR, 30 WARNING. Anything below that is the
     * radio being informative rather than reporting a problem.
     */
    const struct mesh_ui_radio_notice *notice = &snapshot->settings.notice;
    const bool have_notice = notice->seq != 0U && notice->text[0] != '\0';
    const enum mesh_ui_tone notice_tone = notice->level >= 40U   ? MESH_UI_TONE_BAD
                                          : notice->level >= 30U ? MESH_UI_TONE_ACCENT
                                                                 : MESH_UI_TONE_NORMAL;
    const struct mesh_ui_queue_status *queue = &snapshot->settings.queue;
    /* The radio's send queue is only worth a row once it is under pressure or has just refused
       something: on an idle link it reads "16 of 16 free" for ever, which is one more number to
       skip past. A refusal keeps the row up because it is the explanation for a message that
       was never transmitted at all. */
    const bool have_queue =
        queue->valid && queue->maxlen > 0U && (queue->res != 0 || queue->free < queue->maxlen / 2U);

    /* The card reports the worst thing it holds. A refused packet and an ERROR notice are both
       the radio saying no; a flat battery is the reason it is about to. */
    enum mesh_ui_tone radio_tone = MESH_UI_TONE_ACCENT;
    if (low_battery || (have_queue && queue->res != 0) || (have_notice && notice->level >= 40U)) {
        radio_tone = MESH_UI_TONE_BAD;
    } else if (have_notice && notice->level >= 30U) {
        radio_tone = MESH_UI_TONE_ACCENT;
    }

    /*
     * Ordered most-read first, which matters here and on no other card: this is the one that can
     * outgrow the panel - every row on it appears only when the radio is in some kind of
     * trouble, so the worst case is all of them at once - and fb_draw_card() drops from the end.
     * So the battery and the radio's own words come first and the heap figure last, because a
     * free-heap number is the row a user would have scrolled past anyway.
     */
    fb_card_begin(&card, MESH_STR_STATUS_CARD_RADIO, radio_tone);
    if (have_battery || have_uptime) {
        buffer[0] = '\0';
        if (have_battery) {
            /* 101 is upstream's "running off USB", not a 101% battery. */
            if (metrics->battery_level > 100U) {
                mesh_str_copy(buffer, sizeof buffer, mesh_str(MESH_STR_STATUS_BATTERY_USB));
            } else {
                mesh_str_format(buffer, sizeof buffer, MESH_STR_STATUS_BATTERY_PERCENT,
                                (unsigned)metrics->battery_level);
            }
        }
        second[0] = '\0';
        if (have_uptime) {
            char uptime[32];
            fb_format_uptime(uptime_value, uptime, sizeof uptime);
            mesh_str_format(second, sizeof second, MESH_STR_STATUS_UPTIME_SUFFIX,
                            buffer[0] != '\0' ? ", " : "", uptime);
        }
        fb_card_row(&card, low_battery ? MESH_UI_TONE_BAD : MESH_UI_TONE_NORMAL,
                    MESH_STR_STATUS_LABEL_BATTERY, MESH_STR_STATUS_SYNC_VALUE,
                    buffer[0] != '\0' ? buffer : mesh_str(MESH_STR_STATUS_BATTERY_UNKNOWN), second);
    }
    if (have_notice) {
        /*
         * When and how often on the labelled row, the words themselves as a note underneath at
         * the card's full width. Every other row here is a label and a short value, but a
         * notification is a sentence the firmware wrote, and a sentence in the narrow value
         * gutter is three words and a cut - which loses exactly the part that explains anything.
         */
        char age[24];
        fb_format_age(notice->received, age, sizeof age);
        if (notice->seq > 1U) {
            fb_card_row(&card, MESH_UI_TONE_DIM, MESH_STR_STATUS_LABEL_RADIO_SAID,
                        MESH_STR_STATUS_RADIO_SAID_COUNT, age, notice->seq);
        } else {
            fb_card_row_text(&card, MESH_UI_TONE_DIM, MESH_STR_STATUS_LABEL_RADIO_SAID, age);
        }
        fb_card_note(&card, notice_tone, notice->text);
    }
    if (have_queue) {
        fb_card_row(&card, queue->res != 0 ? MESH_UI_TONE_BAD : MESH_UI_TONE_ACCENT,
                    MESH_STR_STATUS_LABEL_TX_QUEUE, MESH_STR_STATUS_TX_QUEUE, (unsigned)queue->free,
                    (unsigned)queue->maxlen,
                    queue->res != 0 ? mesh_str(MESH_STR_STATUS_TX_QUEUE_REFUSED) : "");
    }
    if (snapshot->settings.reboot_notices > 0U) {
        fb_card_row(&card, MESH_UI_TONE_ACCENT, MESH_STR_STATUS_LABEL_REBOOTS,
                    MESH_STR_STATUS_REBOOTS_SINCE, snapshot->settings.reboot_notices);
    }
    if (stats->valid && stats->has_heap) {
        fb_card_row(&card, stats->heap_free_bytes < 20480U ? MESH_UI_TONE_ACCENT : MESH_UI_TONE_DIM,
                    MESH_STR_STATUS_LABEL_HEAP, MESH_STR_STATUS_HEAP,
                    stats->heap_free_bytes / 1024U, stats->heap_total_bytes / 1024U);
    }
    (void)fb_draw_card(state, layout, &y, &card);
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
    fb_draw_wrapped(state, layout->body_y, text, layout->cols, text_lines,
                    fb_tone_color(state, MESH_UI_TONE_NORMAL));

    /* The two choices are a list of their own, below the wrapped body text. */
    struct fb_layout choices = *layout;
    choices.body_y = layout->body_y + text_lines * layout->line + layout->line / 2;
    struct fb_list list = fb_list_begin(&choices, 2U, nav->confirm_cursor);
    const char *const rows[] = {mesh_ui_settings_confirm_accept(confirmed),
                                mesh_str(MESH_STR_COMMON_CANCEL)};
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        fb_list_row(state, &list, i, rows[i], i == 0U ? MESH_UI_TONE_ACCENT : MESH_UI_TONE_NORMAL);
    }
}

/* Settings: the section list, or one section's label/value rows. Editable rows show a
   pending edit in place of the radio's value with a marker until Y saves it. */
/* Takes the state mutably, unlike its neighbours: the switches on the toggle rows step an
   animation kept on it. Nothing else here writes to the state. */
static void fb_render_settings(struct mesh_ui_backend_fb_state *state,
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
    const char *const trail = nav->settings_parent == MESH_UI_SETTINGS_MODULES
                                  ? mesh_str(MESH_STR_SETTINGS_TRAIL_MODULES)
                                  : "";
    char title[96];
    if (section_open && nav->settings_channel != MESH_UI_SETTINGS_NO_CHANNEL) {
        mesh_str_format(title, sizeof title, MESH_STR_SETTINGS_TITLE_CHANNEL,
                        (unsigned)nav->settings_channel,
                        nav->settings_edit_count > 0U ? mesh_str(MESH_STR_SETTINGS_UNSAVED) : "");
    } else if (section_open) {
        mesh_str_format(title, sizeof title, MESH_STR_SETTINGS_TITLE_SECTION, trail,
                        mesh_ui_settings_section_name(section),
                        nav->settings_edit_count > 0U ? mesh_str(MESH_STR_SETTINGS_UNSAVED) : "");
    } else {
        snprintf(title, sizeof title, "%s", mesh_str(MESH_STR_SETTINGS_TITLE));
    }
    fb_draw_title(state, layout, title);

    /* Every other section describes the radio, but About describes this client, so the tab
       stays usable with nothing connected: the section list still draws (About is the only
       row not greyed out) and opening About still works. Modules is let through for the
       reason the section list itself is - it is a list of what exists, not a read of the
       radio, and each of its rows says "not loaded" on its own. */
    if (!settings->loaded && (handshake == NULL || !handshake->has_my_info) && section_open &&
        section != MESH_UI_SETTINGS_ABOUT && section != MESH_UI_SETTINGS_MODULES) {
        fb_draw_empty(state, layout, mesh_str(MESH_STR_SETTINGS_EMPTY_DISCONNECT));
        return;
    }

    const uint32_t count = section_open ? mesh_ui_settings_item_count(settings, handshake, section,
                                                                      nav->settings_channel)
                                        : mesh_ui_settings_root_count();
    if (count == 0U) {
        fb_draw_empty(state, layout, mesh_str(MESH_STR_SETTINGS_EMPTY_SECTION));
        return;
    }

    /* Label column: a fixed width so values line up, capped for narrow scales. */
    const size_t label_cols = fb_field_label_cols(state, layout, 0U);
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
                fb_list_row_line(state, &list, i, &line, MESH_UI_TONE_DIM);
                continue;
            }
            /* Editable rows carry a marker so the eye can tell what Left/Right will act on;
               channel rows open with A. */
            const char *marker = item.dirty                            ? "* "
                                 : item.field != MESH_UI_FIELD_NONE    ? "> "
                                 : item.kind == MESH_UI_SETTING_ACTION ? "> "
                                                                       : "  ";
            const enum mesh_ui_tone tone = item.dirty ? MESH_UI_TONE_STRONG : MESH_UI_TONE_NORMAL;
            /*
             * A boolean gets a switch rather than the words. The words are still what the CLI
             * backend draws and still what item.value holds - this is the fb backend deciding
             * how to say the same thing on a screen, which is exactly the choice a backend is
             * for.
             *
             * The field id is the switch's identity, and it has to be one no other row in the
             * frame shares: the channel rows repeat the same fields per channel, so the
             * channel is mixed in. A read-only toggle has no field at all and is keyed on its
             * row instead, above everything the field enum can reach.
             */
            if (item.kind == MESH_UI_SETTING_TOGGLE) {
                struct fb_switch sw = {
                    .id = item.field != MESH_UI_FIELD_NONE
                              ? 0x01000000U | ((uint32_t)nav->settings_channel << 16) |
                                    (uint32_t)item.field
                              : 0x02000000U | i,
                    .on = item.number != 0U,
                    .dim = item.field == MESH_UI_FIELD_NONE,
                };
                const struct fb_list_item row = {
                    .label = item.label,
                    .label_cols = label_cols,
                    .marker = marker,
                    .tone = tone,
                    .trailing = {.kind = FB_TRAILING_SWITCH, .sw = &sw},
                };
                fb_list_item(state, &list, i, &row);
                continue;
            }
            const struct fb_list_item row = {
                .label = item.label,
                .label_cols = label_cols,
                .marker = marker,
                .value = item.value,
                .tone = tone,
            };
            fb_list_item(state, &list, i, &row);
        } else {
            const enum mesh_ui_settings_section section_row = mesh_ui_settings_root_at(i);
            const bool loaded = mesh_ui_settings_section_loaded(settings, handshake, section_row);
            const struct fb_list_item row = {
                .label = mesh_ui_settings_section_name(section_row),
                .label_cols = label_cols,
                .marker = "",
                .value = loaded ? "" : mesh_str(MESH_STR_SETTINGS_NOT_LOADED),
                .tone = loaded ? MESH_UI_TONE_NORMAL : MESH_UI_TONE_DIM,
            };
            fb_list_item(state, &list, i, &row);
        }
    }
}

void fb_render_snapshot(struct mesh_ui_backend_fb_state *state,
                        const struct mesh_ui_snapshot *snapshot) {
    /* Before anything is measured: a theme carries the glyph scale and the margin the whole
       frame is laid out against, so adopting one mid-frame would draw half of each. */
    (void)fb_state_follow_snapshot(state, snapshot);

    fb_clear(state, fb_color(state, MESH_UI_COLOR_BG));

    struct fb_layout layout;
    memset(&layout, 0, sizeof layout);
    layout.small = mesh_ui_theme_chrome_scale(state->theme, state->scale);
    layout.line = fb_line_adv(state, state->scale);
    layout.cols = fb_cols(state, state->scale);

    fb_draw_tabs(state, snapshot, &layout);

    const int margin = fb_margin(state);
    const int footer_height = 2 * fb_line_adv(state, layout.small) + margin;
    layout.footer_y = (int)state->var.yres - footer_height;
    const int body_height = layout.footer_y - layout.body_y - margin / 2;
    layout.rows = body_height > 0 ? (uint32_t)(body_height / layout.line) : 0U;

    const char *hint = mesh_str(MESH_STR_HINT_DEFAULT);
    if (snapshot->nav.confirm_open) {
        hint = mesh_str(MESH_STR_HINT_CONFIRM);
        fb_render_confirm(state, snapshot, &layout);
        fb_draw_footer(state, snapshot, &layout, hint);
        return;
    }
    if (snapshot->nav.picker_open) {
        hint = mesh_str(MESH_STR_HINT_ENUM_PICKER);
        fb_render_picker(state, snapshot, &layout);
        fb_draw_footer(state, snapshot, &layout, hint);
        return;
    }
    if (snapshot->nav.keyboard_open) {
        if (snapshot->nav.keyboard_passkey) {
            hint = snapshot->nav.pairing_confirm ? mesh_str(MESH_STR_HINT_PAIRING_CONFIRM)
                                                 : mesh_str(MESH_STR_HINT_PAIRING_ENTRY);
        } else {
            hint = snapshot->nav.keyboard_field != MESH_UI_FIELD_NONE
                       ? mesh_str(MESH_STR_HINT_KEYBOARD_FIELD)
                       : mesh_str(MESH_STR_HINT_KEYBOARD_MESSAGE);
        }
        fb_render_keyboard(state, snapshot, &layout);
        fb_draw_footer(state, snapshot, &layout, hint);
        return;
    }
    if (snapshot->nav.compose_open) {
        hint = mesh_str(MESH_STR_HINT_CANNED);
        fb_render_compose(state, snapshot, &layout);
        fb_draw_footer(state, snapshot, &layout, hint);
        return;
    }
    switch (snapshot->nav.screen) {
    case MESH_UI_SCREEN_MESSAGES:
        if (!snapshot->nav.thread_open) {
            hint = snapshot->nav.messages_delete_armed ? mesh_str(MESH_STR_HINT_CONVERSATION_DELETE)
                                                       : mesh_str(MESH_STR_HINT_CONVERSATIONS);
            fb_render_conversations(state, snapshot, &layout);
        } else {
            hint = snapshot->nav.inbox ? mesh_str(MESH_STR_HINT_INBOX)
                                       : mesh_str(MESH_STR_HINT_THREAD);
            fb_render_thread(state, snapshot, &layout);
        }
        break;
    case MESH_UI_SCREEN_NODES:
        hint = snapshot->nav.node_remove_armed  ? mesh_str(MESH_STR_HINT_NODE_REMOVE)
               : snapshot->nav.node_detail_open ? mesh_str(MESH_STR_HINT_NODE_DETAIL)
                                                : mesh_str(MESH_STR_HINT_NODES);
        fb_render_nodes(state, snapshot, &layout);
        break;
    case MESH_UI_SCREEN_DEVICES:
        hint = snapshot->nav.devices_forget_armed ? mesh_str(MESH_STR_HINT_DEVICES_FORGET)
                                                  : mesh_str(MESH_STR_HINT_DEVICES);
        fb_render_devices(state, snapshot, &layout);
        break;
    case MESH_UI_SCREEN_SETTINGS:
        if (snapshot->nav.settings_section == MESH_UI_SETTINGS_NO_SECTION) {
            hint = mesh_str(MESH_STR_HINT_SETTINGS_ROOT);
        } else if (snapshot->nav.settings_discard_armed) {
            hint = mesh_str(MESH_STR_HINT_SETTINGS_DISCARD);
        } else if (snapshot->nav.settings_edit_count > 0U) {
            hint = mesh_str(MESH_STR_HINT_SETTINGS_EDIT_SAVE);
        } else if (snapshot->nav.settings_section == MESH_UI_SETTINGS_CHANNELS &&
                   snapshot->nav.settings_channel == MESH_UI_SETTINGS_NO_CHANNEL) {
            hint = mesh_str(MESH_STR_HINT_SETTINGS_CHANNELS);
        } else if (snapshot->nav.settings_section == MESH_UI_SETTINGS_MODULES) {
            /* A list, not a section: nothing on it is editable, so the edit keys would be
               advertising a press that does nothing. The same branch the channel list has. */
            hint = mesh_str(MESH_STR_HINT_SETTINGS_MODULES);
        } else if (snapshot->nav.settings_section == MESH_UI_SETTINGS_ABOUT) {
            /* Nothing here is editable and nothing here comes from the radio, so neither the
               edit keys nor X mean anything. */
            hint = mesh_str(MESH_STR_HINT_SETTINGS_ACTIONS);
        } else {
            hint = mesh_str(MESH_STR_HINT_SETTINGS_SECTION);
        }
        fb_render_settings(state, snapshot, &layout);
        break;
    case MESH_UI_SCREEN_STATUS:
    default:
        /* Status has no controls of its own, so the footer says the one thing the Brick's chrome
           cannot: how to get out. It used to be a body row on this screen, which is the only
           screen that ever put a hint in the body - the footer is where every other screen says
           what the buttons do, and the body row it frees is one the cards spend. Only while a
           radio is attached, though: the line under this one already ends in the quit hint when
           there is none, and the same sentence twice reads as a rendering fault. */
        hint = fb_connected_device(snapshot) != NULL ? mesh_ui_input_quit_hint()
                                                     : mesh_str(MESH_STR_HINT_TABS_ONLY);
        fb_render_status(state, snapshot, &layout);
        break;
    }

    fb_draw_footer(state, snapshot, &layout, hint);
}
