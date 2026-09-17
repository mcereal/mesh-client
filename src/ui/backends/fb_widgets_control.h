#ifndef MESH_UI_BACKENDS_FB_WIDGETS_CONTROL_H
#define MESH_UI_BACKENDS_FB_WIDGETS_CONTROL_H

/*
 * The things a press changes: the switch, the checkbox and radio, the segmented button, and the
 * text field.
 *
 * The first three animate, so they take the state mutably - the easing is stepped by the draw,
 * which is what keeps a knob's position out of the nav model and off the store.
 */

/*
 * Not public API. include/mesh/ui/backends/fb.h is; fb_widgets.h is the umbrella over this file
 * and its siblings, and nothing outside src/ui/backends/ should include either.
 */

#include "fb_internal.h"

#include "mesh/ui/theme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A switch: a boolean the eye reads without reading a word.
 *
 * The knob slides rather than jumps, which is the whole reason this is a component and not two
 * fills. Where it has got to is not in the snapshot - the snapshot only knows on or off - so
 * the backend remembers, keyed by `id`, in the animation table on the state
 * (include/mesh/ui/anim.h). A screen therefore passes identity, not position, and gets the
 * transition for free.
 *
 * `id` must be stable for as long as the control is on screen and unique within the frame -
 * mesh_ui_setting_field is exactly such a key, which is what the Settings rows use. An `id` of
 * 0 means "no identity": the switch draws correctly, just without ever animating.
 *
 * Geometry is derived from the glyph metrics, so the control grows and shrinks with the
 * theme's scale like every other thing on the row.
 *
 * Colours are the theme's validated pairs and nothing else. On the accent track the knob is
 * ON_ACCENT and off it the knob is TEXT_ON_SEL, which are the two pairs
 * mesh_ui_theme_validate() already holds to 4.5:1 - so a theme cannot be added that makes the
 * knob disappear. Both pairs are contracted against the *ground*, which is why a switch on a
 * selected row lays its own ground first rather than sitting on the cursor fill: on two of the
 * four themes the cursor fill and the resting track are the same colour, and the control would
 * have vanished on exactly the row the cursor was on.
 */
struct fb_switch {
    struct fb_rect rect; /* the box it is drawn in; fb_switch_size() measures one */
    uint32_t id;         /* identity for the animation, 0 for none */
    /* The family the track takes once the knob is past the middle. Zero is
       MESH_UI_FAMILY_PRIMARY, which is what a plain preference wants; a toggle that arms
       something can name the error family and be red while it is on. */
    enum mesh_ui_family family;
    bool on;
    bool selected; /* the row under it carries the cursor fill */
    /*
     * What the control is standing on when the row under it is not the cursor's: the panel, or
     * the surface of the card its list drew the group on.
     *
     * It matters because this control lays its own ground under a cursor fill - the colour pairs
     * it is contracted against are contracted against what it sits on, and on two of the four
     * themes the cursor fill *is* one of them. That patch has to be the row's ground and not the
     * panel's, or a control on a card gets a hole punched round it.
     *
     * MESH_UI_COLOR_BG is 0, so a caller drawing onto the panel leaves it zeroed and says
     * nothing. A control in a list never sets it at all: fb_list_item() writes it from the
     * model, the same way it already writes `selected`.
     */
    enum mesh_ui_color ground;
    bool dim; /* it reports a state rather than offering one: drawn muted */
};

/* The size a switch wants at `scale`. Both out params may be NULL. */
void fb_switch_size(const struct mesh_ui_backend_fb_state *state, int scale, int *w, int *h);

/* Draws it, advancing the knob towards its target. Needs the mutable state: the animation it
   is stepping lives there. */
void fb_draw_switch(struct mesh_ui_backend_fb_state *state, const struct fb_switch *sw);

/* ---- the selection control ------------------------------------------------------------------
 *
 * The other two answers to "which of these", and the two the set did not have: a checkbox and a
 * radio.
 *
 * A switch is a boolean that *acts* - flick it and the thing it names is on. Neither of these
 * is. A radio is one of a set of alternatives, so it says as much about the rows it is not on
 * as about the row it is on, and the meaning is only there because the whole column is drawn:
 * one radio alone is a switch that has forgotten how to say off. A checkbox is a boolean that
 * is *part of a set* rather than a thing in itself - which is why both live in a list's
 * trailing slot and the switch is the only one of the three that also makes sense on its own.
 *
 * So they are one component and one drawing, and the shape is the sentence: a circle is "one of
 * these", a square is "any of these". That is not a stylistic convention this file is copying,
 * it is the only part of either control the eye reads before it has counted the column.
 *
 * What it replaced, on the one screen that had it: the "send to" picker gave the current
 * target's avatar a stated accent fill. That works for one choice and does not generalise -
 * and it costs the row the very thing the disc is there for, because the tint the whole UI
 * identifies a node by is exactly what the accent fill overwrites. Identity is the leading
 * slot's job and selection is the trailing slot's; a row that said both in one disc was a row
 * where turning the second on turned the first off.
 *
 * Everything else is the switch's, deliberately: `id` keys the same animation table, the
 * geometry falls out of the same glyph metrics, and a selected row gets its own ground laid
 * for the same reason - two of the four themes make the cursor fill and the resting control the
 * same colour.
 */

/*
 * A note before the enum, because the checkbox spent four steps here with no caller and §5 of
 * the component roadmap is about exactly that: a slot that is implemented, documented and
 * unused reads exactly like a slot that is in use.
 *
 * It has one now, and it is not the multi-select list this note used to predict. The Settings
 * tab's FLAG rows - the ten bits of a position packet's `position_flags` - are a set of
 * booleans held in one word, which is the checkbox's own sentence arriving from a direction
 * nobody was watching: the rows are not a list that can arm more than one entry, they are one
 * value that has more than one bit. A screen names MESH_UI_SETTING_FLAG and gets a square.
 */
enum fb_selection_shape {
    /* A square: any number of these may be on, and this one's state says nothing about its
       neighbours. */
    FB_SELECTION_CHECKBOX = 0,
    /* A circle: exactly one of the column is on. */
    FB_SELECTION_RADIO,
};

struct fb_selection {
    struct fb_rect rect; /* the box it is drawn in; fb_selection_size() measures one */
    uint32_t id;         /* identity for the animation, 0 for none */
    enum fb_selection_shape shape;
    /* The family the mark takes when it is on. Zero is MESH_UI_FAMILY_PRIMARY, which is what a
       plain choice wants; a row selected *for deletion* can name the error family. */
    enum mesh_ui_family family;
    bool on;
    bool selected; /* the row under it carries the cursor fill */
    bool dim;      /* it reports a state rather than offering one: drawn muted */
};

/* The size a selection control wants at `scale` - a square, so both out params get the same
   number. Both may be NULL. */
void fb_selection_size(const struct mesh_ui_backend_fb_state *state, int scale, int *w, int *h);

/* Draws it, growing the mark towards its target. Needs the mutable state: the animation it is
   stepping lives there, exactly as the switch's does. */
void fb_draw_selection(struct mesh_ui_backend_fb_state *state, const struct fb_selection *sel);

/* ---- the segmented button -------------------------------------------------------------------
 *
 * A small set of alternatives, all of them on screen at once.
 *
 * fb_draw_chip()'s own comment has said since it was written that a tab strip, a filter row and
 * a segmented control are one shape, and this is the third of them: the same buttons, sized to
 * a share of the room rather than to their own words, inside one outlined container that says
 * they are alternatives rather than a row of separate offers.
 *
 * What it is for. An ENUM setting was a word in the value column stepped with Left and Right,
 * which shows one option at a time - so "Metric" gives no sign that there is an "Imperial"
 * behind it, and three-valued settings gave no sign of which end of the three you were at.
 * Two to four choices is exactly the range where showing the set costs less room than hiding
 * it; above that the words stop fitting and the value column is the honest answer, which is
 * why the component measures rather than assumes (see `value` below).
 *
 * It takes no input of its own and deliberately so. Left and Right already step an ENUM field,
 * the marker gutter already carries the pencil that says so, and a control that grew its own
 * cursor would be a second opinion about a row the list is already highlighting.
 */

/* At most this many segments. Not a buffer bound - it is the point at which a segmented button
   stops being one: five equal shares of a value column are five clipped words, and the set that
   cannot be read at a glance is better read one at a time. */
#define FB_SEGMENTED_MAX 4U

struct fb_segmented {
    const char *labels[FB_SEGMENTED_MAX];
    size_t count;
    /* Which one is set. Outside `count` is not clamped anywhere - see `value` below: a choice
       the set does not contain is drawn as the words, never as the first segment lit. */
    size_t active;
    /*
     * The same choice in words, for when the control cannot draw it.
     *
     * Two ways that happens and they are equally ordinary. There is not the room - a value
     * column too narrow for the segments - or `active` is outside the set, which is a state a
     * radio can genuinely report: an enum value from a newer firmware, or a corrupt one. The
     * item carries and formats it either way ("Unknown"), so the words are always the honest
     * answer and they are always already here.
     *
     * Not a fallback bolted on: it is what the row would have drawn anyway - `item.value` is
     * this string - and naming it here is what lets one function decide between the two. A
     * screen that had to ask "does it fit" would be a screen computing pixels, and a slot
     * measured by one rule and drawn by another is the bug the trailing slot exists to prevent.
     */
    const char *value;
};

/* The width the set wants at `scale`: every segment as wide as the widest label's button,
   because equal shares are what make it read as one control rather than as chips. */
int fb_segmented_width(const struct mesh_ui_backend_fb_state *state,
                       const struct fb_segmented *segmented, int scale);

/* The height it wants at `scale` - the switch's, so the two controls sit the same distance off
   a row's top and bottom edges. `scale` here is the row's, not the segments': a control's labels
   may be chrome-sized without the control shrinking away from the switch above it. */
int fb_segmented_height(const struct mesh_ui_backend_fb_state *state, int scale);

/*
 * Draws the set into `rect`, which is divided into `count` equal shares.
 *
 * `ground` is what the caller has filled behind it, on the same terms as a button's - the
 * container is an outline, so what shows through it is the caller's, not this component's.
 */
void fb_draw_segmented(const struct mesh_ui_backend_fb_state *state, const struct fb_rect *rect,
                       const struct fb_segmented *segmented, bool selected,
                       enum mesh_ui_color ground, int scale);

/* ---- the text field -------------------------------------------------------------------------
 *
 * A box holding text that is being typed, with a label over it and a counter under it.
 *
 * The on-screen keyboard drew this by hand: the outline, the fill inside it, the corner radius,
 * where the caret goes, which tail of an overlong draft to show, and the right edge the counter
 * is aligned against. That is a container's geometry written out in a screen renderer, which is
 * the one thing fb_screens.c is not supposed to contain - and it was the last of it.
 *
 * There is one field on screen at a time and it is always the thing being edited, so there is
 * no unfocused state to draw: a text field here is a *focused* text field, which is why it has
 * no `selected` and takes no cursor index. What it does have is the two things the keyboard got
 * wrong when it owned them:
 *
 *   - **the tail, not the head.** A draft longer than the box shows its *end*, because the end
 *     is where the caret is and the caret is what the next press moves. Measured in cells, so a
 *     draft of emoji scrolls by glyphs rather than by bytes.
 *   - **the counter is outside the box.** It reports on the field rather than being part of
 *     what is typed, and text inside the fill that is not the value reads as the value.
 */

struct fb_text_field {
    /* Over the box, at the chrome scale - what Material would float on the outline. "" for a
       field whose screen title already says what it holds. */
    const char *label;
    const char *value;
    /* The caret after the value. A block rather than a bar, because at this glyph scale a
       one-pixel rule beside a five-pixel-wide cell is not findable. */
    bool caret;
    /* Wrapped lines the box holds. 0 is read as 1; the box is this tall whether or not the
       value fills it, so it does not change height as somebody types. */
    uint32_t lines;
    /* Under the box, against its trailing edge: "12/233". "" for none. */
    const char *counter;
    /* The value is not something the field will accept - the outline and the counter take the
       bad tone. Nothing here decides that; a screen does. */
    bool error;
};

/* Pixels the field occupies, label and counter included - what a screen laying something out
   under it needs, and what fb_draw_text_field() advances by. */
int fb_text_field_height(const struct mesh_ui_backend_fb_state *state,
                         const struct fb_layout *layout, const struct fb_text_field *field);

/* Draws it with its top edge at `*y` and advances `*y` past it. */
void fb_draw_text_field(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout, int *y, const struct fb_text_field *field);

#endif /* MESH_UI_BACKENDS_FB_WIDGETS_CONTROL_H */
