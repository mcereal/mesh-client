#ifndef MESH_UI_BACKENDS_FB_WIDGETS_ITEM_H
#define MESH_UI_BACKENDS_FB_WIDGETS_ITEM_H

/*
 * One row of a list, and what sits in its slots.
 *
 * A screen fills in the slots - a marker gutter, a leading disc or icon, a label, a trailing
 * value or control, a supporting line - and this measures them into the row the list model
 * handed out. The conversation cell is the two-row variant the Messages list is made of.
 *
 * A trailing slot can hold any of the controls, so this is the one component header that names
 * most of the others.
 */

/*
 * Not public API. include/mesh/ui/backends/fb.h is; fb_widgets.h is the umbrella over this file
 * and its siblings, and nothing outside src/ui/backends/ should include either.
 */

#include "fb_internal.h"
#include "fb_widgets_control.h"
#include "fb_widgets_list.h"
#include "fb_widgets_meter.h"

#include "mesh/ui/icon.h"
#include "mesh/ui/theme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- the list item ------------------------------------------------------------------------
 *
 * One component for every row this UI draws that is more than a line of text.
 *
 * There were four of these, and they were the same row four times: a settings row was a label
 * column and a value, a toggle row was that with a control on the end, a conversation was a
 * disc and two lines with a count after them. Each carried its own copy of the two things that
 * are actually hard - clipping the text to leave a trailing control its room, and picking the
 * ink for a row the cursor is on - and each got them slightly differently.
 *
 * So it is one item with *slots*, which is the shape the phone and desktop platforms all
 * settled on: something optional at the leading edge, one or two lines of content, something
 * optional at the trailing edge. A caller fills in the slots it wants and leaves the rest
 * zeroed, and adding a new kind of row stops being a new function.
 *
 *     const struct fb_list_item row = {
 *         .label = item->label,
 *         .label_cols = label_cols,
 *         .marker_icon = MESH_UI_ICON_EDIT,
 *         .value = item->value,
 *         .tone = MESH_UI_TONE_NORMAL,
 *     };
 *     fb_list_item(state, &list, i, &row);
 */

/* What sits against the row's trailing edge. The row is clipped to leave it room rather than
   drawn under it, whichever of these it is. */
enum fb_trailing_kind {
    FB_TRAILING_NONE = 0,
    FB_TRAILING_TEXT,   /* right-aligned and quiet: an age, a "not loaded" */
    FB_TRAILING_BADGE,  /* a filled capsule: an unread count, said the way messengers say it */
    FB_TRAILING_SWITCH, /* the boolean control - see struct fb_switch */
    /* One cell against the trailing edge: the chevron that says a row opens something, the
       check that says this is the one in use. The slot every platform's list rows end with. */
    FB_TRAILING_ICON,
    /*
     * A short bar against the trailing edge: how far a download has got, how full something is.
     *
     * Inline rather than a band under the row because the list's scroll window counts rows, and
     * a row that quietly grew a second tier would put the cursor and the fill in two different
     * places. A bar the width of a few cells is enough to be read as a length, which is the
     * whole of what it is for - the exact figure is what the value column beside it is for.
     */
    FB_TRAILING_METER,
    /*
     * A staircase of rungs against the trailing edge: how well we hear a node, said the way
     * every handset says it.
     *
     * The one slot that carries two things, and the pair is why it exists. A node row's
     * trailing column was "4.2dB 3m" - a figure whose scale nobody carries around, next to an
     * age - and on a list of forty-two nodes that is forty-two numbers to read in order to
     * find the one that is fading. Rungs are counted at a glance and compared against each
     * other down the column without being read at all, which is the whole of what a list wants
     * from a signal; the exact figure is on the node's own screen, where there is one of it.
     *
     * `text` still draws, quietly, to the left of the rungs - the age, which the rungs have
     * nothing to say about. Rightmost is the signal, exactly as a status bar orders the two.
     */
    FB_TRAILING_SIGNAL,
    /*
     * A trend line against the trailing edge: which way a reading has been going - see struct
     * fb_sparkline.
     *
     * The slot's second picture of a reading, and the pair with FB_TRAILING_METER is the point:
     * an inline bar says where a number sits between its ends *now*, and a row that already
     * carries a bar under its words - the node detail's battery, say - has said that twice
     * before it has said anything about the direction. Six cells of line is where the direction
     * fits, and it is the one thing on such a row that its figure, its bar and its band all
     * leave out.
     */
    FB_TRAILING_SPARK,
    /* The two selection controls - see struct fb_selection. A checkbox for a boolean that is
       one of a set - the settings tab's flag rows, which are the bits of one word - and a
       radio for one alternative among a column of them. */
    FB_TRAILING_CHECKBOX,
    FB_TRAILING_RADIO,
    /*
     * A small set of alternatives with all of them on screen - see struct fb_segmented.
     *
     * The slot that can come back as words. Every other kind here either fits or is dropped;
     * this one has a second form the caller has already supplied, so a value column too narrow
     * for three segments draws the chosen word instead of nothing. That is not the slot being
     * inconsistent: a switch with no room has nothing to fall back to, and a set of choices
     * always does.
     */
    FB_TRAILING_SEGMENTED,
};

struct fb_trailing {
    enum fb_trailing_kind kind;
    /* BADGE: which family the capsule is filled with. Zero is MESH_UI_FAMILY_PRIMARY - an
       unread count - and a row counting failures can name the error family instead. */
    enum mesh_ui_family family;
    const char *text;       /* TEXT and BADGE, and the quiet figure beside SIGNAL */
    enum mesh_ui_icon icon; /* ICON */
    struct fb_switch *sw;   /* SWITCH. Its rect is filled in by the row: where the value column
                               ends is the row's business, not the caller's. */
    struct fb_meter *meter; /* METER. Its rect is filled in by the row, as the switch's is. */
    /* SPARK. Its rect is filled in by the row, as the meter's is. */
    struct fb_sparkline *spark;
    /* CHECKBOX and RADIO. Its rect is filled in by the row, as the switch's is; `shape` is set
       from the kind, so a caller cannot name one and draw the other. */
    struct fb_selection *sel;
    /* SEGMENTED. Read, not written: unlike the controls above it needs no rect back, because
       what it is given is a share of the row rather than a box of its own size. */
    const struct fb_segmented *segmented;
    /* SIGNAL: rungs lit, 0..MESH_UI_SIGNAL_STEPS. mesh_ui_signal_level() is what answers it, so
       that the quantising is arithmetic a test can reach rather than a ladder in a renderer. */
    uint8_t signal;
};

/*
 * An icon in a row's own slots - leading, marker, supporting, trailing - is drawn in the row's
 * ink, and there is deliberately no way to ask for another colour.
 *
 * It is standing in for a character that used to be part of the row's text ("> ", "* ", "#"),
 * so it inherits what that character would have had: the row's tone on the ground, the
 * cursor's ink under the cursor, and the quiet pairing for a trailing slot, exactly as a
 * trailing age is quiet. A screen that wants an icon to shout says so by giving the *row* a
 * tone - which is the same sentence it was already making about the words.
 */

struct fb_list_item {
    struct fb_leading leading;

    /*
     * The headline. Two shapes, and `label_cols` is which:
     *
     *   0        `text` is the whole line - a plain row.
     *   non-zero `label` occupies exactly that many cells, then `marker` and `value` - the
     *            label/value shape the settings and node-detail rows have. Measured in cells,
     *            so a value column lines up under a label that is not all ASCII.
     */
    const char *text;
    const char *label;
    size_t label_cols;
    /*
     * The gutter between the label column and the value, which says what the row *offers*: the
     * pencil on a row Left and Right change, the dot on one changed and not yet written, the
     * chevron on one that opens something.
     *
     * It is one cell wide whether or not there is an icon in it, so the value column starts in
     * the same place on every row of a list - which is the whole reason this is a slot rather
     * than two characters somebody prepended to the value.
     *
     * On a plain row - `label_cols` of 0, `text` the whole line - the gutter goes *before* the
     * words instead, and `marker_slot` is what puts it there. The star on a pinned node is the
     * case: a fact about the row, one cell, and neither the identity the leading disc carries
     * nor one of the row's own words.
     */
    enum mesh_ui_icon marker_icon;
    /*
     * Reserve the marker cell on a plain row, whether or not this row filled it.
     *
     * A label column measures the gutter for the rows that have one; a plain row has nothing to
     * measure it against, so the list declares it - on every row, exactly as it declares a
     * leading slot, because a list that indents only the rows with a marker is a list whose
     * text starts in two columns. Ignored when `label_cols` is set, which already has a gutter.
     */
    bool marker_slot;
    const char *value;
    enum mesh_ui_tone tone;
    /*
     * Whether the label column is the row's quiet tier.
     *
     * A label and the value beside it are two tiers of one row, and until now they were one
     * string: fb_item_headline() pasted the column, the marker gutter and the value together
     * and the row drew the result in a single colour - so on every fact this client states,
     * the question and the answer were typographically identical and a card of them read as a
     * block of text with no way into it. That is the bubble's trailing run one component over,
     * and it is fixed the same way: the pieces are drawn as pieces, so each can take its own
     * ink.
     *
     * Which tier is quiet is the row's to say, because it depends on what the row *is*. On a
     * control row the label is what the reader is choosing and the value is where it currently
     * stands, so the label leads - which is the zero, and the whole row draws in `tone` exactly
     * as the composed line did. On a stated fact the label is the question and repeats down the
     * column while the value is what the reader came for, so the label recedes and the value
     * keeps the row's own tone.
     *
     * A flag rather than a tone of its own, and that is the correction rather than a shorthand.
     * A tone cannot say "whatever the row is": MESH_UI_TONE_NORMAL is the zero, so a field
     * holding one would silently flatten every row whose tone is *not* normal - a settings
     * section that is not loaded is dim and an unsaved field is strong, and both state that
     * about the whole row. Spelled as a tone, an unloaded section drew its name at full
     * strength and read as available. Spelled as a flag, a row that says nothing here keeps
     * what it always had, by construction rather than by every caller remembering.
     *
     * It is also the shape `supporting_quiet` already has one line down, and it means the same
     * thing: a tier quiet on the ground stays quiet on the fill, taking TEXT_ON_SEL_DIM where
     * the rest of the row takes TEXT_ON_SEL.
     *
     * Ignored on a plain row, which has no label column to ink.
     */
    bool label_quiet;
    /*
     * Whether the row's tone is spent on its *marks* alone, leaving the words in ordinary ink.
     *
     * A tone says what a row means, and there are three places to spend it: the leading disc,
     * the accent edge, and the text. Spending all three at once is right for a row whose whole
     * state is unusual - a section that is not loaded, a field that will not be honoured - and
     * wrong for a column of verbs, where every row means something and only the difference
     * between them is worth a colour. Radio actions is the case: nine of its ten rows carry a
     * warning or error tone, so inking the words made the screen a wall of orange with the two
     * rows that cannot be undone somewhere inside it. The disc and the edge carry the same
     * gradient in a container, which is where Material puts it, and the labels stay a column
     * the eye can run down.
     *
     * A flag rather than a second tone, for the reason `label_quiet` is one: a tone cannot say
     * "whatever ordinary is" without MESH_UI_TONE_NORMAL's zero flattening the rows that meant
     * something by it. This says only *where* the row's one statement is drawn, never what it
     * is - the disc and `accent_edge` still read `tone`, so nothing here is a second opinion.
     *
     * Quiet wins where both are set: a tier that recedes is a stronger claim than a tier that
     * is merely ordinary.
     */
    bool label_plain;
    /*
     * Whether the value column is a *state* rather than a reading, and so is drawn as a capsule
     * instead of as words - the status bubble every phone app answers "is this thing OK?" with.
     *
     * Which capsule is not a second thing the row says, for the reason FB_LEADING_TONAL's disc
     * is not: it is `tone`'s family, and the neutral surface where the tone names none. So a
     * verified key is a green pill and an unverified one a grey pill by the row having said
     * success and nothing, and the colour-blind theme's swap reaches both without this knowing
     * a palette exists.
     *
     * The shape is the point rather than the colour. A state said in ink alone is a state said
     * to people who can tell those two inks apart; a state said in a pill is legible as *a
     * state* before it is read at all, which is what stops "verified" and "Weather Hut" being
     * typographically the same kind of answer.
     *
     * It follows that a row whose value is a measurement, a name or an identifier must not set
     * it - a capsule round "6.75 dB" is a pill that shouts a number - and that a card where
     * every row set it is a column of pills reporting nothing. Same bar fb_draw_badge() states.
     *
     * Ignored on a plain row, which has no value column, and silently declined when the value
     * column is too narrow for the capsule: the words are drawn instead, which is the fallback
     * FB_TRAILING_SEGMENTED already has and is right for the same reason - a caller that has
     * supplied the text has supplied something to fall back to.
     */
    bool value_chip;
    struct fb_trailing trailing;

    /*
     * The supporting line. Non-NULL is what makes this a two-row item.
     *
     * It is set closer to the headline than two separate rows would be, and the space that
     * frees becomes the gap between items - otherwise a column of two-line items reads as one
     * block of text with no way into it.
     */
    const char *supporting;
    /* One cell before the supporting line, on the same terms as the marker: the reply arrow
       that says the last word in a thread was ours. */
    enum mesh_ui_icon supporting_icon;
    enum mesh_ui_tone supporting_tone;
    /* Whether the supporting line stays secondary even under the cursor. A row's ink and its
       cursor ink are different pairs rather than the same colour dimmed, so a line that is
       quiet on the ground has to say whether it is still quiet on the fill: a message preview
       is, a delete warning is not. */
    bool supporting_quiet;
    struct fb_trailing supporting_trailing;

    /*
     * A bar across the row, under the words rather than against the trailing edge. Costs the
     * item a second step, and is drawn only when the list was told to give it one.
     *
     * The trailing slot's meter (FB_TRAILING_METER) is eight cells, which is enough to read as
     * a length and not enough for anything else: threshold marks land on top of each other, and
     * a domain with a negative end - a signal-to-noise ratio, which is the reading this screen
     * exists for - has its whole interesting half inside two cells. A bar with the row to
     * itself is the one that can carry bands, and it is what every phone puts under a reading
     * it wants you to judge rather than merely read.
     *
     * So the two are not variants of one slot. Inline is for a figure the eye passes; this is
     * for one it stops on, and a row only earns the second step by being the second kind.
     */
    struct fb_meter *meter;
    /*
     * The other thing that can occupy that step: the control, where the meter is the reading.
     *
     * Two pointers rather than a kind and a union, because unlike the trailing slot there is no
     * measurement to get wrong - a bar is the width the words had, whichever of the two it is -
     * and one function answers how tall the step must be for either. A row that set both would
     * draw them on top of each other, which is a call site with two opinions about what its
     * second step is for and not a state this can resolve for it.
     */
    struct fb_slider *slider;

    /* A bar down the leading edge when the cursor is on the row. Not decoration: a fill one
       step off the ground is not by itself findable on a small panel in sunlight, and gives a
       colour-blind eye nothing at all.

       Drawn in the row's own tone when that tone names a family, and in the primary otherwise -
       so a row marked because it has an unsaved draft gets a bar the colour of "unsaved" rather
       than one the colour of everything else that is merely marked. */
    bool accent_edge;
    /* An inset hairline below, between this item and the next. Skipped under the cursor, whose
       fill is already doing that job, and below the last item on screen - a rule separates two
       things, and under the last one there is nothing to separate it from. */
    bool divider;
};

/*
 * Draws the item and advances past the row (or two) it occupies.
 *
 * Takes the state mutably, unlike the plain row above: a trailing switch steps an animation
 * kept on it, keyed by the control's identity. That is the direction the whole component set
 * is going - a meter and a progress bar want the same table - so it is the item API that
 * carries it rather than a second entry point per animated slot.
 */
void fb_list_item(struct mesh_ui_backend_fb_state *state, struct fb_list *list, uint32_t index,
                  const struct fb_list_item *item);

/*
 * A conversation cell: the component the Messages list is made of.
 *
 * Two body rows, laid out the way every messenger lays this out - a tinted disc with the
 * correspondent's initials, then the name with the age of the last traffic against the right
 * edge, then what was last said with the unread count as a pill after it. The shape is what
 * makes the list skimmable: the eye finds a thread by the colour and the two letters, long
 * before it has read a name.
 *
 * Everything here is content, not geometry. The disc's size, the text column it pushes the
 * name into and the pill's corner radius are all derived from the glyph scale down in
 * fb_draw_conversation(), because they have to stay in proportion as a theme changes it.
 */
struct fb_conversation {
    const char *avatar; /* one or two cells inside the disc: a node's initials */
    /* Inside the disc instead of initials, for the rows that are not a person: the tag on a
       channel, the globe on all traffic, the plus on the row that starts a new thread. */
    enum mesh_ui_icon avatar_icon;
    uint32_t tint;         /* seeds the disc's colour; ignored when `accent` is set */
    bool accent;           /* draw the disc in the accent instead - "All traffic", "New message" */
    const char *name;      /* who or where */
    const char *age;       /* "2m" since the last message; "" when the radio has no clock */
    const char *preview;   /* the last thing said; "" for a conversation with no traffic yet */
    bool preview_outbound; /* it was ours, so the preview is marked as a reply */
    const char *badge;     /* unread count as it should read ("3", "99+"); "" for none */
    bool unread;
    /*
     * The user has asked this conversation not to interrupt them.
     *
     * It is a *fact about the row* rather than something the row offers, which is what puts it
     * in the marker gutter beside the pinned node's star rather than in a trailing slot. And it
     * changes the badge rather than removing it: a muted thread still says how much has piled
     * up in it, in the secondary family instead of the accent, because muting a conversation is
     * asking not to be interrupted by it and not asking to be kept in the dark about it.
     */
    bool muted;
    /* X has been pressed once on it: the cell asks the question rather than the footer, so the
       row that would go is the row carrying the warning. */
    bool armed;
    enum mesh_ui_tone name_tone;
};

/* Draws one conversation into the next two rows of `list` and advances past them. Mutable
   state, like every fb_list_item() caller: the item is the thing that can carry an animated
   control, so the whole entry point takes the table it would step. */
void fb_draw_conversation(struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                          uint32_t index, const struct fb_conversation *conversation);

/* The label column width for a body this wide - narrow scales give the value more room, at the
   width the theme calls narrow. `preferred` of 0 takes the theme's own. */
size_t fb_field_label_cols(const struct mesh_ui_backend_fb_state *state,
                           const struct fb_layout *layout, size_t preferred);

#endif /* MESH_UI_BACKENDS_FB_WIDGETS_ITEM_H */
