#define _POSIX_C_SOURCE 200809L

/*
 * The Messages tab: the conversations, and one of them opened as a transcript.
 *
 * Level one is a column of conversation cells - all traffic, the channels, whoever we have
 * direct messages with. Level two is the shape every messenger has: theirs on the left, ours on
 * the right, the newest against the bottom.
 *
 * Most of this file is neither, and that is the point of it having one: a bubble is expensive to
 * word and to measure, so the rows a thread draws are derived once and cached against the
 * messages they were built from. Nothing here computes a pixel - inkcell_fb_bubble_rows() says how
 * tall a message is and inkcell_transcript_window() says which of them are on screen.
 */

#include "inkcell/ui/emoji.h"
#include "inkcell/ui/layout.h"
#include "inkcell/ui/widgets.h"
#include "inkcell/utils/text.h"
#include "inkcell/utils/time.h"

#include "fb_screens_internal.h"

#include "mesh/core/message.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/delivery.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/reactions.h"
#include "mesh/ui/trust.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Level one of the Messages tab: all traffic, the channels, whoever we have direct messages
   with, and the way to start a new one. One conversation cell a row - see inkcell/ui/widgets.h. */
void fb_render_conversations(struct inkcell_backend_fb_state *state,
                             const struct mesh_ui_snapshot *snapshot,
                             struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    struct mesh_ui_store view;
    mesh_ui_store_view(snapshot, &view);

    const uint32_t count = mesh_ui_nav_conversation_count(&view);
    char title[96];
    inkcell_fb_title_count(title, sizeof title, inkcell_str(MESH_STR_TAB_MESSAGES), count,
                           snapshot->messages.dropped);
    inkcell_fb_draw_app_bar(state, layout, &(const struct inkcell_fb_app_bar){.title = title});
    if (count == 0U) {
        inkcell_fb_draw_empty(state, layout, INKCELL_ICON_MESSAGES,
                              inkcell_str(MESH_STR_MESSAGES_EMPTY));
        return;
    }

    /*
     * Each conversation is one cell two body rows tall: the avatar, the name and the age, then
     * the last thing said with the unread count as a pill. The cell owns every pixel of that -
     * this loop only says which strings go in it and what each one means.
     */
    struct inkcell_fb_list list =
        inkcell_fb_list_begin_rows(layout, count, nav->cursor[MESH_UI_SCREEN_MESSAGES], 2U);
    inkcell_fb_list_glide(state, &list, FB_LIST_CONVERSATIONS);
    char age[8];
    char badge[8];
    uint32_t i;
    while (inkcell_fb_list_next(&list, &i)) {
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
            inkcell_fb_format_age(conversation.last_time, age, sizeof age);
        }
        badge[0] = '\0';
        if (conversation.unread > 0U) {
            /* Past two figures a badge stops being a number and becomes a width, which is what
               every messenger's "99+" is for. */
            if (conversation.unread > 99U) {
                snprintf(badge, sizeof badge, "%s", inkcell_str(MESH_STR_MESSAGES_UNREAD_OVERFLOW));
            } else {
                snprintf(badge, sizeof badge, "%u", (unsigned)conversation.unread);
            }
        }

        const struct inkcell_fb_conversation cell = {
            .avatar = conversation.initials,
            /*
             * The three rows that are not a person say so with a symbol instead of initials.
             * Which symbol is this backend's business, not the store's: the store publishes
             * what a row *is* (its kind) and the CLI backend still draws the "#" and "+" it
             * always did.
             */
            .avatar_icon =
                is_new                                                ? INKCELL_ICON_COMPOSE
                : (conversation.kind == MESH_UI_CONVERSATION_ALL)     ? INKCELL_ICON_BROADCAST
                : (conversation.kind == MESH_UI_CONVERSATION_CHANNEL) ? INKCELL_ICON_CHANNEL
                                                                      : INKCELL_ICON_NONE,
            .tint = conversation.tint,
            /* The two rows that are not somebody: a view over the others, and a button. */
            .accent = is_view,
            .name = conversation.name,
            .age = age,
            /* The one row that is a button rather than a conversation says what it does
               instead of what was last said in it. */
            .preview = is_new ? inkcell_str(MESH_STR_MESSAGES_NEW_PREVIEW) : conversation.preview,
            .preview_outbound = conversation.preview_outbound,
            .badge = badge,
            .unread = (conversation.unread > 0U),
            .muted = conversation.muted,
            .armed = mesh_ui_nav_conversation_is_armed(nav, &conversation),
            /* All traffic is accented because it is a view rather than somebody; a channel
               used to be too, and no longer needs to be now that its avatar carries the tag.
               That frees the strong tone to mean what it means everywhere else on this
               screen: there is something here you have not read. */
            .name_tone = is_new                                              ? INKCELL_TONE_DIM
                         : (conversation.kind == MESH_UI_CONVERSATION_ALL)   ? INKCELL_TONE_PRIMARY
                         : (conversation.unread > 0U && !conversation.muted) ? INKCELL_TONE_STRONG
                                                                             : INKCELL_TONE_NORMAL,
        };
        inkcell_fb_draw_conversation(state, &list, i, &cell);
    }
}

/*
 * Level two: the open conversation, drawn as a transcript of bubbles.
 *
 * The shape is the one every messenger has: theirs on the left, ours on the right, the newest
 * against the bottom, the sender said once per run rather than once per line, and the clock
 * tucked into the message it belongs to. It replaced a list of clipped one-line rows with a
 * detail pane underneath - which meant the only way to read a message in full was to select it,
 * and reading the one before it meant losing the one you had.
 *
 * Nothing here computes a pixel: inkcell_fb_bubble_rows() says how tall a message is and
 * inkcell_transcript_window() says which of them are on screen.
 */

/* A message as the screen describes it, with the strings the bubble points at.
 *
 * One buffer per slot rather than one buffer per line: the bubble's trailing run is four typed
 * parts it measures itself (struct inkcell_fb_bubble_meta), and the screen's job is to fill the
 * slots rather than to assemble a line out of them. Concatenating them here is what used to let a
 * failure reason push the run past the bubble's own width. */
struct fb_thread_row {
    struct inkcell_fb_bubble bubble;
    char separator[24];
    char name[48];
    char clock[8];
    char reactions[40];
    char relay[24]; /* "via BOB" - the relay chip, sized for the longest short name plus it */
    char note[64];
    char quote[64]; /* the message this one replies to; the bubble elides it to one line */
};

/* Both first-visible and ordinary variants are derived from exact message inputs. Cursor
   movement only chooses between them; it never reformats or remeasures the transcript. */
struct inkcell_fb_thread_cache {
    bool valid;
    /*
     * The messages the cached rows were built from, *after* the conversation filter - `count`
     * of them, in the order they are drawn.
     *
     * The whole flat log used to be the key, which was exact but too wide: a message arriving
     * on a channel the reader is not looking at changed it, so every bubble in the open thread
     * was reformatted and remeasured for traffic that could not possibly have altered one. It
     * is also no longer the right list, because a thread is drawn from the deep window when
     * there is one - and the filtered run is what both of those have in common.
     */
    struct mesh_ui_message entries[MESH_UI_MAX_THREAD_MESSAGES];
    uint32_t count;
    bool inbox;
    uint32_t target_node;
    /* Part of the key, because it decides which row carries the unread line and it can move
       without a single message doing so: leaving a conversation and coming straight back is the
       same log, the same indices and the same target with the line in a different place. */
    uint32_t unread_from;
    const struct inkcell_theme *theme;
    const struct inkcell_i18n_locale *locale;
    int scale;
    size_t cols;
    char calendar[80];
    struct fb_thread_row rows[2][MESH_UI_MAX_THREAD_MESSAGES];
    uint8_t heights[2][MESH_UI_MAX_THREAD_MESSAGES];
};

void fb_thread_cache_free(struct inkcell_backend_fb_state *state) {
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
    const time_t now = (time_t)inkcell_time_wall_s();
    struct tm today;
    if (now > 0 && localtime_r(&now, &today) != NULL) {
        if (when.tm_year == today.tm_year && when.tm_yday == today.tm_yday) {
            snprintf(out, out_len, "%s", inkcell_str(MESH_STR_DATE_TODAY));
            return;
        }
        if (when.tm_year == today.tm_year && when.tm_yday + 1 == today.tm_yday) {
            snprintf(out, out_len, "%s", inkcell_str(MESH_STR_DATE_YESTERDAY));
            return;
        }
    }
    /* "%e" pads a single-digit day with a space, which reads as a typo in a centred label. */
    static const enum inkcell_str_id kMonths[] = {
        MESH_STR_DATE_JAN, MESH_STR_DATE_FEB, MESH_STR_DATE_MAR, MESH_STR_DATE_APR,
        MESH_STR_DATE_MAY, MESH_STR_DATE_JUN, MESH_STR_DATE_JUL, MESH_STR_DATE_AUG,
        MESH_STR_DATE_SEP, MESH_STR_DATE_OCT, MESH_STR_DATE_NOV, MESH_STR_DATE_DEC,
    };
    static const enum inkcell_str_id kWeekdays[] = {
        MESH_STR_DATE_SUN, MESH_STR_DATE_MON, MESH_STR_DATE_TUE, MESH_STR_DATE_WED,
        MESH_STR_DATE_THU, MESH_STR_DATE_FRI, MESH_STR_DATE_SAT,
    };
    const char *month = inkcell_str(kMonths[when.tm_mon]);
    const char *weekday = inkcell_str(kWeekdays[when.tm_wday]);
    inkcell_str_format(out, out_len, MESH_STR_DATE_WEEKDAY_DAY_MONTH, weekday, when.tm_mday, month);
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

static void fb_thread_reactions(struct mesh_ui_message_view messages, uint32_t packet_id, char *out,
                                size_t out_len) {
    out[0] = '\0';
    if (packet_id == 0U || messages.entries == NULL) {
        return;
    }

    struct {
        char glyph[8];
        uint16_t count;
    } seen[FB_THREAD_REACTION_KINDS];
    size_t kinds = 0U;

    /* The same messages the transcript is drawn from rather than the flat list, which for a
       conversation read off the card is the difference between a reaction on a message from
       this morning and one on a message from last week: the reaction and its target are
       archived together, and a lookup that only saw the radio's last 64 would find neither. */
    for (uint32_t i = 0; i < messages.count; ++i) {
        const struct mesh_ui_message *reaction = &messages.entries[i];
        if (!reaction->is_reaction || reaction->reply_id != packet_id ||
            reaction->text[0] == '\0') {
            continue;
        }
        char glyph[8];
        const size_t take = inkcell_text_cell_offset(reaction->text, 1U);
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
            inkcell_str_copy(seen[kinds].glyph, sizeof seen[kinds].glyph, glyph);
            seen[kinds].count = 0U;
            kinds++;
        }
        seen[slot].count++;
    }

    struct inkcell_line line;
    inkcell_line_reset(&line);
    for (size_t i = 0; i < kinds; ++i) {
        /* The count is left off a lone reaction: "\U0001F44D 1" reads as a score. */
        if (seen[i].count > 1U) {
            inkcell_line_printf(&line, "%s%s%u", i > 0U ? " " : "", seen[i].glyph,
                                (unsigned)seen[i].count);
        } else {
            inkcell_line_printf(&line, "%s%s", i > 0U ? " " : "", seen[i].glyph);
        }
    }
    inkcell_str_copy(out, out_len, inkcell_line_text(&line));
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
void fb_thread_quote(struct mesh_ui_message_view messages, uint32_t reply_id, char *out,
                     size_t out_len) {
    out[0] = '\0';
    if (reply_id == 0U || messages.entries == NULL) {
        return;
    }
    for (uint32_t i = messages.count; i > 0U; --i) {
        const struct mesh_ui_message *target = &messages.entries[i - 1U];
        if (target->packet_id != reply_id || target->is_reaction) {
            continue;
        }
        /* Sanitised rather than copied: this truncates, and inkcell_str_copy truncates by bytes -
           which on a message ending in an emoji would cut a character in half. */
        inkcell_text_sanitise_str(target->text, out, out_len);
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
static void fb_thread_row_build(const struct mesh_ui_snapshot *snapshot,
                                struct mesh_ui_message_view messages, const uint32_t *indices,
                                uint32_t position, bool force_name, struct fb_thread_row *row) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_message *message = &messages.entries[indices[position]];
    const struct mesh_ui_message *previous =
        position > 0U ? &messages.entries[indices[position - 1U]] : NULL;
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
    row->bubble.meta.relay = row->relay;
    row->bubble.meta.clock = row->clock;
    /* A reaction never reaches a bubble - the transcript filters it out - so anything here
       carrying a reply_id is a threaded reply, and the quote is what says so. */
    fb_thread_quote(messages, message->reply_id, row->quote, sizeof row->quote);

    /* A separator opens the transcript and marks every day boundary and every long silence, so
       "when was this" is answered by the shape of the screen rather than by reading timestamps. */
    if (previous == NULL || !fb_thread_same_day(previous->rx_time, message->rx_time)) {
        fb_format_day(message->rx_time, row->separator, sizeof row->separator);
    } else if (fb_thread_elapsed(previous->rx_time, message->rx_time) >= FB_THREAD_GAP_SECONDS) {
        inkcell_fb_format_clock(message->rx_time, row->separator, sizeof row->separator);
    }
    row->bubble.separator_tone = INKCELL_TONE_DIM;

    /*
     * And the line under where the reader stopped last time, which takes the slot from a date
     * when both want it - see struct inkcell_fb_bubble for why that is the right way round.
     *
     * "The first message after the marked one" is exactly "the message whose predecessor is the
     * marked one", which is why this is a comparison against `previous` rather than a search:
     * the transcript is in order, so the bubble that follows the last one read is the first one
     * that was not. It needs the mark the *press* captured (nav.thread_unread_from) and not the
     * store's own, which has already advanced to the newest message by the time a frame is
     * built - see the field.
     *
     * `previous` being non-NULL is doing a second job: a conversation whose every message is
     * new gets no line, because a divider hanging above the first bubble separates the
     * transcript from nothing.
     */
    if (nav->thread_unread_from != 0U && previous != NULL &&
        previous->packet_id == nav->thread_unread_from) {
        inkcell_str_copy(row->separator, sizeof row->separator,
                         inkcell_str(MESH_STR_THREAD_UNREAD_FROM_HERE));
        row->bubble.separator_tone = INKCELL_TONE_PRIMARY;
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
        const char *peer = message->peer_name[0] != '\0'
                               ? message->peer_name
                               : inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT);
        struct inkcell_line line;
        inkcell_line_reset(&line);
        if (outbound) {
            /* Ours in all-traffic still needs a destination: "sent" alone does not say to whom,
               and a broadcast and a DM look identical without it. */
            if (message->broadcast) {
                inkcell_line_printf(&line, "%s", inkcell_str(MESH_STR_BUBBLE_SENT));
            } else {
                inkcell_line_str(&line, MESH_STR_BUBBLE_SENT_TO, peer);
            }
        } else {
            inkcell_line_printf(&line, "%s", peer);
        }
        /* All-traffic is several conversations at once, so each bubble says which one it is. */
        if (nav->inbox) {
            if (message->broadcast) {
                inkcell_line_str(&line, MESH_STR_BUBBLE_CHANNEL, (unsigned)message->channel);
            } else {
                inkcell_line_printf(&line, "%s", inkcell_str(MESH_STR_BUBBLE_DIRECT));
            }
        }
        if (message->kind == (uint8_t)MESH_MESSAGE_KIND_ALERT) {
            inkcell_line_printf(&line, "%s", inkcell_str(MESH_STR_BUBBLE_ALERT));
        } else if (message->kind == (uint8_t)MESH_MESSAGE_KIND_DETECTION) {
            inkcell_line_printf(&line, "%s", inkcell_str(MESH_STR_BUBBLE_SENSOR));
        }
        inkcell_str_copy(row->name, sizeof row->name, inkcell_line_text(&line));
    }

    /*
     * The trailing run: when it arrived, whether it went out encrypted, and for ours what
     * became of it. Four slots the bubble measures for itself rather than a line assembled
     * here - see struct inkcell_fb_bubble_meta for why that distinction is the whole of it.
     */
    inkcell_fb_format_clock(message->rx_time, row->clock, sizeof row->clock);

    /*
     * And through whom, when that was somebody other than whoever sent it. The store resolves
     * the relay's name at publish and writes "" when there is nothing to say - the firmware
     * named no relay, or it named the sender - so the chip is present exactly when the message
     * reached us second-hand, which is the one thing a hop count in a heading cannot say about
     * an individual message.
     */
    if (message->relay_name[0] != '\0') {
        inkcell_str_format(row->relay, sizeof row->relay, MESH_STR_BUBBLE_RELAY,
                           message->relay_name);
    }

    /*
     * A padlock on a message the radio decrypted with our key pair rather than with a channel
     * PSK. It only means anything on a direct message, and it is worth saying there: on a
     * channel still using the default key every node on the mesh holds that key, so a DM that
     * did *not* go out PKI-encrypted was readable by all of them, and nothing else on the
     * screen distinguishes the two.
     */
    /*
     * And *which* padlock, which is the half the mark could not say until key trust existed: a
     * key the radio happened to hold and a key somebody proved is theirs are two different
     * claims, and the bubble used to make the stronger-looking one for both. The shield is the
     * verified case and the padlock the ordinary one - two shapes rather than two colours, for
     * the reason mesh/ui/trust.h gives.
     *
     * Read off the peer's roster record rather than off the message, because verification is a
     * fact about the key and not about the packet: a message sent last week to a node verified
     * this morning was encrypted to that same key, and drawing it as unverified would be the
     * transcript remembering a doubt that has since been settled.
     */
    if (message->pki_encrypted && !message->broadcast) {
        const struct mesh_ui_node_summary *const peer_node =
            snapshot->handshake_valid
                ? mesh_ui_node_detail_find(&snapshot->handshake, message->peer)
                : NULL;
        const enum mesh_ui_key_trust peer_trust = mesh_ui_key_trust_of(peer_node);
        /* A node the roster has lost reads as NONE, which has no mark at all - and the message
           in front of the user certainly was encrypted. The padlock is the honest floor there:
           it says what the packet did, and claims nothing about whose key it used. */
        row->bubble.meta.lock = peer_trust == MESH_UI_KEY_TRUST_VERIFIED
                                    ? mesh_ui_key_trust_icon(MESH_UI_KEY_TRUST_VERIFIED)
                                    : INKCELL_ICON_ENCRYPTED;
    }

    /* What became of one of ours, as src/ui/tables/delivery.c answers - the mark for the corner,
       and the word for the line below when there is nothing better to put there. Which one a state
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
        inkcell_str_copy(row->note, sizeof row->note,
                         message->ack_error != 0U
                             ? mesh_message_ack_error_to_string(message->ack_error)
                             : inkcell_str(delivery.word));
    }

    /* Reactions ride the trailing run rather than taking a row: they are an annotation on this
       bubble, and a row of their own is the bubble they were filtered out of being. */
    fb_thread_reactions(messages, message->packet_id, row->reactions, sizeof row->reactions);
}

/* A bubble's height, clamped into the byte the transcript window measures in. */
static uint8_t fb_thread_height(const struct inkcell_backend_fb_state *state,
                                const struct inkcell_fb_layout *layout,
                                const struct fb_thread_row *row) {
    const uint32_t rows = inkcell_fb_bubble_rows(state, layout, &row->bubble);
    return rows > 0xFFU ? 0xFFU : (uint8_t)rows;
}

static struct inkcell_fb_thread_cache *
fb_thread_cache_get(struct inkcell_backend_fb_state *state, const struct mesh_ui_snapshot *snapshot,
                    const struct inkcell_fb_layout *layout, struct mesh_ui_message_view messages,
                    const uint32_t *indices, uint32_t count) {
    if (state->thread_cache_disabled) {
        return NULL;
    }
    if (state->thread_cache == NULL) {
        state->thread_cache = calloc(1U, sizeof *state->thread_cache);
    }
    struct inkcell_fb_thread_cache *cache = state->thread_cache;
    if (cache == NULL) {
        return NULL;
    }
    /* Include local calendar and zone, so midnight and a timezone change invalidate labels. */
    char calendar[80] = {0};
    const time_t now = (time_t)inkcell_time_wall_s();
    struct tm local;
    if (localtime_r(&now, &local) != NULL) {
        (void)strftime(calendar, sizeof calendar, "%Y-%m-%d %Z %z", &local);
    }
    /* Gathered before the comparison because it is also what is kept: the filtered run is the
       key and the rows are built from the same entries a moment later. */
    bool same_entries = cache->valid && cache->count == count;
    for (uint32_t i = 0; same_entries && i < count; ++i) {
        same_entries = memcmp(&cache->entries[i], &messages.entries[indices[i]],
                              sizeof cache->entries[i]) == 0;
    }
    const bool changed = !same_entries || cache->inbox != snapshot->nav.inbox ||
                         cache->target_node != snapshot->nav.target_node ||
                         cache->unread_from != snapshot->nav.thread_unread_from ||
                         cache->theme != state->theme || cache->locale != inkcell_i18n_locale() ||
                         cache->scale != state->scale || cache->cols != layout->cols ||
                         strcmp(cache->calendar, calendar) != 0;
    if (changed) {
        for (uint32_t i = 0; i < count; ++i) {
            cache->entries[i] = messages.entries[indices[i]];
        }
        cache->count = count;
        cache->inbox = snapshot->nav.inbox;
        cache->target_node = snapshot->nav.target_node;
        cache->unread_from = snapshot->nav.thread_unread_from;
        cache->theme = state->theme;
        cache->locale = inkcell_i18n_locale();
        cache->scale = state->scale;
        cache->cols = layout->cols;
        memcpy(cache->calendar, calendar, sizeof calendar);
        for (unsigned variant = 0U; variant < 2U; ++variant) {
            for (uint32_t i = 0U; i < count; ++i) {
                fb_thread_row_build(snapshot, messages, indices, i, variant != 0U,
                                    &cache->rows[variant][i]);
                cache->heights[variant][i] =
                    fb_thread_height(state, layout, &cache->rows[variant][i]);
            }
        }
        cache->valid = true;
    }
    return cache;
}

static void fb_thread_row_get(const struct mesh_ui_snapshot *snapshot,
                              struct mesh_ui_message_view messages, const uint32_t *indices,
                              uint32_t position, bool force_name,
                              const struct inkcell_fb_thread_cache *cache,
                              struct fb_thread_row *row) {
    if (cache == NULL) {
        fb_thread_row_build(snapshot, messages, indices, position, force_name, row);
        return;
    }
    *row = cache->rows[force_name ? 1 : 0][position];
    /* Never retain pointers into a caller-owned snapshot, or into a copied row's strings. */
    row->bubble.text = messages.entries[indices[position]].text;
    row->bubble.separator = row->separator;
    row->bubble.name = row->name;
    row->bubble.note = row->note;
    row->bubble.quote = row->quote;
    row->bubble.meta.reactions = row->reactions;
    row->bubble.meta.relay = row->relay;
    row->bubble.meta.clock = row->clock;
}

void fb_render_thread(struct inkcell_backend_fb_state *state,
                      const struct mesh_ui_snapshot *snapshot, struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;

    const struct mesh_ui_message_view messages = mesh_ui_snapshot_message_view(snapshot);
    uint32_t indices[MESH_UI_MAX_THREAD_MESSAGES];
    const uint32_t count =
        mesh_ui_nav_filter_messages(nav, messages, indices, MESH_UI_MAX_THREAD_MESSAGES);

    char convo[MESH_UI_NAV_TARGET_NAME_MAX];
    mesh_ui_nav_conversation_name(nav, convo, sizeof convo);
    char title[96];
    /* The view's own count of what is behind the top of it, not the transport ring's: a
       conversation drawn from the card has the messages the ring evicted, and saying "+30 older"
       over thirty messages the reader can scroll to is the opposite of what the line is for. */
    if (nav->inbox) {
        inkcell_fb_title_count(title, sizeof title, convo, count, messages.dropped);
    } else if (messages.dropped > 0U) {
        inkcell_str_format(title, sizeof title, MESH_STR_THREAD_TITLE_OLDER, convo,
                           inkcell_str(nav->target_node == MESH_MESSAGE_BROADCAST_ADDR
                                           ? MESH_STR_THREAD_KIND_CHANNEL
                                           : MESH_STR_THREAD_KIND_DIRECT),
                           (unsigned)messages.dropped);
    } else {
        inkcell_str_format(title, sizeof title, MESH_STR_THREAD_TITLE, convo,
                           inkcell_str(nav->target_node == MESH_MESSAGE_BROADCAST_ADDR
                                           ? MESH_STR_THREAD_KIND_CHANNEL
                                           : MESH_STR_THREAD_KIND_DIRECT));
    }
    /* No overline. Which kind of conversation this is stays in the title, because a channel's
       name already starts with a '#' and every bubble under it is tagged - so a trail would be
       spending a body row of transcript to repeat what two other things on the frame say. */
    inkcell_fb_draw_app_bar(state, layout, &(const struct inkcell_fb_app_bar){.title = title});

    if (count == 0U) {
        inkcell_fb_draw_empty(
            state, layout, INKCELL_ICON_MESSAGES,
            inkcell_str(nav->inbox ? MESH_STR_THREAD_EMPTY_INBOX : MESH_STR_THREAD_EMPTY));
        return;
    }

    /* Measure every message, then let the transcript say which of them are on screen. Heights
       come from the same component that draws them, so the window can never be a row out. */
    const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_MESSAGES];
    uint8_t heights[MESH_UI_MAX_THREAD_MESSAGES];
    struct fb_thread_row row;
    struct inkcell_fb_thread_cache *cache =
        fb_thread_cache_get(state, snapshot, layout, messages, indices, count);
    for (uint32_t i = 0; i < count; ++i) {
        if (cache != NULL) {
            heights[i] = cache->heights[0][i];
        } else {
            fb_thread_row_build(snapshot, messages, indices, i, false, &row);
            heights[i] = fb_thread_height(state, layout, &row);
        }
    }
    struct inkcell_transcript window =
        inkcell_transcript_window(heights, count, cursor, layout->rows);

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
                fb_thread_row_build(snapshot, messages, indices, named, false, &row);
                heights[named] = fb_thread_height(state, layout, &row);
            }
        }
        named = window.first;
        if (cache != NULL) {
            heights[named] = cache->heights[1][named];
        } else {
            fb_thread_row_build(snapshot, messages, indices, named, true, &row);
            heights[named] = fb_thread_height(state, layout, &row);
        }
        window = inkcell_transcript_window(heights, count, cursor, layout->rows);
    }
    /* Only force what the heights were settled against, so the draw can never disagree with the
       measure even if the loop ran out of passes. */
    const bool settled = (named == window.first);

    int y = layout->body_y + (int)window.pad * layout->line;
    for (uint32_t i = window.first; i < window.first + window.count && i < count; ++i) {
        fb_thread_row_get(snapshot, messages, indices, i, settled && i == named, cache, &row);
        row.bubble.selected = (i == cursor);
        inkcell_fb_draw_bubble(state, layout, y, &row.bubble);
        y += (int)heights[i] * layout->line;
    }
}
