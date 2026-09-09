#define _POSIX_C_SOURCE 200809L

/*
 * One renderer per screen, plus the chrome and the frame that dispatches between them.
 *
 * Each takes an immutable snapshot and a layout. Derived transcript measurements are cached;
 * scroll windows are still selected from the current navigation state on every frame.
 * Adding a screen is a fb_render_* here and a case in fb_render_snapshot().
 *
 * Nothing in this file computes a pixel coordinate or a padding width. A row is built with
 * struct mesh_ui_line (which measures in cells) and handed to a component in fb_widgets.h,
 * so a renderer reads as a description of its content rather than as arithmetic.
 */

#include "fb_widgets.h"

#include "mesh/core/message.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/chrome.h"
#include "mesh/ui/delivery.h"
#include "mesh/ui/emoji.h"
#include "mesh/ui/help.h"
#include "mesh/ui/input.h"
#include "mesh/ui/layout.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/reactions.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/status.h"
#include "mesh/ui/waypoints.h"
#include "mesh/utils/text.h"
#include "mesh/utils/time.h"

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
    view->waypoints = snapshot->waypoints;
    view->read_state = snapshot->read_state;
    /* The radio's display units, which is what a waypoint's range is stated in. `nav` stays
       zeroed: nothing that takes a store reads it, and the screens that need one are handed it
       separately - a view that carried it would be a second copy of the cursor. */
    view->settings = snapshot->settings;
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
    case MESH_UI_SCREEN_WAYPOINTS:
        /* `place` - the same pin the Position settings section wears, and deliberately the same
           id: icons.def's rule is one id per job, and both are saying "somewhere on Earth". A
           second sprite drawing the same rune would be a second answer to one question. */
        return MESH_UI_ICON_POSITION;
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
    fb_draw_app_bar(state, layout, &(const struct fb_app_bar){.title = title});
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

/* A message as the screen describes it, with the strings the bubble points at.
 *
 * One buffer per slot rather than one buffer per line: the bubble's trailing run is four typed
 * parts it measures itself (struct fb_bubble_meta), and the screen's job is to fill the slots
 * rather than to assemble a line out of them. Concatenating them here is what used to let a
 * failure reason push the run past the bubble's own width. */
struct fb_thread_row {
    struct fb_bubble bubble;
    char separator[24];
    char name[48];
    char clock[8];
    char reactions[40];
    char note[64];
    char quote[64]; /* the message this one replies to; the bubble elides it to one line */
};

/* Both first-visible and ordinary variants are derived from exact message inputs. Cursor
   movement only chooses between them; it never reformats or remeasures the transcript. */
struct fb_thread_cache {
    bool valid;
    struct mesh_ui_message_list messages;
    uint32_t indices[MESH_UI_MAX_MESSAGES];
    uint32_t count;
    bool inbox;
    uint32_t target_node;
    const struct mesh_ui_theme *theme;
    const struct mesh_i18n_locale *locale;
    int scale;
    size_t cols;
    char calendar[80];
    struct fb_thread_row rows[2][MESH_UI_MAX_MESSAGES];
    uint8_t heights[2][MESH_UI_MAX_MESSAGES];
};

void fb_thread_cache_free(struct mesh_ui_backend_fb_state *state) {
    free(state->thread_cache);
    state->thread_cache = NULL;
}

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
    const time_t now = (time_t)mesh_time_wall_s();
    struct tm today;
    if (now > 0 && localtime_r(&now, &today) != NULL) {
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
 * What a threaded reply is answering, for the quote line inside its bubble.
 *
 * Looked up across the whole message list rather than the filtered transcript, because the two
 * are not the same set: in a channel the target is in the transcript, and after a delete or a
 * ring eviction it is in neither - which is why nothing is a normal answer here and the bubble
 * simply loses its quote. A reaction is never quoted: it has no bubble to be answered from,
 * and a reply *to* one is not a thing any client makes.
 */
static void fb_thread_quote(const struct mesh_ui_snapshot *snapshot, uint32_t reply_id, char *out,
                            size_t out_len) {
    out[0] = '\0';
    if (reply_id == 0U) {
        return;
    }
    const uint32_t total = snapshot->messages.count > MESH_UI_MAX_MESSAGES
                               ? MESH_UI_MAX_MESSAGES
                               : snapshot->messages.count;
    for (uint32_t i = total; i > 0U; --i) {
        const struct mesh_ui_message *target = &snapshot->messages.entries[i - 1U];
        if (target->packet_id != reply_id || target->is_reaction) {
            continue;
        }
        /* Sanitised rather than copied: this truncates, and mesh_str_copy truncates by bytes -
           which on a message ending in an emoji would cut a character in half. */
        mesh_text_sanitise_str(target->text, out, out_len);
        return;
    }
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
    row->bubble.note = row->note;
    row->bubble.quote = row->quote;
    row->bubble.meta.reactions = row->reactions;
    row->bubble.meta.clock = row->clock;
    /* A reaction never reaches a bubble - the transcript filters it out - so anything here
       carrying a reply_id is a threaded reply, and the quote is what says so. */
    fb_thread_quote(snapshot, message->reply_id, row->quote, sizeof row->quote);

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

    /*
     * The trailing run: when it arrived, whether it went out encrypted, and for ours what
     * became of it. Four slots the bubble measures for itself rather than a line assembled
     * here - see struct fb_bubble_meta for why that distinction is the whole of it.
     */
    fb_format_clock(message->rx_time, row->clock, sizeof row->clock);

    /*
     * A padlock on a message the radio decrypted with our key pair rather than with a channel
     * PSK. It only means anything on a direct message, and it is worth saying there: on a
     * channel still using the default key every node on the mesh holds that key, so a DM that
     * did *not* go out PKI-encrypted was readable by all of them, and nothing else on the
     * screen distinguishes the two.
     */
    if (message->pki_encrypted && !message->broadcast) {
        row->bubble.meta.lock = MESH_UI_ICON_ENCRYPTED;
    }

    /* What became of one of ours, as src/ui/delivery.c answers - the mark for the corner, and
       the word for the line below when there is nothing better to put there. Which one a state
       gets is a decision the transcript reads rather than makes. */
    const struct mesh_ui_delivery delivery = mesh_ui_delivery_of(message->ack);
    if (outbound) {
        row->bubble.meta.state = delivery.icon;
    }

    /*
     * And why, when it failed. A reason is a sentence, so it goes under the message as a
     * supporting line rather than into the corner beside the clock: "!!" alone left the user
     * with no idea whether to move, retry or fix a key, and those are different problems, but a
     * corner mark is not where a sentence can live.
     *
     * Routing_Error NONE reads as "delivered", which on a failed message is a straight
     * contradiction. It should not reach us - a failure carries a reason - but a bubble is the
     * wrong place to find out that it did, so the generic word stands in for it.
     */
    if (row->bubble.failed) {
        mesh_str_copy(row->note, sizeof row->note,
                      message->ack_error != 0U
                          ? mesh_message_ack_error_to_string(message->ack_error)
                          : mesh_str(delivery.word));
    }

    /* Reactions ride the trailing run rather than taking a row: they are an annotation on this
       bubble, and a row of their own is the bubble they were filtered out of being. */
    fb_thread_reactions(snapshot, message->packet_id, row->reactions, sizeof row->reactions);
}

/* A bubble's height, clamped into the byte the transcript window measures in. */
static uint8_t fb_thread_height(const struct mesh_ui_backend_fb_state *state,
                                const struct fb_layout *layout, const struct fb_thread_row *row) {
    const uint32_t rows = fb_bubble_rows(state, layout, &row->bubble);
    return rows > 0xFFU ? 0xFFU : (uint8_t)rows;
}

static struct fb_thread_cache *fb_thread_cache_get(struct mesh_ui_backend_fb_state *state,
                                                   const struct mesh_ui_snapshot *snapshot,
                                                   const struct fb_layout *layout,
                                                   const uint32_t *indices, uint32_t count) {
    if (state->thread_cache_disabled) {
        return NULL;
    }
    if (state->thread_cache == NULL) {
        state->thread_cache = calloc(1U, sizeof *state->thread_cache);
    }
    struct fb_thread_cache *cache = state->thread_cache;
    if (cache == NULL) {
        return NULL;
    }
    /* Include local calendar and zone, so midnight and a timezone change invalidate labels. */
    char calendar[80] = {0};
    const time_t now = (time_t)mesh_time_wall_s();
    struct tm local;
    if (localtime_r(&now, &local) != NULL) {
        (void)strftime(calendar, sizeof calendar, "%Y-%m-%d %Z %z", &local);
    }
    const bool changed =
        !cache->valid || cache->count != count || cache->inbox != snapshot->nav.inbox ||
        cache->target_node != snapshot->nav.target_node || cache->theme != state->theme ||
        cache->locale != mesh_i18n_locale() || cache->scale != state->scale ||
        cache->cols != layout->cols || strcmp(cache->calendar, calendar) != 0 ||
        memcmp(cache->indices, indices, count * sizeof *indices) != 0 ||
        memcmp(&cache->messages, &snapshot->messages, sizeof cache->messages) != 0;
    if (changed) {
        cache->messages = snapshot->messages;
        memcpy(cache->indices, indices, count * sizeof *indices);
        cache->count = count;
        cache->inbox = snapshot->nav.inbox;
        cache->target_node = snapshot->nav.target_node;
        cache->theme = state->theme;
        cache->locale = mesh_i18n_locale();
        cache->scale = state->scale;
        cache->cols = layout->cols;
        memcpy(cache->calendar, calendar, sizeof calendar);
        for (unsigned variant = 0U; variant < 2U; ++variant) {
            for (uint32_t i = 0U; i < count; ++i) {
                fb_thread_row_build(snapshot, indices, i, variant != 0U, &cache->rows[variant][i]);
                cache->heights[variant][i] =
                    fb_thread_height(state, layout, &cache->rows[variant][i]);
            }
        }
        cache->valid = true;
    }
    return cache;
}

static void fb_thread_row_get(const struct mesh_ui_snapshot *snapshot, const uint32_t *indices,
                              uint32_t position, bool force_name,
                              const struct fb_thread_cache *cache, struct fb_thread_row *row) {
    if (cache == NULL) {
        fb_thread_row_build(snapshot, indices, position, force_name, row);
        return;
    }
    *row = cache->rows[force_name ? 1 : 0][position];
    /* Never retain pointers into a caller-owned snapshot, or into a copied row's strings. */
    row->bubble.text = snapshot->messages.entries[indices[position]].text;
    row->bubble.separator = row->separator;
    row->bubble.name = row->name;
    row->bubble.note = row->note;
    row->bubble.quote = row->quote;
    row->bubble.meta.reactions = row->reactions;
    row->bubble.meta.clock = row->clock;
}

static void fb_render_thread(struct mesh_ui_backend_fb_state *state,
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
    /* No overline. Which kind of conversation this is stays in the title, because a channel's
       name already starts with a '#' and every bubble under it is tagged - so a trail would be
       spending a body row of transcript to repeat what two other things on the frame say. */
    fb_draw_app_bar(state, layout, &(const struct fb_app_bar){.title = title});

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
    struct fb_thread_cache *cache = fb_thread_cache_get(state, snapshot, layout, indices, count);
    for (uint32_t i = 0; i < count; ++i) {
        if (cache != NULL) {
            heights[i] = cache->heights[0][i];
        } else {
            fb_thread_row_build(snapshot, indices, i, false, &row);
            heights[i] = fb_thread_height(state, layout, &row);
        }
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
            if (cache != NULL) {
                heights[named] = cache->heights[0][named];
            } else {
                fb_thread_row_build(snapshot, indices, named, false, &row);
                heights[named] = fb_thread_height(state, layout, &row);
            }
        }
        named = window.first;
        if (cache != NULL) {
            heights[named] = cache->heights[1][named];
        } else {
            fb_thread_row_build(snapshot, indices, named, true, &row);
            heights[named] = fb_thread_height(state, layout, &row);
        }
        window = mesh_ui_transcript_window(heights, count, cursor, layout->rows);
    }
    /* Only force what the heights were settled against, so the draw can never disagree with the
       measure even if the loop ran out of passes. */
    const bool settled = (named == window.first);

    int y = layout->body_y + (int)window.pad * layout->line;
    for (uint32_t i = window.first; i < window.first + window.count && i < count; ++i) {
        fb_thread_row_get(snapshot, indices, i, settled && i == named, cache, &row);
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
        fb_draw_app_bar(state, layout,
                        &(const struct fb_app_bar){.title = mesh_str(MESH_STR_TAB_NODES)});
        fb_draw_empty(state, layout, MESH_UI_ICON_NODES, mesh_str(MESH_STR_NODES_GONE));
        return;
    }

    const bool is_self = hs->has_my_info && node->node_id == hs->my_info.node_num;
    char title[96];
    const char *name = node->long_name[0] != '\0'    ? node->long_name
                       : node->short_name[0] != '\0' ? node->short_name
                                                     : NULL;
    if (name != NULL) {
        mesh_str_copy(title, sizeof title, name);
    } else {
        /* A node with no User is named after its node number, exactly as the phone apps do. */
        mesh_str_format(title, sizeof title, MESH_STR_NODE_VAL_USER_ID_HEX, node->node_id);
    }
    /*
     * "Nodes > %s" was a breadcrumb inside a translated string, and the "Nodes >" half of it
     * was the navigation bar's job all along: the tab is up there, selected, three rows above.
     * So the trail is empty and the node keeps the whole title line for a name the radio chose
     * and we cannot bound. What the breadcrumb was really carrying - that B leaves - is the
     * leading arrow now, which says it in a cell rather than in seven.
     */
    fb_draw_app_bar(state, layout, &(const struct fb_app_bar){.title = title});

    struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
    const uint32_t count = mesh_ui_node_detail_build(
        node, is_self, mesh_time_wall_s(), &snapshot->traceroute, nav->node_remove_armed,
        &snapshot->handshake, &snapshot->history, items, MESH_UI_NODE_ITEMS_MAX);
    if (count == 0U) {
        fb_draw_empty(state, layout, MESH_UI_ICON_NODES, mesh_str(MESH_STR_NODES_DETAIL_EMPTY));
        return;
    }

    const size_t label_cols = fb_field_label_cols(state, layout, 16U);
    /*
     * The one list on the device whose rows are not all the same height, and the reason the
     * window learned to count steps: a reading gets a bar with the row to itself, so the
     * decibels and the percentages on this screen can be judged rather than merely read.
     *
     * Measured here, from the same `kind` the loop below draws from, and handed to the model
     * before anything is placed - which is what stops the window and the draw from ever being
     * a row apart.
     */
    uint8_t heights[MESH_UI_NODE_ITEMS_MAX];
    for (uint32_t r = 0; r < count; ++r) {
        heights[r] = items[r].kind == MESH_UI_NODE_ROW_METER ? 2U : 1U;
    }
    struct fb_list list =
        fb_list_begin_heights(layout, count, nav->cursor[MESH_UI_SCREEN_NODES], heights);
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        const struct mesh_ui_node_item *item = &items[i];
        if (item->kind == MESH_UI_NODE_ROW_HEADING) {
            fb_list_subheader(state, &list, i, item->label);
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
            /*
             * And, where the client has been watching one, which way the reading has been
             * going - in the trailing slot, beside the figure rather than under it.
             *
             * The two pictures on this row are deliberately different sizes, because they are
             * different weights of question. Where a battery sits between flat and full is what
             * the screen is for, so it gets the row's second step and the bands with it;
             * whether it has been falling is a glance, so it gets six cells against the edge.
             * A trend given the full-width treatment would have taken a third step and said the
             * word "battery" three times down one screen.
             */
            struct mesh_ui_polyline points;
            mesh_ui_series_project(item->trend, item->scale, &points);
            struct fb_sparkline trend = {.points = &points, .tone = MESH_UI_TONE_PRIMARY};
            const struct fb_list_item row = {
                .label = item->label,
                .label_cols = label_cols,
                .value = item->value,
                .tone = MESH_UI_TONE_NORMAL,
                .meter = &meter,
                .trailing = {.kind = FB_TRAILING_SPARK, .spark = &trend},
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

/* ---- the waypoints ---------------------------------------------------------------------------
 *
 * The list of places, and one of them opened. Two levels, the Nodes tab's shape - what differs
 * is that a row's trailing column is a *range* rather than a signal, because how far away a
 * place is and which way it lies is the whole of what a client with no map can say about a
 * point. src/ui/waypoints.c works both out; this draws them.
 */

static void fb_render_waypoint_detail(struct mesh_ui_backend_fb_state *state,
                                      const struct mesh_ui_snapshot *snapshot,
                                      struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_waypoint *waypoint =
        mesh_ui_waypoint_find(&snapshot->waypoints, nav->waypoint_detail_id);
    if (waypoint == NULL) {
        fb_draw_app_bar(state, layout,
                        &(const struct fb_app_bar){.title = mesh_str(MESH_STR_TAB_WAYPOINTS)});
        fb_draw_empty(state, layout, MESH_UI_ICON_POSITION, mesh_str(MESH_STR_WAYPOINTS_GONE));
        return;
    }

    /* The place's own name has the whole title line, exactly as a node's does: the tab is up
       there in the navigation bar already, so a trail would be repeating it. */
    fb_draw_app_bar(state, layout,
                    &(const struct fb_app_bar){
                        .title = waypoint->name[0] != '\0' ? waypoint->name
                                                           : mesh_str(MESH_STR_WAYPOINTS_UNNAMED)});

    struct mesh_ui_waypoint_item items[MESH_UI_WAYPOINT_ITEMS_MAX];
    const uint32_t count = mesh_ui_waypoint_detail_build(
        waypoint, snapshot->handshake_valid ? &snapshot->handshake : NULL, &snapshot->settings,
        /* The credible clock rather than the machine's: this screen's "Expires" row subtracts
           from it, and a Brick that has not been told the date would otherwise report every
           deadline as tens of thousands of days away rather than saying it cannot tell. */
        mesh_time_wall_credible_s(), nav->waypoint_delete_armed, items, MESH_UI_WAYPOINT_ITEMS_MAX);
    if (count == 0U) {
        fb_draw_empty(state, layout, MESH_UI_ICON_POSITION, mesh_str(MESH_STR_WAYPOINTS_GONE));
        return;
    }

    const size_t label_cols = fb_field_label_cols(state, layout, 12U);
    /* The note is the one row here that is a sentence rather than a fact, so it takes a second
       step and puts the sharer's words on it. Measured from the same `kind` the loop draws
       from, and handed to the model before anything is placed - the node detail's rule. */
    uint8_t heights[MESH_UI_WAYPOINT_ITEMS_MAX];
    for (uint32_t r = 0; r < count; ++r) {
        heights[r] = items[r].kind == MESH_UI_WAYPOINT_ITEM_NOTE ? 2U : 1U;
    }
    struct fb_list list =
        fb_list_begin_heights(layout, count, nav->cursor[MESH_UI_SCREEN_WAYPOINTS], heights);
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        const struct mesh_ui_waypoint_item *item = &items[i];
        if (item->kind == MESH_UI_WAYPOINT_ITEM_HEADING) {
            fb_list_subheader(state, &list, i, item->label);
            continue;
        }
        if (item->kind == MESH_UI_WAYPOINT_ITEM_NOTE) {
            /*
             * The sharer's own sentence, wrapped across the row's whole width rather than
             * squeezed into a value column sized for a coordinate.
             *
             * Two steps, and the wrap is the layout's own walk rather than a split at some
             * character count: upstream caps a description at a hundred characters and the row
             * is nearly sixty cells wide, so two lines hold every description that can exist -
             * which is why the second line is the item's `supporting` slot rather than the
             * beginning of a third row nothing would measure.
             */
            char first[MESH_UI_WAYPOINT_VALUE_MAX];
            char second[MESH_UI_WAYPOINT_VALUE_MAX];
            first[0] = '\0';
            second[0] = '\0';
            struct mesh_ui_wrap wrap;
            mesh_ui_wrap_begin(&wrap, item->value, list.cols);
            if (mesh_ui_wrap_next(&wrap)) {
                mesh_str_copy(first, sizeof first, wrap.line);
            }
            if (mesh_ui_wrap_next(&wrap)) {
                mesh_str_copy(second, sizeof second, wrap.line);
            }
            const struct fb_list_item row = {
                .text = first,
                .tone = MESH_UI_TONE_DIM,
                .supporting = second[0] != '\0' ? second : NULL,
                .supporting_quiet = true,
            };
            fb_list_item(state, &list, i, &row);
            continue;
        }
        if (item->kind == MESH_UI_WAYPOINT_ITEM_ACTION) {
            const struct fb_list_item row = {
                .text = item->label,
                /* The armed delete is the one row on this screen that shouts, because the next
                   press takes the place off the mesh for everybody. */
                .tone = (item->action == (uint8_t)MESH_UI_WAYPOINT_ACTION_DELETE &&
                         nav->waypoint_delete_armed)
                            ? MESH_UI_TONE_ERROR
                            : MESH_UI_TONE_PRIMARY,
                .trailing = {.kind = FB_TRAILING_ICON, .icon = MESH_UI_ICON_CHEVRON},
            };
            fb_list_item(state, &list, i, &row);
            continue;
        }
        const struct fb_list_item row = {
            .label = item->label,
            .label_cols = label_cols,
            .value = item->value,
        };
        fb_list_item(state, &list, i, &row);
    }
}

static void fb_render_waypoints(struct mesh_ui_backend_fb_state *state,
                                const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    if (nav->waypoint_detail_open) {
        fb_render_waypoint_detail(state, snapshot, layout);
        return;
    }

    struct mesh_ui_store view;
    fb_store_view(snapshot, &view);
    const uint32_t count = mesh_ui_waypoint_count(&view);
    const uint32_t places = count > 0U ? count - 1U : 0U;

    char title[96];
    if (places > 0U) {
        mesh_str_format(title, sizeof title, MESH_STR_WAYPOINTS_TITLE_COUNT, places);
    } else {
        mesh_str_copy(title, sizeof title, mesh_str(MESH_STR_TAB_WAYPOINTS));
    }
    fb_draw_app_bar(state, layout, &(const struct fb_app_bar){.title = title});

    /*
     * The empty state still draws the list, because the list is never empty: the last row makes
     * a place, and a screen that replaced it with a picture would take away the only thing
     * there is to do here. The picture goes above the one row instead - which is why this is a
     * banner-shaped sentence rather than fb_draw_empty()'s full-body one.
     */
    struct fb_list list =
        fb_list_begin_rows(layout, count, nav->cursor[MESH_UI_SCREEN_WAYPOINTS], 2U);
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        struct mesh_ui_waypoint_row waypoint;
        if (!mesh_ui_waypoint_row(&view, i, &waypoint)) {
            break;
        }
        const bool is_new = (waypoint.type == MESH_UI_WAYPOINT_ROW_NEW);
        const struct fb_list_item row = {
            .leading = {.kind = FB_LEADING_ICON,
                        .icon = is_new ? MESH_UI_ICON_COMPOSE : MESH_UI_ICON_POSITION},
            .text = waypoint.name,
            /* Ours in the accent, the way the node list marks our own radio: a place you shared
               is one you can withdraw, and that is worth seeing from the list. The row that
               makes a place is dim for the same reason the "New message" row is - it is a
               button among things, not a thing. */
            .tone = is_new ? MESH_UI_TONE_DIM
                           : (waypoint.ours ? MESH_UI_TONE_PRIMARY : MESH_UI_TONE_NORMAL),
            /* The range, in the column a node row puts its signal in - the same question asked
               of a place instead of a link. Empty when there is no answer, which draws nothing
               rather than a zero. */
            .trailing = {.kind = FB_TRAILING_TEXT, .text = waypoint.range},
            /* The row that makes a place has nothing shared to report, so it says what the
               screen would otherwise have had to say in a picture: that nothing is here yet.
               Once something is, it stops saying it - a list with places in it is not empty,
               and the row is then only a button. */
            .supporting = is_new ? (places == 0U ? mesh_str(MESH_STR_WAYPOINTS_EMPTY) : NULL)
                                 : waypoint.shared,
            .supporting_quiet = true,
        };
        fb_list_item(state, &list, i, &row);
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
        fb_draw_app_bar(state, layout,
                        &(const struct fb_app_bar){.title = mesh_str(MESH_STR_TAB_NODES)});
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
    /*
     * Two things can be bigger than this list, and the honest "of" is whichever is bigger.
     * The radio's database is one; the roster is the other, and it is the one that used to go
     * unsaid - it holds 256 and the UI publishes its best 128, so a busy mesh quietly dropped
     * half of what the client knew with the title still reading "128 nodes". After a NodeDB
     * reset the radio's number is the smaller of the two, which is exactly when taking the max
     * matters rather than preferring either.
     */
    const uint32_t known_by_radio = hs->has_my_info ? hs->my_info.nodedb_entries : 0U;
    const uint32_t known = hs->nodes_known > known_by_radio ? hs->nodes_known : known_by_radio;
    if (known > count) {
        mesh_str_format(title, sizeof title, MESH_STR_NODES_TITLE_OF, count, known);
    } else if (off_radio > 0U) {
        /* The count the Status screen shows is the radio's; this one is ours, and after a
           NodeDB reset the two are nothing alike. Saying how much of the gap is nodes only we
           remember is what keeps "81 here, 2 there" from reading as a bug. */
        mesh_str_format(title, sizeof title, MESH_STR_NODES_TITLE_OFF_RADIO, count, off_radio);
    } else {
        fb_title_count(title, sizeof title, mesh_str(MESH_STR_TAB_NODES), count, 0U);
    }
    fb_draw_app_bar(state, layout, &(const struct fb_app_bar){.title = title});

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
         * The star sits in the marker gutter, beside the name rather than in place of the
         * identity the disc is carrying: being pinned is a fact about the node, and a fact one
         * cell wide before the words is exactly what that slot is. It is reserved on every row
         * of this list and filled on the pinned ones, so the names stay in one column.
         *
         * Never on ourselves, which is what the old marker column got right by ordering the two
         * cases. A radio can carry a stale `is_favorite` on its own NodeDB entry, and nav.c and
         * node_detail.c both refuse to pin our own node - so a star there would advertise a
         * preference that no press can clear.
         */
        mesh_ui_line_reset(&line);
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
            .marker_icon = (node->is_favorite && !is_me) ? MESH_UI_ICON_PINNED : MESH_UI_ICON_NONE,
            .marker_slot = true,
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

/*
 * The tapback picker: the fixed emoji set, one per row, aimed at the message X was pressed on.
 *
 * The message it is about is the heading rather than a row, for the reason the compose sheet's
 * destination is: nothing on this screen chooses it, so a row that looked pressable would be
 * offering a choice that is already made.
 */
static void fb_render_reactions(struct mesh_ui_backend_fb_state *state,
                                const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    char target[96] = {0};
    fb_thread_quote(snapshot, nav->reply_to, target, sizeof target);
    char title[160];
    mesh_str_format(title, sizeof title, MESH_STR_REACT_TO, target);
    fb_draw_app_bar(state, layout, &(const struct fb_app_bar){.title = title});

    const uint32_t count = mesh_ui_nav_reaction_row_count();
    struct fb_list list = fb_list_begin(layout, count, nav->reaction_cursor);
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        /* The glyph in the leading slot and the word beside it: eight faces in a column at
           this scale are not eight distinguishable things, and a text backend has no sprites
           for any of them. */
        const struct fb_list_item row = {
            .leading = {.kind = FB_LEADING_AVATAR,
                        .label = mesh_ui_reaction_emoji(i),
                        /* A stated neutral fill rather than a tint: an avatar's colour is how
                           the eye tells one node from another, and here the glyph inside it is
                           already the whole of what the row is. */
                        .role = MESH_UI_COLOR_SURFACE_SEL},
            .text = mesh_str(mesh_ui_reaction_label(i)),
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
    /* The badge is the one thing on the sheet that says this answers a message rather than the
       conversation - which is the whole difference between the press that opened it and Y. */
    fb_draw_app_bar(
        state, layout,
        &(const struct fb_app_bar){
            .title = title,
            .badge = nav->reply_to != 0U ? mesh_str(MESH_STR_COMPOSE_BADGE_REPLY) : NULL,
            .badge_family = MESH_UI_FAMILY_TERTIARY,
        });

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
    fb_draw_app_bar(state, layout, &(const struct fb_app_bar){.title = title});
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
         * The disc also carries the "(channel)" suffix the row used to spell out, as the tag.
         *
         * What it no longer carries is which row is the current target. That was a stated
         * accent fill, and the fill is exactly the thing the disc is *for*: a node is the same
         * two letters and the same colour everywhere in this client, and marking the target
         * overwrote the second of those on precisely the row the eye was looking for. It is a
         * radio in the trailing slot now - identity at the leading edge, selection at the
         * trailing one, which is the division every list of choices makes and the reason the
         * two were fighting over one disc.
         */
        uint32_t tint = 0U;
        mesh_ui_nav_target_avatar(&view, node, channel, initials, sizeof initials, &tint);
        /* Keyed on the row, above everything the settings fields reach. The picker closes on the
           press that changes the answer, so this never animates in place - it is the same
           control the rest of the UI uses, drawn at rest. */
        struct fb_selection sel = {
            .id = 0x04000000U | i,
            .on = current,
        };
        const struct fb_list_item row = {
            .leading =
                {
                    .kind = FB_LEADING_AVATAR,
                    .label = initials,
                    /* A channel's disc carries the tag rather than the '#' the nav layer hands
                       back, which is what the conversation list puts in the same disc. */
                    .icon = is_channel ? MESH_UI_ICON_CHANNEL : MESH_UI_ICON_NONE,
                    .tint = tint,
                    .role = MESH_UI_COLOR_COUNT,
                },
            .text = name,
            .tone = is_channel ? MESH_UI_TONE_PRIMARY : MESH_UI_TONE_NORMAL,
            .trailing = {.kind = FB_TRAILING_RADIO, .sel = &sel},
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
    /* The same badge the compose sheet carries, for the same reason: this keyboard was raised
       over a bubble, and the destination in the title is not what says so. A setting's keyboard
       and the pairing prompt never carry one - `reply_to` belongs to the thread. */
    const bool replying = (!for_passkey && !for_setting && nav->reply_to != 0U);
    fb_draw_app_bar(state, layout,
                    &(const struct fb_app_bar){
                        .title = title,
                        .badge = replying ? mesh_str(MESH_STR_COMPOSE_BADGE_REPLY) : NULL,
                        .badge_family = MESH_UI_FAMILY_TERTIARY,
                    });

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
    fb_draw_app_bar(state, layout, &(const struct fb_app_bar){.title = title});

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
    char attach[16];
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        const struct mesh_ui_device *device = &snapshot->devices[i];
        const char *name = device->name[0] != '\0' ? device->name : device->identifier;
        if (name[0] == '\0') {
            name = mesh_str(MESH_STR_DEVICES_UNNAMED);
        }
        /*
         * What pressing A on this row would do. An unpaired BLE node is the case worth
         * calling out: it connects and then fails on StartNotify unless it is bonded first,
         * which is exactly what A now does for it.
         *
         * It is a capsule rather than a word, which is what the catalog ids have called it
         * since they were written. A device list is four rows about one question - which of
         * these am I on - so the state is the thing the eye is scanning for, and a state set
         * as prose on a supporting line is the one shape that cannot be scanned: every row
         * reads the same until it has been read. The family is the sentence: good for the one
         * we are on, the accent for a step in flight, warning for the one that will fail on
         * StartNotify until it is bonded.
         *
         * `paired` gets no capsule, and that is the whole of what the other three are worth.
         * It is the resting state of a bonded radio - every row in a list of known radios has
         * it - so a pill there is on every row at once, which is a column of colour reporting
         * nothing. Worse than nothing on two themes: the contrast palette has one yellow and
         * the colourblind palette one blue, so a resting capsule came out the same colour as
         * the warning beside it on the first and as `connected` on the second. A quiet word in
         * the same slot says the same thing and leaves the colour to the rows that have
         * something to report. Anything a badge does not shout is a badge that should not be
         * there.
         */
        const char *status = "";
        enum mesh_ui_family status_family = MESH_UI_FAMILY_PRIMARY;
        bool status_badge = true;
        if (device->connected) {
            status = mesh_str(MESH_STR_DEVICES_BADGE_CONNECTED);
            status_family = MESH_UI_FAMILY_SUCCESS;
        } else if (device->busy) {
            status = mesh_str(MESH_STR_DEVICES_BADGE_WORKING);
        } else if (device->kind == (uint8_t)MESH_UI_DEVICE_BLE && !device->paired) {
            status = mesh_str(MESH_STR_DEVICES_BADGE_NEEDS_PAIR);
            status_family = MESH_UI_FAMILY_WARNING;
        } else if (device->kind == (uint8_t)MESH_UI_DEVICE_BLE) {
            status = mesh_str(MESH_STR_DEVICES_BADGE_PAIRED);
            status_badge = false;
        }

        /* A USB port has no RSSI to show, so it says which bus it is instead - the supporting
           line answers "how is this attached" either way. */
        if (device->kind == (uint8_t)MESH_UI_DEVICE_SERIAL) {
            mesh_str_copy(attach, sizeof attach, mesh_str(MESH_STR_DEVICES_TRAILING_USB));
        } else {
            mesh_str_format(attach, sizeof attach, MESH_STR_DEVICES_TRAILING_RSSI,
                            (int)device->rssi);
        }

        const bool armed = nav->devices_forget_armed && nav->devices_forget_row == i;
        enum mesh_ui_tone tone = MESH_UI_TONE_NORMAL;
        if (device->connected) {
            tone = MESH_UI_TONE_SUCCESS;
        } else if (armed) {
            tone = MESH_UI_TONE_ERROR;
        }
        /* A row armed to be forgotten says so in every part of itself, the resting state
           included: the capsule reports the link, which is a different fact, but a red row
           carrying a green pill is two rows' worth of statement in one and the press being
           asked about is the destructive one. */
        if (armed) {
            status_family = MESH_UI_FAMILY_ERROR;
            status_badge = true;
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
            /*
             * The two facts swap lines, and the order is the point. The capsule takes the
             * headline's trailing edge because how a radio is attached is a detail and whether
             * it is the one we are on is not; how well we hear it drops to the supporting line,
             * where it keeps the dim ink it already had.
             *
             * A row with nothing to report - a USB port that is merely present - draws no
             * capsule and loses nothing: the slot is right-aligned, so unlike the leading
             * gutter an empty one costs no column, and neither kind draws at all on an empty
             * string.
             */
            .trailing = status_badge
                            ? (struct fb_trailing){.kind = FB_TRAILING_BADGE,
                                                   .family = status_family,
                                                   .text = status}
                            : (struct fb_trailing){.kind = FB_TRAILING_TEXT, .text = status},
            .supporting = attach,
            .supporting_tone = MESH_UI_TONE_DIM,
            /* A figure is something the eye glances at on its way past, on the ground and under
               the cursor alike - which is what the trailing slot it used to sit in already did
               for it, and what it keeps here. */
            .supporting_quiet = true,
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
 * The verbs one card offers, hung on the card that offers them.
 *
 * The flat list is walked rather than the card asked what it wants, so the order the buttons
 * draw in is the order the cursor walks them by construction - see include/mesh/ui/status.h.
 * `focus` is the screen cursor; the button it lands on is the one that draws filled, and a card
 * holding it draws its focus ring.
 */
static void fb_status_card_actions(struct fb_card *card,
                                   const struct mesh_ui_status_actions *actions,
                                   enum mesh_ui_status_card which, uint32_t focus) {
    for (uint32_t i = 0U; i < actions->count && i < MESH_UI_STATUS_ACTIONS_MAX; ++i) {
        if (actions->items[i].card != (uint8_t)which) {
            continue;
        }
        fb_card_action(card, actions->items[i].label, i == focus);
    }
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
static void fb_render_status(struct mesh_ui_backend_fb_state *state,
                             const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    int y = layout->body_y;
    struct fb_card card;
    char buffer[64];
    char second[64];

    /* ---- the link: what we are talking to, and whether it has told us who it is ---- */

    const struct mesh_ui_device *connected = mesh_ui_snapshot_connected_device(snapshot);

    /*
     * The verbs on offer and which of them the cursor is on. Both come from the same table
     * nav.c walks and the action bar names, so the button that draws filled here is the one A
     * will run - see include/mesh/ui/status.h.
     */
    struct mesh_ui_status_actions actions;
    mesh_ui_status_actions(&actions, connected != NULL, snapshot->handshake_valid);
    const uint32_t focus = snapshot->nav.cursor[MESH_UI_SCREEN_STATUS];

    /*
     * Elevated, always. It is the first question the screen answers - is there a radio - and
     * every number on the two cards below it is about a link this one says whether we have; a
     * column of equal weights was the audit's complaint about this screen.
     */
    fb_card_begin(&card, FB_CARD_ELEVATED, MESH_UI_ICON_LINK, MESH_STR_STATUS_CARD_LINK,
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
        /* A running sync says how far along it is rather than only that it is running: the
           replay is most of the wait, and on a link that keeps dropping mid-roster the count is
           the difference between "this is working" and "this has stalled again". It needs the
           radio's own total to count against, so a sync that has not reached MyNodeInfo yet
           still says just "in progress". */
        if (hs->request_in_flight && !hs->config_complete && hs->has_my_info &&
            hs->my_info.nodedb_entries > 0U) {
            fb_card_row(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_SYNC,
                        MESH_STR_STATUS_SYNC_PROGRESS, hs->sync_nodes, hs->my_info.nodedb_entries);
        } else {
            fb_card_row(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_SYNC,
                        MESH_STR_STATUS_SYNC_VALUE,
                        mesh_str(hs->config_complete     ? MESH_STR_STATUS_SYNC_COMPLETE
                                 : hs->request_in_flight ? MESH_STR_STATUS_SYNC_IN_PROGRESS
                                                         : MESH_STR_STATUS_SYNC_IDLE),
                        hs->cached ? mesh_str(MESH_STR_STATUS_SYNC_CACHED) : "");
        }
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
    fb_status_card_actions(&card, &actions, MESH_UI_STATUS_CARD_LINK, focus);
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

    /* Filled: the ordinary weight, and the middle of the three. The mesh is the subject of the
       screen once there is a link, but it is never the thing to read first. */
    fb_card_begin(&card, FB_CARD_FILLED, MESH_UI_ICON_NODES, MESH_STR_STATUS_CARD_MESH,
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
            /*
             * And the same number again, over the reports before this one: whether the mesh is
             * getting busier.
             *
             * The third row about one figure, and the one the other two cannot cover between
             * them. A percentage says how busy, a bar says whether that is a lot, and neither
             * distinguishes a mesh at 31% and settling from a mesh at 31% on its way past 50 -
             * which is the difference between waiting and moving. On the same domain and drawn
             * directly under the bar, so the line's height and the fill's length are one scale
             * read twice.
             *
             * It costs a row and only ever when there is a trend to draw: the radio has to have
             * reported twice, which on a fresh link is a few minutes in. A card that reserved
             * the row against a second report would spend it saying nothing on exactly the
             * screen that has least room to spare.
             */
            fb_card_spark(&card, air_tone, MESH_STR_NONE, &snapshot->history.channel_utilization,
                          (struct mesh_ui_scale){0, 0});
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
    /*
     * The one card whose weight is a reading rather than a decision.
     *
     * Every row on it appears only when the radio is in some kind of trouble, so on a healthy
     * link it is a heading over a battery figure and nothing else - and a quiet card should
     * recede rather than spend a panel's worth of fill saying nothing. It is outlined there,
     * and lifts to the raised tier the moment the tone above says it has something to report,
     * which is the fact the heading colour was already carrying alone.
     */
    fb_card_begin(&card, radio_tone == MESH_UI_TONE_PRIMARY ? FB_CARD_OUTLINED : FB_CARD_ELEVATED,
                  MESH_UI_ICON_RADIO, MESH_STR_STATUS_CARD_RADIO, radio_tone);
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
    /*
     * A radio that has told us nothing about itself, said out loud.
     *
     * Every row above appears only when something is worth reporting, so a link that has just
     * come up and a radio with no battery sensor both leave this card with no rows at all - and
     * a card with no rows is not drawn. That was fine while the card was a readout. It is not
     * fine now that it carries a verb: the action bar would be naming a press whose button is
     * not on screen, and the cursor would step onto nothing. The Mesh card already had this row
     * for the same reason its counters can be missing; this is the same sentence for the same
     * situation, with its own id because it is read somewhere else.
     */
    if (fb_card_is_empty(&card) && snapshot->handshake_valid) {
        fb_card_row_text(&card, MESH_UI_TONE_DIM, MESH_STR_STATUS_LABEL_RADIO_SELF,
                         mesh_str(MESH_STR_STATUS_RADIO_NO_REPORT));
    }
    fb_status_card_actions(&card, &actions, MESH_UI_STATUS_CARD_RADIO, focus);
    (void)fb_draw_card(state, layout, &y, &card);
}

/* "Save <section>?" for the sections whose write can cut this client off, and "Reboot the
   radio?" and its siblings for the Radio actions section. Which of the two it is standing in
   front of is nav->confirm_action; all three strings come from settings.c. */
/*
 * Help: what the screen underneath is for, and what its rows mean.
 *
 * A list of paragraphs rather than a dialog, because there is nothing here to answer - and a
 * list rather than a card for the same reason: a card note stops at three lines, and these are
 * the only content on the frame. Which paragraphs there are is src/ui/help.c's answer, read
 * here and by the action bar and the key handler alike.
 *
 * The heights are measured before the list opens, which on this screen is not a formality: a
 * note's height is a property of its *words* rather than of its kind, so this is the one list in
 * the client where the screen genuinely cannot guess and the model genuinely has to be told.
 */
static void fb_render_help(struct mesh_ui_backend_fb_state *state,
                           const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    struct mesh_ui_help_topic topic;
    if (!mesh_ui_help_topic(&snapshot->settings,
                            snapshot->handshake_valid ? &snapshot->handshake : NULL, nav, &topic)) {
        /* Reachable only if the section emptied under an open help screen - a disconnect
           between the press and this frame. Saying so beats drawing an empty list. */
        fb_draw_empty(state, layout, MESH_UI_ICON_ABOUT, mesh_str(MESH_STR_HELP_TITLE));
        return;
    }

    /*
     * The screen it explains, on the trail above the title.
     *
     * This is the one place a trail earns a level the navigation bar is not already carrying:
     * the strip says "Settings" and this screen's own title says "Help", so without the section
     * name between them the frame never says *what* is being explained.
     */
    struct fb_app_bar bar = {.title = mesh_str(MESH_STR_HELP_TITLE)};
    bar.trail[bar.trail_count++] =
        mesh_ui_settings_section_name((enum mesh_ui_settings_section)nav->settings_section);
    fb_draw_app_bar(state, layout, &bar);

    const char *headings[MESH_UI_HELP_ENTRIES_MAX];
    const char *bodies[MESH_UI_HELP_ENTRIES_MAX];
    uint8_t heights[MESH_UI_HELP_ENTRIES_MAX];
    for (uint32_t i = 0; i < topic.count; ++i) {
        /* The opening paragraph is about the whole screen and names no row, so it gets the one
           heading this screen writes rather than a field's label. */
        headings[i] = topic.entries[i].label != MESH_STR_NONE ? mesh_str(topic.entries[i].label)
                                                              : mesh_str(MESH_STR_HELP_OVERVIEW);
        bodies[i] = mesh_str(topic.entries[i].body);
        const uint32_t steps = fb_list_note_steps(state, headings[i], bodies[i]);
        heights[i] = steps > UINT8_MAX ? UINT8_MAX : (uint8_t)steps;
    }

    struct fb_list list = fb_list_begin_heights(layout, topic.count, nav->help_cursor, heights);
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        fb_list_note(state, &list, i, headings[i], bodies[i]);
    }
}

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
/*
 * Whether a settings row says its value with a slider, and where the handle goes if it does.
 *
 * One function, asked twice - once to measure the row's height and once to draw it - for the
 * reason fb_trailing_cols() is one function: a control whose presence was decided by one piece
 * of code and whose room was reserved by another is a control drawn over the row beneath it.
 * Here the two answers are a step apart rather than a cell, which is the more visible half of
 * the same bug.
 *
 * The decision itself is not this screen's. Whether a field's numbers measure something or name
 * something is a fact about the field, stated in its own table entry, and the arithmetic that
 * places a value among the presets is unit-tested in the settings model - so this is a call, not
 * a rule. What the backend decides is only that a scale is worth a length, which is the same
 * choice it makes when a boolean gets a switch instead of the word "On".
 */
static bool settings_row_slider(const struct mesh_ui_settings_item *item,
                                struct mesh_ui_settings_track *out) {
    if (item->kind != MESH_UI_SETTING_NUMBER || item->field == MESH_UI_FIELD_NONE) {
        return false;
    }
    return mesh_ui_settings_number_track(item->field, item->number, out);
}

static void fb_render_settings(struct mesh_ui_backend_fb_state *state,
                               const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_settings *settings = &snapshot->settings;
    const struct mesh_ui_handshake_state *handshake =
        snapshot->handshake_valid ? &snapshot->handshake : NULL;
    const bool section_open = (nav->settings_section != MESH_UI_SETTINGS_NO_SECTION);
    const enum mesh_ui_settings_section section =
        (enum mesh_ui_settings_section)nav->settings_section;

    /*
     * The breadcrumb, as slots rather than as a sentence.
     *
     * It used to be one catalog entry - "Settings > %s%s%s" - with "Modules > " for the third
     * level and " (unsaved)" arriving as the trailing %s. Three things were wrong with that and
     * the app bar answers all three: a translator was handed the trail's grammar along with its
     * words, a count glued in with %s cannot be a badge, and at the title's glyph scale
     * "Settings > Modules > Telemetry" is thirty of the thirty-four cells on the line, so the
     * leaf - the only part naming *this* screen - was the half that got elided.
     *
     * What is left here is a level per slot. The separators are the component's, and it draws
     * them as chevrons.
     */
    struct fb_app_bar bar = {.title = mesh_str(MESH_STR_SETTINGS_TITLE)};
    char title[96];
    char unsaved[32];
    if (section_open) {
        /*
         * The levels *between* the tab and this screen, which for most sections is none: the
         * navigation bar is already saying "Settings", selected, three rows above, and a trail
         * that repeated it would spend a body row on a word the frame already carries. What is
         * left is the one level nothing else says - "Modules" over a module's own section, and
         * "Channels" over one channel - which is exactly where the breadcrumb was earning its
         * keep and nowhere else.
         */
        if (nav->settings_channel != MESH_UI_SETTINGS_NO_CHANNEL) {
            /* One channel out of the Channels list: the list is the level above it, and unlike
               Modules it is not in settings_parent - a channel is identified by its number
               rather than by a section of its own. */
            bar.trail[bar.trail_count++] = mesh_ui_settings_section_name(MESH_UI_SETTINGS_CHANNELS);
            mesh_str_format(title, sizeof title, MESH_STR_SETTINGS_TITLE_CHANNEL,
                            (unsigned)nav->settings_channel);
            bar.title = title;
        } else {
            if (nav->settings_parent != MESH_UI_SETTINGS_NO_SECTION) {
                bar.trail[bar.trail_count++] = mesh_ui_settings_section_name(
                    (enum mesh_ui_settings_section)nav->settings_parent);
            }
            bar.title = mesh_ui_settings_section_name(section);
        }
        /*
         * How many rows are edited and not yet written, in the slot that is about the screen
         * rather than about a row. The warning family because that is what it is: the radio
         * does not know about these yet, and leaving the section is what loses them.
         */
        if (nav->settings_edit_count > 0U) {
            mesh_str_format(unsaved, sizeof unsaved, MESH_STR_SETTINGS_UNSAVED,
                            (unsigned)nav->settings_edit_count);
            bar.badge = unsaved;
            bar.badge_family = MESH_UI_FAMILY_WARNING;
        }
    }
    fb_draw_app_bar(state, layout, &bar);

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

    /*
     * The section's rows, all of them, before anything is placed.
     *
     * Built once rather than asked for row by row, which is what step 9's list model requires of
     * any screen whose rows are not all one height: the window, the highlight and the scroll
     * thumb are three sums of the heights, and the model has to be handed them before it decides
     * which rows are on screen. The node detail has had this shape since that step; the settings
     * screen only needed it once a row could be two steps tall.
     *
     * It is also strictly cheaper than what it replaced. mesh_ui_settings_item() rebuilds the
     * whole section from the radio's config for every row it answers, so the loop below used to
     * build it once per visible row.
     */
    struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
    const uint32_t count =
        section_open
            ? mesh_ui_settings_items(settings, handshake, nav->settings_edits,
                                     nav->settings_edit_count, section, nav->settings_channel,
                                     items, MESH_UI_SETTINGS_ITEMS_MAX)
            : mesh_ui_settings_root_count();
    if (count == 0U) {
        fb_draw_empty(state, layout, MESH_UI_ICON_SETTINGS,
                      mesh_str(MESH_STR_SETTINGS_EMPTY_SECTION));
        return;
    }

    /*
     * The leading slot is declared for the whole list or not at all - a list that indents only
     * the rows with something in it is a list the eye cannot run down - so it is decided here,
     * once, rather than per row. Two lists here are lists of *subjects*: the section list, and
     * Modules, which is one wearing a section's clothes. Everything else is settings, and a
     * setting's row already says what it is in its label column.
     */
    const bool rows_lead_with_icon = !section_open || mesh_ui_settings_section_icons_rows(section);

    /* Label column: a fixed width so values line up, capped for narrow scales. */
    const size_t label_cols = fb_field_label_cols(state, layout, 0U);
    /*
     * Which rows carry a slider, measured here and handed to the model before the first row is
     * placed - the node detail's arrangement, and the rule step 9 left behind: the screen
     * measures, because the screen is the only thing that knows whether a row carries a bar, and
     * from there the model is the authority on every height.
     *
     * A field's answer does not depend on its current value, deliberately. The slider is dropped
     * for a field whose numbers name something rather than measure it, and that is a fact about
     * the field; if it depended on the value, stepping a row would change the height of the row
     * the cursor is sitting on.
     */
    uint8_t heights[MESH_UI_SETTINGS_ITEMS_MAX];
    for (uint32_t r = 0; r < count; ++r) {
        heights[r] = (section_open && settings_row_slider(&items[r], NULL)) ? 2U : 1U;
    }
    struct fb_list list =
        fb_list_begin_heights(layout, count, nav->cursor[MESH_UI_SCREEN_SETTINGS], heights);
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        if (section_open) {
            const struct mesh_ui_settings_item item = items[i];
            /* A heading names the group below it: dimmed, no marker, and no value column -
               the same row the node detail draws, so the two screens stay identical. */
            if (item.kind == MESH_UI_SETTING_HEADING) {
                fb_list_subheader(state, &list, i, item.label);
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
            /* Empty on every list but Modules, and reserved on all of that one's rows - which
               is what the kind means, and why it is set from the list's answer rather than
               from whether this particular row filled it. */
            const struct fb_leading leading =
                rows_lead_with_icon
                    ? (struct fb_leading){.kind = FB_LEADING_ICON, .icon = item.icon}
                    : (struct fb_leading){.kind = FB_LEADING_NONE};
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
                    .leading = leading,
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
                    .leading = leading,
                    .label = item.label,
                    .label_cols = label_cols,
                    .marker_icon = marker,
                    .tone = tone,
                    .trailing = {.kind = FB_TRAILING_SWITCH, .sw = &sw},
                };
                fb_list_item(state, &list, i, &row);
                continue;
            }
            /*
             * A number on a scale gets the scale drawn under it.
             *
             * The same choice again, one kind further along: the item already says what the
             * value is - "5m", and the CLI backend draws exactly that and nothing else - and
             * this is the fb backend adding what the word cannot carry, which is where 5m falls
             * among the durations this field will accept. A row of intervals used to be a
             * column of figures that could only be compared against each other by reading all
             * of them.
             *
             * It costs the row its second step, and that is the deal §1.4 struck: a bar with the
             * row to itself is the one that can be aimed at, and the trailing slot's eight cells
             * cannot carry a dozen stops. A control the reader is about to change is exactly the
             * kind of thing that earns a step, where a figure the eye passes does not.
             *
             * Keyed on the field, with the channel mixed in for the rows the Channels section
             * repeats per slot - the switch's identity, because this is a control on a field in
             * the same way, and 0x05 keeps it clear of everything the switch and the meters use.
             */
            struct mesh_ui_settings_track track;
            if (settings_row_slider(&item, &track)) {
                struct fb_slider slider = {
                    .id = 0x05000000U | ((uint32_t)nav->settings_channel << 16) |
                          (uint32_t)item.field,
                    .position = track.position,
                    .stops = track.stops,
                    .unplaced = track.unplaced,
                    .tone = MESH_UI_TONE_PRIMARY,
                };
                const struct fb_list_item row = {
                    .leading = leading,
                    .label = item.label,
                    .label_cols = label_cols,
                    .marker_icon = marker,
                    /* The figure stays. The track says how far along, the word says how long,
                       and neither is the other's caption - a slider with no reading is a
                       control that cannot be set to a value anybody could name. */
                    .value = item.value,
                    .tone = tone,
                    .slider = &slider,
                };
                fb_list_item(state, &list, i, &row);
                continue;
            }
            /*
             * A small set of alternatives gets the whole set rather than the one word.
             *
             * On the same terms as a boolean getting a switch: the item already says what it is
             * and what it is set to, and this is the fb backend choosing how to say it on a
             * screen. The CLI backend still draws the word, and so does this one when the value
             * column is too narrow for the segments - which is what `value` on the struct is
             * for, and why the choice between the two is the component's rather than a test
             * written out here.
             *
             * Two to four, because five equal shares of a value column are five clipped words.
             * The larger enums - thirty-eight regions, seventeen presets - are stepped exactly
             * as they were; a set nobody can take in at a glance is better read one at a time.
             */
            const uint32_t choices =
                item.kind == MESH_UI_SETTING_ENUM ? mesh_ui_settings_enum_count(item.field) : 0U;
            if (choices >= 2U && choices <= FB_SEGMENTED_MAX) {
                struct fb_segmented segmented = {
                    .count = choices,
                    .active = item.number,
                    .value = item.value,
                };
                for (uint32_t c = 0U; c < choices; ++c) {
                    segmented.labels[c] = mesh_ui_settings_enum_name(item.field, c);
                }
                const struct fb_list_item row = {
                    .leading = leading,
                    .label = item.label,
                    .label_cols = label_cols,
                    .marker_icon = marker,
                    /* No value column: the set is the value, and the word for the chosen one is
                       inside the control that decides which of the two forms to draw. */
                    .tone = tone,
                    .trailing = {.kind = FB_TRAILING_SEGMENTED, .segmented = &segmented},
                };
                fb_list_item(state, &list, i, &row);
                continue;
            }
            const struct fb_list_item row = {
                .leading = leading,
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
                /* What the section is, in the slot the eye reaches first. The one list on this
                   screen that was a column of words with nothing to aim at. */
                .leading = {.kind = FB_LEADING_ICON,
                            .icon = mesh_ui_settings_section_icon(section_row)},
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

/* The same snapshot and geometry can only move inside the bounds declared by animated
   widgets. Re-run composition through a clip so overlapping chrome is restored in draw order. */
struct fb_render_cache {
    struct mesh_ui_snapshot snapshot;
    const struct mesh_ui_theme *theme;
    const struct mesh_i18n_locale *locale;
    int scale;
    uint32_t width, height;
    time_t second;
    bool valid;
};

void fb_render_cache_free(struct mesh_ui_backend_fb_state *state) {
    free(state->render_cache);
    state->render_cache = NULL;
}

void fb_animation_damage(struct mesh_ui_backend_fb_state *state, int x, int y, int w, int h) {
    struct fb_damage_rect *r = &state->animation_damage;
    if (w <= 0 || h <= 0)
        return;
    if (!r->valid) {
        *r =
            (struct fb_damage_rect){.x = x, .y = y, .right = x + w, .bottom = y + h, .valid = true};
    } else {
        if (x < r->x)
            r->x = x;
        if (y < r->y)
            r->y = y;
        if (x + w > r->right)
            r->right = x + w;
        if (y + h > r->bottom)
            r->bottom = y + h;
    }
}

static void fb_render_begin(struct mesh_ui_backend_fb_state *state,
                            const struct mesh_ui_snapshot *snapshot) {
    state->clip_active = false;
    if (!state->partial_disabled && state->render_cache == NULL) {
        state->render_cache = calloc(1U, sizeof *state->render_cache);
    }
    struct fb_render_cache *cache = state->render_cache;
    if (!state->partial_disabled && cache != NULL) {
        const time_t second = (time_t)mesh_time_wall_s();
        cache->snapshot.update_flags = snapshot->update_flags;
        state->clip_active = cache->valid && state->animation_damage.valid &&
                             cache->theme == state->theme && cache->locale == mesh_i18n_locale() &&
                             cache->scale == state->scale && cache->width == state->var.xres &&
                             cache->height == state->var.yres && cache->second == second &&
                             memcmp(&cache->snapshot, snapshot, sizeof *snapshot) == 0;
        state->clip = state->animation_damage;
        cache->snapshot = *snapshot;
        cache->theme = state->theme;
        cache->locale = mesh_i18n_locale();
        cache->scale = state->scale;
        cache->width = state->var.xres;
        cache->height = state->var.yres;
        cache->second = second;
        cache->valid = true;
    }
    state->animation_damage.valid = false;
}

void fb_render_snapshot(struct mesh_ui_backend_fb_state *state,
                        const struct mesh_ui_snapshot *snapshot) {
    /* Before anything is measured: a theme carries the glyph scale and the margin the whole
       frame is laid out against, so adopting one mid-frame would draw half of each. */
    (void)fb_state_follow_snapshot(state, snapshot);
    fb_render_begin(state, snapshot);

    fb_clear(state, fb_color(state, MESH_UI_COLOR_BG));

    struct fb_layout layout;
    memset(&layout, 0, sizeof layout);
    layout.small = mesh_ui_theme_type_scale(state->theme, MESH_UI_TYPE_LABEL, state->scale);
    layout.line = fb_line_adv(state, state->scale);
    layout.cols = fb_cols(state, state->scale);

    fb_draw_nav_bar(state, &layout, fb_tab_chips(), MESH_UI_SCREEN_COUNT,
                    (size_t)snapshot->nav.screen);

    /*
     * The two things the *client* says about itself, rather than what any screen says about
     * itself. Both are drawn here, once, around whichever screen is up - and both take their
     * content from src/ui/chrome.c for the reason the action bar takes its verbs from
     * src/ui/actions.c: which states are worth a notice is a fact about the client, not about
     * a framebuffer, and it is a unit test's business rather than a screenshot's.
     *
     * The bar goes first and costs nothing: it hangs in the gap the navigation bar already
     * leaves, so `layout` is unchanged by it and a request going out never reflows a list.
     */
    fb_draw_progress(state, &layout, mesh_ui_chrome_busy(snapshot));

    layout.footer_y = (int)state->var.yres - fb_action_bar_height(state, &layout);
    const int body_height = layout.footer_y - layout.body_y - fb_gutter(state);
    layout.rows = body_height > 0 ? (uint32_t)(body_height / layout.line) : 0U;

    /*
     * The banner does cost rows, so it is drawn after the body has been measured and hands back
     * what is left - exactly as the top app bar under it does. Nothing below this line knows it
     * happened, which is the whole point: a screen renderer lays out against `layout`.
     */
    struct mesh_ui_banner banner;
    if (mesh_ui_chrome_banner(snapshot, &banner)) {
        const struct fb_banner drawn = {
            .icon = banner.icon,
            .text = mesh_str(banner.text),
            .supporting = banner.supporting != MESH_STR_NONE ? mesh_str(banner.supporting) : NULL,
            .detail = banner.detail,
            .family = banner.family,
        };
        fb_draw_banner(state, &layout, &drawn);
    }

    /*
     * Which buttons mean something here is no longer decided in this function.
     *
     * It used to be a branch per screen alongside the one below - the same conditions written
     * out twice, once to pick a renderer and once to pick a hint sentence, which is two places
     * to remember when a screen grows a press. mesh_ui_actions_for() answers from the snapshot
     * instead, walking the same chain of overlays, and it is a unit test's business rather than
     * a screenshot's.
     *
     * It is asked *before* the screen draws because both ends of the frame read it now: the
     * action bar at the bottom says what B does, and the top app bar's leading slot draws an
     * arrow when what B does is leave. Two answers from one table is the whole point - a screen
     * deciding its own back arrow would be free to disagree with the keycap under it.
     */
    struct mesh_ui_action_bar actions;
    mesh_ui_actions_for(snapshot, &actions);
    layout.back = mesh_ui_action_bar_goes_back(&actions);

    /*
     * And whether this frame is part of a move between two places, which is the one thing about
     * it that is not a function of the snapshot - see fb_transition_offset().
     *
     * The band is everything below the navigation bar and above the action bar, because those
     * two are the same on both sides of any move: the strip still names the tab it named, the
     * keycaps still say what the buttons do. What is inside the band and drawn *before* the
     * transform - the screen progress bar and the banner - stays put for the same reason, being
     * about the client rather than about the screen that is arriving.
     *
     * It is also declared as animation damage, so the frame after this one repaints the whole
     * band. Everything in there is a function of the clock while a move is running, and the
     * partial-redraw path assumes the opposite of anything it has not been told about.
     */
    const int slide = fb_transition_offset(state, &snapshot->nav);
    if (slide != 0) {
        fb_animation_damage(state, 0, layout.nav_y, (int)state->var.xres,
                            layout.footer_y - layout.nav_y);
        fb_shift_begin(state, slide, layout.nav_y, layout.footer_y);
    }

    /*
     * One tail for every path through this function, which is what lets the chrome below it be
     * written once. The overlays used to draw the footer and return, and each of the four
     * carried its own copy of that call - so anything drawn over the whole frame (the notice
     * below is the first) had to be added in five places or be missing from four screens.
     */
    if (snapshot->nav.help_open) {
        fb_render_help(state, snapshot, &layout);
    } else if (snapshot->nav.confirm_open) {
        fb_render_confirm(state, snapshot, &layout);
    } else if (snapshot->nav.picker_open) {
        fb_render_picker(state, snapshot, &layout);
    } else if (snapshot->nav.keyboard_open) {
        fb_render_keyboard(state, snapshot, &layout);
    } else if (snapshot->nav.compose_open) {
        fb_render_compose(state, snapshot, &layout);
    } else if (snapshot->nav.reaction_open) {
        fb_render_reactions(state, snapshot, &layout);
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
        case MESH_UI_SCREEN_WAYPOINTS:
            fb_render_waypoints(state, snapshot, &layout);
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
    fb_shift_end(state);

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
