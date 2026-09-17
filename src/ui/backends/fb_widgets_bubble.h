#ifndef MESH_UI_BACKENDS_FB_WIDGETS_BUBBLE_H
#define MESH_UI_BACKENDS_FB_WIDGETS_BUBBLE_H

/*
 * The transcript's one component: a message as a bubble, and the separator that rules a line
 * between two of them.
 */

/*
 * Not public API. include/mesh/ui/backends/fb.h is; fb_widgets.h is the umbrella over this file
 * and its siblings, and nothing outside src/ui/backends/ should include either.
 */

#include "fb_internal.h"

#include "mesh/ui/icon.h"
#include "mesh/ui/theme.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * A bubble's trailing run: the small marks and figures that ride the end of its last line.
 *
 * A run rather than a string, and this is the fix for the one class of bug the transcript kept
 * producing. The clock, the delivery state, the padlock and the reactions were concatenated
 * into one `meta` string by the screen and measured by the bubble; a failure reason ("no public
 * key for that node") or a fourth reaction pushed that string past the bubble's own width, the
 * measure clamped the *bubble* to its maximum and the draw right-aligned the *string* inside
 * it, and the difference between the two came out of the left edge - text painted outside the
 * bubble it belonged to, and on an inbound one, off the panel.
 *
 * Typed parts cannot do that. The run knows its own cells, the bubble is sized around it, and
 * the parts that do not fit are dropped from the front - a reaction chip is worth losing, the
 * mark that says the message failed is not. It is the same correction fb_list_item's slots
 * made: a component measures what it is given, so it must be given the pieces rather than a
 * sentence somebody else assembled.
 *
 * Drawn left to right in the order below, which is the order every messenger puts them in and
 * the order they are worth losing in - annotations first, then the facts, with what became of
 * the message hard against the corner.
 */
struct fb_bubble_meta {
    const char *reactions; /* the reaction chip run: "\U0001F44D3 \U0001F602"; "" for none */
    /*
     * Who carried the packet the last stretch - "via BOB" - for a message that did not come
     * straight from the node that sent it. "" for one that did, and for a mesh whose firmware
     * does not fill the field in.
     *
     * In the run rather than on the bubble's header line, which is where the channel and alert
     * chips live, for two reasons. It is a fact about *this packet* and the header line is
     * about the sender, said once per run of theirs; and on a multi-hop mesh nearly every
     * message is relayed, so a header line each would be a transcript of alternating names.
     * Second to be dropped when the bubble is too narrow, after the reaction chips: the route
     * a message took is worth more than a tapback and less than when it arrived.
     */
    const char *relay;
    /*
     * The padlock on a direct message the radio decrypted with our key pair rather than with a
     * channel PSK.
     *
     * A part rather than a character in a text run because it is a *fact about the message*
     * rather than a word about it, and because a glyph the string catalog carried would be a
     * mark a translator could delete. MESH_UI_ICON_NONE for a message that is not one.
     */
    enum mesh_ui_icon lock;
    const char *clock; /* when it arrived, "14:05"; "" when the radio has no clock set */
    /* What became of one of ours: the clock, the double tick or the alert circle that
       src/ui/tables/delivery.c answers with. MESH_UI_ICON_NONE on anything inbound, and on a
       broadcast that went out without want_ack - there is nothing to be waiting for. */
    enum mesh_ui_icon state;
};

/*
 * A chat bubble: the component the thread screen is made of.
 *
 * A bubble sizes itself to its own text - never to the panel - and sits against the edge its
 * direction names, which is the whole of what makes a transcript readable at a glance without
 * reading a single word of it. Everything optional is omitted rather than blanked, so a run of
 * messages from one sender stacks with the name said once.
 *
 * The measure and the draw share one wrap walk (struct mesh_ui_wrap) and one pass over the
 * trailing run, so the rows a bubble reserves and the rows it paints cannot disagree - which
 * they must not, because the transcript places the next bubble from the count this one
 * reported.
 */
struct fb_bubble {
    const char *separator; /* centred label above the bubble ("Today", "14:05"); "" for none */
    /*
     * Which kind of separator it is, and so how loud it is drawn. DIM is a date or a silence;
     * anything else is the unread line.
     *
     * One slot rather than two, because a bubble has one row above it and the two can want it at
     * once - a conversation left yesterday and returned to today is exactly that case. The
     * unread line wins there, and it should: the date is recoverable from the clock in the
     * bubble's own trailing run, and "this is where you stopped" is sayable in one place only.
     */
    enum mesh_ui_tone separator_tone;
    const char *name; /* sender line inside the bubble; "" when it repeats the one above */
    /*
     * The message this one answers, as one dim line above the text with a bar down its left
     * edge - the quote block every messenger draws for a threaded reply. "" for none, which is
     * every message that is not one and every reply whose target the ring has since evicted.
     *
     * One line, and elided rather than wrapped: it is a reminder of something that is already
     * further up the transcript, not a second message. A quote that could grow would let one
     * bubble be mostly somebody else's words.
     */
    const char *quote;
    const char *text; /* the message */
    /*
     * Why a failed message failed, as a wrapped supporting line under the text.
     *
     * Its own line rather than another clause in the trailing run, which is where it used to
     * be. A reason is a sentence - "no public key for that node" is twenty-seven cells against
     * a bubble that holds thirty-nine at the device scale and twenty-five at the largest - so a
     * run carrying one could never be a corner mark, and it was what pushed the run past the
     * bubble in the first place. A failure is worth the row; nothing else here is.
     */
    const char *note;
    struct fb_bubble_meta meta;
    bool outbound; /* ours: drawn against the right edge */
    bool selected; /* the cursor is on it */
    bool failed;   /* the radio said it did not get there */
    /* A critical alert (ALERT_APP). Draws the name line in the bad tone rather than the accent,
       which is the one line every bubble in a channel already has - so an alert is picked out
       without a bubble fill that would then mean two different things in one colour. */
    bool alert;
};

/* Body rows the bubble occupies, separator included. Ask before placing it. */
uint32_t fb_bubble_rows(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout, const struct fb_bubble *bubble);

/* Draws it with its top row at `y`. Occupies exactly fb_bubble_rows() rows. */
void fb_draw_bubble(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                    int y, const struct fb_bubble *bubble);

/*
 * A centred label with a hairline either side, filling one body row. What separates one day - or
 * one long silence - from the next, and what rules a line under where the reader last stopped.
 *
 * `tone` is which of those two it is. A date is furniture and is drawn dim; "new from here" is
 * the one line on the transcript the reader is actually looking for, and a dim one is a line
 * the eye slides off - which is the whole of why the parameter exists rather than the component
 * picking DIM for everything.
 */
void fb_draw_separator(const struct mesh_ui_backend_fb_state *state, int y, const char *label,
                       enum mesh_ui_tone tone);

#endif /* MESH_UI_BACKENDS_FB_WIDGETS_BUBBLE_H */
