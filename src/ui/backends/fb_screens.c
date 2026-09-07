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

/* What to call it: the advertised name when it has one, otherwise whatever we addressed it by. */
static const char *fb_device_label(const struct mesh_ui_device *device) {
    return device->name[0] != '\0' ? device->name : device->identifier;
}

/*
 * The icon a screen is known by: on its tab, and again on the empty state that stands in for
 * its list. One answer in one place, because a tab and its empty screen showing two different
 * symbols for the same thing is exactly the drift a table like this prevents.
 */
static enum mesh_ui_icon fb_screen_icon(enum mesh_ui_screen screen) {
    switch (screen) {
    case MESH_UI_SCREEN_MESSAGES:
        return MESH_UI_ICON_MESSAGES;
    case MESH_UI_SCREEN_NODES:
        return MESH_UI_ICON_NODES;
    case MESH_UI_SCREEN_DEVICES:
        return MESH_UI_ICON_DEVICES;
    case MESH_UI_SCREEN_STATUS:
        return MESH_UI_ICON_STATUS;
    case MESH_UI_SCREEN_SETTINGS:
        return MESH_UI_ICON_SETTINGS;
    default:
        return MESH_UI_ICON_NONE;
    }
}

/* ---- chrome ------------------------------------------------------------------------------ */

/*
 * The tab strip and the two lines under the body were both written out here, and both were the
 * last screen-level renderers laying out their own pixels. They are components now
 * (fb_draw_nav_bar, fb_draw_action_bar), so what is left in this file is the *content*: which
 * tabs there are, which one is up, and what the bottom line has to say about the radio.
 */

/* One chip per screen, in tab order. Static because the set never changes and the strip only
   reads it; what moves is which index is active. */
static const struct fb_chip *fb_tab_chips(void) {
    static struct fb_chip chips[MESH_UI_SCREEN_COUNT];
    for (int i = 0; i < MESH_UI_SCREEN_COUNT; ++i) {
        const enum mesh_ui_screen screen = (enum mesh_ui_screen)i;
        chips[i].icon = fb_screen_icon(screen);
        chips[i].label = mesh_ui_screen_name(screen);
    }
    return chips;
}

/*
 * The line under the keycaps: what the transport is doing, and either the radio it found or
 * how to leave.
 *
 * `tone` is the second half of the same sentence - a link that is up is worth saying in the
 * success colour, and one that is not is not worth shouting about - so the two are decided
 * together here rather than by the widget, which has no idea what the words mean.
 */
static void fb_link_summary(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_line *line,
                            enum mesh_ui_tone *tone) {
    mesh_ui_line_reset(line);
    const char *status = snapshot->transport_status[0] != '\0'
                             ? snapshot->transport_status
                             : mesh_str(MESH_STR_HEADER_TRANSPORT_STARTING);
    const struct mesh_ui_device *device = mesh_ui_snapshot_connected_device(snapshot);
    if (device != NULL) {
        mesh_ui_line_str(line, MESH_STR_HEADER_STATUS_CONNECTED, status, fb_device_label(device));
        *tone = MESH_UI_TONE_SUCCESS;
        return;
    }
    mesh_ui_line_str(line, MESH_STR_HEADER_STATUS_QUIT, status, mesh_ui_input_quit_hint());
    *tone = MESH_UI_TONE_DIM;
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
        fb_draw_empty(state, layout, MESH_UI_ICON_MESSAGES, mesh_str(MESH_STR_MESSAGES_EMPTY));
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
            /*
             * The three rows that are not a person say so with a symbol instead of initials.
             * Which symbol is this backend's business, not the store's: the store publishes
             * what a row *is* (its kind) and the CLI backend still draws the "#" and "+" it
             * always did.
             */
            .avatar_icon =
                is_new                                                ? MESH_UI_ICON_COMPOSE
                : (conversation.kind == MESH_UI_CONVERSATION_ALL)     ? MESH_UI_ICON_BROADCAST
                : (conversation.kind == MESH_UI_CONVERSATION_CHANNEL) ? MESH_UI_ICON_CHANNEL
                                                                      : MESH_UI_ICON_NONE,
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
               used to be too, and no longer needs to be now that its avatar carries the tag.
               That frees the strong tone to mean what it means everywhere else on this
               screen: there is something here you have not read. */
            .name_tone = is_new                                            ? MESH_UI_TONE_DIM
                         : (conversation.kind == MESH_UI_CONVERSATION_ALL) ? MESH_UI_TONE_PRIMARY
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
    static const enum mesh_str_id kMonths[] = {
        MESH_STR_DATE_JAN, MESH_STR_DATE_FEB, MESH_STR_DATE_MAR, MESH_STR_DATE_APR,
        MESH_STR_DATE_MAY, MESH_STR_DATE_JUN, MESH_STR_DATE_JUL, MESH_STR_DATE_AUG,
        MESH_STR_DATE_SEP, MESH_STR_DATE_OCT, MESH_STR_DATE_NOV, MESH_STR_DATE_DEC,
    };
    static const enum mesh_str_id kWeekdays[] = {
        MESH_STR_DATE_SUN, MESH_STR_DATE_MON, MESH_STR_DATE_TUE, MESH_STR_DATE_WED,
        MESH_STR_DATE_THU, MESH_STR_DATE_FRI, MESH_STR_DATE_SAT,
    };
    const char *month = mesh_str(kMonths[when.tm_mon]);
    const char *weekday = mesh_str(kWeekdays[when.tm_wday]);
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
        fb_draw_empty(state, layout, MESH_UI_ICON_MESSAGES,
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
 * scroll and clip identically. Headings are dimmed and get no value column; the action row ends
 * in the chevron a settings row that opens something ends in, for the same reason - it is the
 * only thing on the screen A does anything to.
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
        fb_draw_empty(state, layout, MESH_UI_ICON_NODES, mesh_str(MESH_STR_NODES_GONE));
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
        fb_draw_empty(state, layout, MESH_UI_ICON_NODES, mesh_str(MESH_STR_NODES_DETAIL_EMPTY));
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
            const struct fb_list_item row = {
                .text = item->label,
                .tone = MESH_UI_TONE_PRIMARY,
                .trailing = {.kind = FB_TRAILING_ICON, .icon = MESH_UI_ICON_CHEVRON},
            };
            fb_list_item(state, &list, i, &row);
        } else if (item->kind == MESH_UI_NODE_ROW_METER) {
            /*
             * The figure and, beside it, where that figure sits between its own two ends - which
             * is the half of a reading that decibels and percentages do not carry. The row still
             * says the number; the bar is what says whether the number is a problem.
             *
             * Keyed on the row index above everything the settings rows can reach, for the
             * reason a meter row there is: a reading is a fact rather than a control, so it has
             * no field of its own to be identified by.
             */
            struct fb_meter meter = {
                .id = 0x04000000U | i,
                .kind = FB_METER_DETERMINATE,
                .value = item->number,
                .scale = item->scale,
                .band = item->banded ? &item->band : NULL,
                .tone = MESH_UI_TONE_SUCCESS,
            };
            const struct fb_list_item row = {
                .label = item->label,
                .label_cols = label_cols,
                .value = item->value,
                .tone = MESH_UI_TONE_NORMAL,
                .trailing = {.kind = FB_TRAILING_METER, .meter = &meter},
            };
            fb_list_item(state, &list, i, &row);
        } else {
            const struct fb_list_item row = {
                .label = item->label,
                .label_cols = label_cols,
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
        fb_draw_empty(state, layout, MESH_UI_ICON_NODES,
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
         *
         * The branches that say something instead of a signal are the ones where there is no
         * signal *to this node* to say. An SNR is measured on the packet that arrived, so for a
         * node reached over several hops it describes the last relay and for one arriving over
         * MQTT it describes nothing on the air at all - a staircase there would be reporting
         * somebody else's link as this node's.
         *
         * The last branch is the one that matters and it is not the same test as the others:
         * `hops_away` unset means the firmware did not say, which is not the same as zero, and
         * an SNR of 0.0 is the session layer's own "no reading". Either would give a node
         * nothing was ever heard from three of four rungs. mesh_ui_node_signal_heard() is the
         * whole of that question, and everything it declines falls through to the figure this
         * column drew before - which is the right way round, because printing a number that
         * describes something else is unhelpful where drawing it is a claim.
         */
        bool direct = false;
        if (!node->in_nodedb) {
            mesh_str_format(right, sizeof right, MESH_STR_NODES_ROW_OFF_RADIO, age);
        } else if (node->has_hops_away && node->hops_away > 0U) {
            mesh_str_format(right, sizeof right, MESH_STR_NODES_ROW_HOPS, (unsigned)node->hops_away,
                            age);
        } else if (node->via_mqtt) {
            mesh_str_format(right, sizeof right, MESH_STR_NODES_ROW_MQTT, age);
        } else if (mesh_ui_node_signal_heard(node)) {
            /*
             * Heard directly, with a reading of its own: rungs and the age, and the decibels go
             * to the node's own screen.
             *
             * The figure was the column's whole content and it is the part a list cannot use.
             * "4.2dB" has to be read and then held against a threshold to mean anything, and a
             * list is forty-two of them - whereas rungs are compared against the rungs above
             * and below without being read, which is the only thing a column of signals is
             * scanned for.
             */
            direct = true;
            mesh_str_copy(right, sizeof right, age);
        } else {
            /* Hops the firmware never reported, or no reading behind the figure. Exactly the
               column this list drew before, which is why MESH_STR_NODES_ROW_SNR keeps its
               entry - and what the CLI backend, which has no staircase, draws throughout. */
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
            tone = MESH_UI_TONE_PRIMARY;
        } else if (!node->in_nodedb) {
            tone = MESH_UI_TONE_DIM;
        }

        const struct fb_list_item row = {
            .leading =
                {
                    .kind = FB_LEADING_AVATAR,
                    .label = initials,
                    .tint = tint,
                    .role = is_me ? MESH_UI_COLOR_PRIMARY : MESH_UI_COLOR_COUNT,
                },
            .text = mesh_ui_line_text(&line),
            .tone = tone,
            .trailing = direct ? (struct fb_trailing){.kind = FB_TRAILING_SIGNAL,
                                                      .text = right,
                                                      .signal = mesh_ui_signal_level(node->snr)}
                               : (struct fb_trailing){.kind = FB_TRAILING_TEXT, .text = right},
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
        const bool is_draft = (i == MESH_UI_COMPOSE_ROW_DRAFT);
        mesh_ui_line_reset(&line);
        /* The draft is the row that opens the keyboard and the rest are texts to send as they
           stand, which is a difference in kind rather than in indentation - so it is the
           accent tone and the accent edge that say so, and the two spaces the canned rows used
           to be pushed over by are gone. */
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
            /* A started-but-unsent message is in flight, which is the tertiary - and the
               edge bar follows the tone, so the row is marked in the colour of the reason it
               is marked. */
            .tone = is_draft ? MESH_UI_TONE_TERTIARY : MESH_UI_TONE_NORMAL,
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
        fb_draw_empty(state, layout, MESH_UI_ICON_MESSAGES, mesh_str(MESH_STR_PICKER_EMPTY));
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
                    /* A channel's disc carries the tag rather than the '#' the nav layer hands
                       back, which is what the conversation list puts in the same disc. */
                    .icon = is_channel ? MESH_UI_ICON_CHANNEL : MESH_UI_ICON_NONE,
                    .tint = tint,
                    .role = current ? MESH_UI_COLOR_PRIMARY : MESH_UI_COLOR_COUNT,
                },
            .text = name,
            .tone = is_channel ? MESH_UI_TONE_PRIMARY : MESH_UI_TONE_NORMAL,
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
     * What is being typed, in the one text field this UI has. Everything about how it is drawn
     * - the raised tier that lifts it off the keyboard, the panel shape, the edge, which tail
     * of an overlong draft to show and where the counter sits - belongs to the component; what
     * is left here is the two facts only this screen knows, which is what it holds and how full
     * it is.
     */
    char meter[32];
    snprintf(meter, sizeof meter, "%zu/%zu", strlen(nav->draft), draft_cap);
    const struct fb_text_field field = {
        .value = nav->draft,
        .caret = true,
        .lines = 2U,
        .counter = meter,
    };
    fb_draw_text_field(state, layout, &y, &field);

    /* The character grid and the action row are the same button, sized differently. */
    const int margin = fb_margin(state);
    const int grid_w = (int)state->var.xres - 2 * margin;
    const int cell_w = grid_w / (int)MESH_UI_KB_COLS;
    const int cell_h = line + fb_space(state, MESH_UI_SPACE_MD);
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
        const enum mesh_ui_kb_action action = (enum mesh_ui_kb_action)col;
        /*
         * Four of the five keys are symbols, which is what a soft keyboard's action row is on
         * every platform: the words for them ("space", "delete", "send") are among the longest
         * strings in the catalog and these are the narrowest boxes on the screen. The layer key
         * keeps its label, because what it says - "ABC", "abc", "#!" - is the layer it switches
         * to, and no symbol carries that.
         */
        const enum mesh_ui_icon icon =
            action == MESH_UI_KB_ACTION_SPACE    ? MESH_UI_ICON_SPACE
            : action == MESH_UI_KB_ACTION_DELETE ? MESH_UI_ICON_BACKSPACE
            : action == MESH_UI_KB_ACTION_CANCEL ? MESH_UI_ICON_CLOSE
            : action == MESH_UI_KB_ACTION_SEND
                ? (nav->keyboard_field != MESH_UI_FIELD_NONE ? MESH_UI_ICON_CHECK
                                                             : MESH_UI_ICON_SEND)
                : MESH_UI_ICON_NONE;
        const struct fb_button button = {
            .rect = {.x = margin + (int)col * action_w,
                     .y = y,
                     .w = action_w - scale,
                     .h = cell_h - scale},
            .icon = icon,
            .label = icon == MESH_UI_ICON_NONE ? mesh_ui_kb_action_label(nav, action) : "",
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
        fb_draw_empty(state, layout, MESH_UI_ICON_DEVICES, mesh_str(MESH_STR_DEVICES_EMPTY));
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
            tone = MESH_UI_TONE_SUCCESS;
        } else if (armed) {
            tone = MESH_UI_TONE_ERROR;
        }

        /* The disc states its fill rather than taking a tint: a device list is four rows about
           one question - which of these am I on - and six hues would be answering a question
           nobody asked. Connected is the good tone, armed to be forgotten is the bad one.
           What it carries is the transport, not initials: the name is already the next thing
           on the row, and which bus a radio is on is the one fact about it the words do not
           repeat. */
        const struct fb_list_item row = {
            .leading =
                {
                    .kind = FB_LEADING_AVATAR,
                    .icon = device->kind == (uint8_t)MESH_UI_DEVICE_SERIAL ? MESH_UI_ICON_USB
                                                                           : MESH_UI_ICON_BLUETOOTH,
                    .tint = i,
                    .role = device->connected ? MESH_UI_COLOR_SUCCESS
                            : armed           ? MESH_UI_COLOR_ERROR
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
 * Where a busy mesh stops being healthy, in permille of the air.
 *
 * Not a look-and-feel number: above roughly a quarter, LoRa's listen-before-talk backs
 * everything off and multi-hop delivery starts failing outright, and by half the mesh is
 * effectively a single-hop one.
 *
 * One band rather than two constants, and it is handed to the bar rather than consulted
 * alongside it. Four things on this card now read these two numbers - the figure's colour, the
 * card heading's, the bar's fill and the notches cut into the bar's track - and the last of
 * those is the reason the shape changed: a threshold that is drawn has to be the same threshold
 * that is compared, or the screen is marking one boundary and colouring another.
 *
 * The numbers themselves moved to layout.h once a node's own detail screen started reading
 * them too. This is the band they make; where the mesh's limits actually are is stated there.
 */
static const struct mesh_ui_band fb_air_band = {.warn = MESH_UI_AIRTIME_BUSY_WARN,
                                                .bad = MESH_UI_AIRTIME_BUSY_BAD};

/*
 * Where a radio's free heap stops being comfortable, in bytes.
 *
 * Stated here for the reason the pair above is: the figure's colour and the card heading's are
 * two readings of one number, and a threshold written out at each of them is a card that can
 * head itself "fine" over a row it has just drawn as a warning.
 */
#define FB_HEAP_LOW_BYTES 20480U

/*
 * The airtime meter's key in the animation table.
 *
 * At the top of the range with the snackbar's, and for the same reason: every other id in here
 * is a row index or a field, handed in by a list that has many of them, and this is a control a
 * screen has exactly one of.
 */
#define FB_ANIM_ID_AIRTIME 0xFFFFFF02U

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
static void fb_render_status(struct mesh_ui_backend_fb_state *state,
                             const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    int y = layout->body_y;
    struct fb_card card;
    char buffer[64];
    char second[64];

    /* ---- the link: what we are talking to, and whether it has told us who it is ---- */

    const struct mesh_ui_device *connected = mesh_ui_snapshot_connected_device(snapshot);

    fb_card_begin(&card, MESH_UI_ICON_LINK, MESH_STR_STATUS_CARD_LINK,
                  connected != NULL ? MESH_UI_TONE_SUCCESS : MESH_UI_TONE_ERROR);
    fb_card_row_text(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_TRANSPORT,
                     snapshot->transport_status[0] != '\0'
                         ? snapshot->transport_status
                         : mesh_str(MESH_STR_HEADER_TRANSPORT_STARTING));
    fb_card_row_text(&card, connected != NULL ? MESH_UI_TONE_SUCCESS : MESH_UI_TONE_ERROR,
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
    /*
     * Above ~25% channel utilization the mesh is saturated and hop delivery collapses, so the
     * number is coloured rather than left as one more figure to interpret - and the card's
     * heading takes the same tone, which is what makes a saturated mesh visible from the shape
     * of the screen rather than from reading a percentage.
     *
     * The thresholds are stated once, in fb_air_band, and answer for the figure's colour, the
     * heading's, the meter's fill and the marks on its track alike - see mesh_ui_band_tone(). A
     * screen that worked them out separately for the words and for the bar would be drawing a
     * picture and a number that can disagree, and the picture is the one that gets believed.
     */
    const int32_t util_permille = mesh_ui_percent_permille(util_value);
    const enum mesh_ui_tone air_tone =
        have_util ? mesh_ui_band_tone(&fb_air_band, util_permille, MESH_UI_TONE_SUCCESS)
                  : MESH_UI_TONE_NORMAL;

    fb_card_begin(&card, MESH_UI_ICON_NODES, MESH_STR_STATUS_CARD_MESH,
                  air_tone != MESH_UI_TONE_NORMAL ? air_tone : MESH_UI_TONE_PRIMARY);
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
        /*
         * And the same number as a length, directly under the words.
         *
         * The row above says how busy the air is; this one says *busy*, and it is the one that
         * works from across a table. A percentage has to be read and then held against a
         * threshold nobody carries around - "is 31% a lot?" - where a bar a third full is
         * compared against the track it sits in, which is right there. The pair is the point:
         * neither replaces the other.
         *
         * Only when there is a real reading. A track drawn empty because the radio has not
         * reported yet says the mesh is quiet, which is a different claim from saying nothing.
         *
         * No label: the row above already names it twice over, and a label column here would
         * cost the track the third of its length that makes a fill readable as a proportion.
         *
         * The band goes with it, so the track carries a notch at a quarter and one at a half -
         * which is what turns "a bar a third full" into "a bar past the first mark". The reader
         * no longer has to know the threshold to see that it has been crossed.
         *
         * A zeroed scale: this reading is already permille, so there is no domain to state.
         */
        if (have_util) {
            fb_card_meter(&card, MESH_UI_TONE_SUCCESS, MESH_STR_NONE, util_permille,
                          (struct mesh_ui_scale){0, 0}, &fb_air_band, FB_ANIM_ID_AIRTIME);
        }
    }

    if (stats->valid) {
        fb_card_row(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_PACKETS,
                    MESH_STR_STATUS_PACKETS, stats->num_packets_tx, stats->num_packets_rx,
                    stats->num_tx_relay);
        /* Bad and dropped packets are the two numbers that explain a mesh that "works but loses
           messages", so they get their own row instead of being folded into Packets. */
        const bool losing = stats->num_packets_rx_bad > 0U || stats->num_tx_dropped > 0U;
        fb_card_row(&card, losing ? MESH_UI_TONE_WARNING : MESH_UI_TONE_DIM,
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
    const enum mesh_ui_tone notice_tone = notice->level >= 40U   ? MESH_UI_TONE_ERROR
                                          : notice->level >= 30U ? MESH_UI_TONE_WARNING
                                                                 : MESH_UI_TONE_NORMAL;
    const struct mesh_ui_queue_status *queue = &snapshot->settings.queue;
    /* The radio's send queue is only worth a row once it is under pressure or has just refused
       something: on an idle link it reads "16 of 16 free" for ever, which is one more number to
       skip past. A refusal keeps the row up because it is the explanation for a message that
       was never transmitted at all. */
    const bool have_queue =
        queue->valid && queue->maxlen > 0U && (queue->res != 0 || queue->free < queue->maxlen / 2U);

    /* Two more rows that only appear when something is off. Derived here rather than at the
       rows themselves because the heading is a reading of the same two facts, and a predicate
       written out twice is the pair that drifts. */
    const bool rebooted = snapshot->settings.reboot_notices > 0U;
    const bool low_heap =
        stats->valid && stats->has_heap && stats->heap_free_bytes < FB_HEAP_LOW_BYTES;

    /* The card reports the worst thing it holds. A refused packet and an ERROR notice are both
       the radio saying no; a flat battery is the reason it is about to. Below that, anything
       drawn in the warning tone heads the card in it too - a card saying "fine" over a row it
       has just drawn as a warning is the summary being wrong about its own contents. */
    enum mesh_ui_tone radio_tone = MESH_UI_TONE_PRIMARY;
    if (low_battery || (have_queue && queue->res != 0) || (have_notice && notice->level >= 40U)) {
        radio_tone = MESH_UI_TONE_ERROR;
    } else if ((have_notice && notice->level >= 30U) || rebooted || low_heap) {
        radio_tone = MESH_UI_TONE_WARNING;
    }

    /*
     * Ordered most-read first, which matters here and on no other card: this is the one that can
     * outgrow the panel - every row on it appears only when the radio is in some kind of
     * trouble, so the worst case is all of them at once - and fb_draw_card() drops from the end.
     * So the battery and the radio's own words come first and the heap figure last, because a
     * free-heap number is the row a user would have scrolled past anyway.
     */
    fb_card_begin(&card, MESH_UI_ICON_RADIO, MESH_STR_STATUS_CARD_RADIO, radio_tone);
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
        fb_card_row(&card, low_battery ? MESH_UI_TONE_ERROR : MESH_UI_TONE_NORMAL,
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
        /* A queue under pressure is work in flight, not a fault - the tertiary. A refusal is
           a fault, and takes the error family. */
        fb_card_row(&card, queue->res != 0 ? MESH_UI_TONE_ERROR : MESH_UI_TONE_TERTIARY,
                    MESH_STR_STATUS_LABEL_TX_QUEUE, MESH_STR_STATUS_TX_QUEUE, (unsigned)queue->free,
                    (unsigned)queue->maxlen,
                    queue->res != 0 ? mesh_str(MESH_STR_STATUS_TX_QUEUE_REFUSED) : "");
    }
    if (rebooted) {
        /* A radio that has restarted since we attached is not broken, but it is the first thing
           to know when something else looks wrong. */
        fb_card_row(&card, MESH_UI_TONE_WARNING, MESH_STR_STATUS_LABEL_REBOOTS,
                    MESH_STR_STATUS_REBOOTS_SINCE, snapshot->settings.reboot_notices);
    }
    if (stats->valid && stats->has_heap) {
        fb_card_row(&card, low_heap ? MESH_UI_TONE_WARNING : MESH_UI_TONE_DIM,
                    MESH_STR_STATUS_LABEL_HEAP, MESH_STR_STATUS_HEAP,
                    stats->heap_free_bytes / 1024U, stats->heap_total_bytes / 1024U);
    }
    (void)fb_draw_card(state, layout, &y, &card);
}

/* "Save <section>?" for the sections whose write can cut this client off, and "Reboot the
   radio?" and its siblings for the Radio actions section. Which of the two it is standing in
   front of is nav->confirm_action; all three strings come from settings.c. */
static void fb_render_confirm(struct mesh_ui_backend_fb_state *state,
                              const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const enum mesh_ui_settings_section section =
        (enum mesh_ui_settings_section)nav->settings_section;
    const enum mesh_ui_settings_action confirmed =
        (enum mesh_ui_settings_action)nav->confirm_action;
    char title[96];
    mesh_ui_settings_confirm_title(section, nav->settings_channel, confirmed, title, sizeof title);
    char text[256];
    mesh_ui_settings_confirm_text(section, confirmed, text, sizeof text);

    /*
     * A dialog rather than a screen. There is no title bar and no list: the question is the
     * panel's own headline and the two answers are buttons on it, which is the shape that says
     * "this is being asked of you" rather than "here is another list to walk".
     */
    const struct fb_dialog dialog = {
        .icon = MESH_UI_ICON_WARNING,
        .headline = title,
        .text = text,
        .accept = mesh_ui_settings_confirm_accept(confirmed),
        .cancel = mesh_str(MESH_STR_COMMON_CANCEL),
        .cursor = nav->confirm_cursor,
        /* A radio action cannot be taken back - a reboot drops the link, a NodeDB reset empties
           the roster - while a section save is only the settings the user has just been
           editing. The two deserve different-coloured answers. */
        .destructive = confirmed != (uint8_t)MESH_UI_SETTINGS_ACTION_NONE,
    };
    fb_draw_dialog(state, layout, &dialog);
}

/* Settings: the section list, or one section's label/value rows. Editable rows show a
   pending edit in place of the radio's value, marked with a dot until Y saves it. */
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
        fb_draw_empty(state, layout, MESH_UI_ICON_SETTINGS,
                      mesh_str(MESH_STR_SETTINGS_EMPTY_DISCONNECT));
        return;
    }

    const uint32_t count = section_open ? mesh_ui_settings_item_count(settings, handshake, section,
                                                                      nav->settings_channel)
                                        : mesh_ui_settings_root_count();
    if (count == 0U) {
        fb_draw_empty(state, layout, MESH_UI_ICON_SETTINGS,
                      mesh_str(MESH_STR_SETTINGS_EMPTY_SECTION));
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
            /*
             * What the row offers, in the marker gutter: the pencil on one Left and Right
             * change, and the dot on one already changed and not yet written. An action row
             * offers something else - it opens - and says so with the chevron every row that
             * opens something ends in, on the trailing edge rather than in the gutter.
             */
            const enum mesh_ui_icon marker = item.dirty ? MESH_UI_ICON_UNSAVED
                                             : item.field != MESH_UI_FIELD_NONE ? MESH_UI_ICON_EDIT
                                                                                : MESH_UI_ICON_NONE;
            const bool opens = (item.kind == MESH_UI_SETTING_ACTION);
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
            /*
             * A level gets a bar next to the words, on the same terms as a boolean getting a
             * switch instead of them: the fb backend deciding how to say what the item already
             * says. The row is keyed on its index, above everything the field enum can reach,
             * because a meter row has no field of its own - it is a fact, not a control.
             */
            if (item.kind == MESH_UI_SETTING_METER) {
                const bool unknown = item.number == MESH_UI_METER_UNKNOWN;
                struct fb_meter meter = {
                    .id = 0x03000000U | i,
                    .kind = unknown ? FB_METER_INDETERMINATE : FB_METER_DETERMINATE,
                    .value = unknown ? 0 : (int32_t)item.number,
                    .tone = MESH_UI_TONE_PRIMARY,
                };
                const struct fb_list_item row = {
                    .label = item.label,
                    .label_cols = label_cols,
                    .marker_icon = marker,
                    .value = item.value,
                    .tone = tone,
                    .trailing = {.kind = FB_TRAILING_METER, .meter = &meter},
                };
                fb_list_item(state, &list, i, &row);
                continue;
            }
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
                    .marker_icon = marker,
                    .tone = tone,
                    .trailing = {.kind = FB_TRAILING_SWITCH, .sw = &sw},
                };
                fb_list_item(state, &list, i, &row);
                continue;
            }
            const struct fb_list_item row = {
                .label = item.label,
                .label_cols = label_cols,
                .marker_icon = marker,
                .value = item.value,
                .tone = tone,
                .trailing = {.kind = FB_TRAILING_ICON,
                             .icon = opens ? MESH_UI_ICON_CHEVRON : MESH_UI_ICON_NONE},
            };
            fb_list_item(state, &list, i, &row);
        } else {
            const enum mesh_ui_settings_section section_row = mesh_ui_settings_root_at(i);
            const bool loaded = mesh_ui_settings_section_loaded(settings, handshake, section_row);
            const struct fb_list_item row = {
                .label = mesh_ui_settings_section_name(section_row),
                .label_cols = label_cols,
                .value = loaded ? "" : mesh_str(MESH_STR_SETTINGS_NOT_LOADED),
                .tone = loaded ? MESH_UI_TONE_NORMAL : MESH_UI_TONE_DIM,
                /* Every row here opens a section, which is what the section list *is*. */
                .trailing = {.kind = FB_TRAILING_ICON, .icon = MESH_UI_ICON_CHEVRON},
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
    layout.small = mesh_ui_theme_type_scale(state->theme, MESH_UI_TYPE_LABEL, state->scale);
    layout.line = fb_line_adv(state, state->scale);
    layout.cols = fb_cols(state, state->scale);

    fb_draw_nav_bar(state, &layout, fb_tab_chips(), MESH_UI_SCREEN_COUNT,
                    (size_t)snapshot->nav.screen);

    layout.footer_y = (int)state->var.yres - fb_action_bar_height(state, &layout);
    const int body_height = layout.footer_y - layout.body_y - fb_gutter(state);
    layout.rows = body_height > 0 ? (uint32_t)(body_height / layout.line) : 0U;

    /*
     * One tail for every path through this function, which is what lets the chrome below it be
     * written once. The overlays used to draw the footer and return, and each of the four
     * carried its own copy of that call - so anything drawn over the whole frame (the notice
     * below is the first) had to be added in five places or be missing from four screens.
     */
    if (snapshot->nav.confirm_open) {
        fb_render_confirm(state, snapshot, &layout);
    } else if (snapshot->nav.picker_open) {
        fb_render_picker(state, snapshot, &layout);
    } else if (snapshot->nav.keyboard_open) {
        fb_render_keyboard(state, snapshot, &layout);
    } else if (snapshot->nav.compose_open) {
        fb_render_compose(state, snapshot, &layout);
    } else {
        switch (snapshot->nav.screen) {
        case MESH_UI_SCREEN_MESSAGES:
            if (!snapshot->nav.thread_open) {
                fb_render_conversations(state, snapshot, &layout);
            } else {
                fb_render_thread(state, snapshot, &layout);
            }
            break;
        case MESH_UI_SCREEN_NODES:
            fb_render_nodes(state, snapshot, &layout);
            break;
        case MESH_UI_SCREEN_DEVICES:
            fb_render_devices(state, snapshot, &layout);
            break;
        case MESH_UI_SCREEN_SETTINGS:
            fb_render_settings(state, snapshot, &layout);
            break;
        case MESH_UI_SCREEN_STATUS:
        default:
            fb_render_status(state, snapshot, &layout);
            break;
        }
    }

    /*
     * Which buttons mean something here is no longer decided in this function.
     *
     * It used to be a branch per screen alongside the one above - the same conditions written
     * out twice, once to pick a renderer and once to pick a hint sentence, which is two places
     * to remember when a screen grows a press. mesh_ui_actions_for() answers from the snapshot
     * instead, walking the same chain of overlays, and it is a unit test's business rather than
     * a screenshot's.
     */
    struct mesh_ui_action_bar actions;
    mesh_ui_actions_for(snapshot, &actions);
    struct mesh_ui_line summary;
    enum mesh_ui_tone summary_tone = MESH_UI_TONE_DIM;
    fb_link_summary(snapshot, &summary, &summary_tone);
    const struct fb_action_bar bar = {
        .items = actions.items,
        .count = actions.count,
        .status = mesh_ui_line_text(&summary),
        .status_tone = summary_tone,
    };
    fb_draw_action_bar(state, &layout, &bar);

    /*
     * Last, because it is over the UI rather than in it: a notice that a screen could paint
     * over is a notice that is only visible on the screens that happen not to reach the bottom
     * of the body.
     */
    const struct fb_snackbar snackbar = {
        .text = snapshot->nav.toast,
        .until_ms = snapshot->nav.toast_until_ms,
    };
    fb_draw_snackbar(state, &layout, &snackbar);
}
