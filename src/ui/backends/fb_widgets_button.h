#ifndef MESH_UI_BACKENDS_FB_WIDGETS_BUTTON_H
#define MESH_UI_BACKENDS_FB_WIDGETS_BUTTON_H

/*
 * The shapes that size themselves to their own label: the button, the chip, the strip a row of
 * chips is laid out in, and the badge.
 *
 * A button is what the rest of this group is built from - a rect, an optional icon, a label,
 * and a variant saying how much of itself it shows at rest. A chip is a button sized to its own
 * words and drawn as a pill; a badge is that pill with nothing to press.
 *
 * Nothing here knows where on the panel it is being drawn, which is what lets the same chip
 * serve a tab strip in the chrome, a filter row on a screen and a relay mark inside a bubble.
 */

/*
 * Not public API. include/mesh/ui/backends/fb.h is; fb_widgets.h is the umbrella over this file
 * and its siblings, and nothing outside src/ui/backends/ should include either.
 */

#include "fb_internal.h"

#include "mesh/ui/icon.h"
#include "mesh/ui/theme.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * How much of itself a button shows when the cursor is not on it.
 *
 * The same three the phone and desktop platforms all landed on, for the same reason: a screen
 * full of controls with equal weight has no shape, so a button says how much it is asking for.
 * Each variant is a *pair* of colours the theme is validated on, at rest and under the cursor,
 * which is why this is an enum here rather than a fill colour at the call site.
 */
enum fb_button_variant {
    FB_BUTTON_TEXT = 0, /* nothing until the cursor arrives: a keyboard's character keys */
    FB_BUTTON_FILLED,   /* always visibly a control: the keyboard's action row */
    /* The accent, held back far enough to sit behind a label: the selected tab. What Material
       calls a tonal button, and what its navigation bar's active indicator is. */
    FB_BUTTON_TONAL,
};

/*
 * A pressable cell with a centred label.
 *
 * The on-screen keyboard's character keys and its action row are both this, and so is a tab
 * once it is given a rect.
 */
struct fb_button {
    struct fb_rect rect;
    /* Drawn before the label, or centred alone when there is no label - which is what the
       keyboard's action row is: a soft keyboard says backspace and send with a symbol on every
       platform there is, and the words for them are the two longest labels in the catalog. */
    enum mesh_ui_icon icon;
    const char *label;
    bool selected; /* the cursor is on it */
    enum fb_button_variant variant;
    /*
     * Which family a TONAL button is tinted with. Zero is MESH_UI_FAMILY_PRIMARY, which is what
     * a tab, a chip and an ordinary affirmative all want; a destructive confirm names the error
     * family instead and gets a red pill with the ink the theme checked against it.
     *
     * Ignored by the other two variants: FILLED is the neutral cursor surface - a keyboard key
     * is not a statement about meaning - and TEXT lays down no fill to tint.
     */
    enum mesh_ui_family family;
    enum mesh_ui_shape shape; /* how round; MESH_UI_SHAPE_FULL is a pill */
    /* Label tone for a button showing no fill at all, which is FB_BUTTON_TEXT at rest and
       nothing else. Every other state draws its label in the colour the theme is validated on
       against that state's fill; a tone chosen against the ground says nothing about a fill
       over it. */
    enum mesh_ui_tone idle_tone;
    /*
     * What the button is sitting on, for the same state: the colour an icon's partial coverage
     * is blended into when the button has laid down no fill of its own.
     *
     * MESH_UI_COLOR_BG is the zero value and the usual answer - a keyboard key sits on the body
     * ground. A tab does not: the strip fills its own bar in MESH_UI_COLOR_SURFACE_LOW first,
     * and an icon blended against the ground there is drawn with a halo of the wrong colour
     * around every antialiased edge. Text does not care - a glyph is solid ink - which is why
     * this arrives with the icons and not before.
     */
    enum mesh_ui_color ground;
    int scale; /* glyph multiplier for the label */
};

void fb_draw_button(const struct mesh_ui_backend_fb_state *state, const struct fb_button *button);

/*
 * A pill sized to its own label, laid out left to right. Returns the x the next chip starts
 * at, so a strip of them is a loop with no arithmetic in it.
 *
 * The active one is a tonal fill and the rest are dim labels on whatever they sit on, which is
 * the shape a tab strip, a filter row and a segmented control all have. It is drawn at
 * MESH_UI_SHAPE_FULL for the reason a badge is: a capsule sized to its own text is read as a
 * label rather than as a box, and a strip of them is read as a set.
 */
int fb_draw_chip(const struct mesh_ui_backend_fb_state *state, int x, int y, enum mesh_ui_icon icon,
                 const char *label, bool active, enum mesh_ui_color ground, int scale);

/*
 * What a button wants to be, for the content it carries: its icon, its label, and the padding
 * either side of them.
 *
 * A button is normally given a rect by whatever is laying a grid out - the keyboard's keys are
 * all one cell wide because they are a grid. A dialog's action row is not a grid: two buttons
 * sit against a trailing edge and each is as wide as its own words, which is a measurement, and
 * a caller that worked it out itself would be padding a button by a number the button did not
 * agree with.
 */
int fb_button_width(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_icon icon,
                    const char *label, int scale);

/* What one chip takes, its trailing gap included - so a strip can ask whether it fits before it
   draws anything. The same arithmetic fb_draw_chip() advances by, because a strip that measured
   itself differently from the way it draws is a strip whose last tab falls off the panel. */
int fb_chip_width(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_icon icon,
                  const char *label, int scale);

/* ---- the chip strip ------------------------------------------------------------------------
 *
 * A row of chips laid out left to right, one of them active, sized to the room it is given.
 *
 * fb_draw_chip()'s own comment predicted this: a tab strip, a filter row and a segmented
 * control are one shape. The strip was written out in the screens layer for the tab bar's sake
 * and was private to it, so a filter row on Nodes - All / Direct / Favourites - would have had
 * to re-derive the measuring loop, which is the exact duplication fb_chip_width() was added to
 * prevent. It is a component now, and the navigation bar below is its first caller.
 *
 * The elision is the part worth having in one place. A strip that does not fit drops the
 * labels, and it drops them in two steps rather than one: first every label but the active
 * one, then all of them. Keeping the selected label longest is Material's "selected" label
 * mode, and it is the one state a row of icons cannot express on its own.
 */

struct fb_chip {
    enum mesh_ui_icon icon;
    const char *label;
    /*
     * A count riding the chip: how much is waiting behind this tab. "" for none.
     *
     * It is the navigation bar's item badge, and it is the one thing on the frame that speaks
     * for a screen the user is not looking at - which is why it belongs to the *strip* rather
     * than to fb_draw_chip(). The strip is what knows whether the labels have been elided, and
     * that is what decides the badge's shape: with words on the chips there is room for the
     * figure, and with the strip down to bare icons a capsule holding "12" would be wider than
     * the tab it is about, so it becomes a plain dot. A dot still answers the question the
     * badge exists for - is there anything there - and it is the answer Material degrades to
     * for the same reason.
     */
    const char *badge;
};

/* How much of the labels a strip is showing. Picked by the draw call from the room it is
   given; exposed because measuring and drawing have to agree about it. */
enum fb_chip_labels {
    FB_CHIP_LABELS_ALL = 0,
    FB_CHIP_LABELS_SELECTED, /* only the active chip keeps its words */
    FB_CHIP_LABELS_NONE,     /* icons alone */
};

/* What the strip takes at this setting, trailing gaps included. */
int fb_chip_strip_width(const struct mesh_ui_backend_fb_state *state, const struct fb_chip *chips,
                        size_t count, size_t active, enum fb_chip_labels labels, int scale);

/* The most labels that fit in `room`. FB_CHIP_LABELS_NONE when even the icons overrun, which
   is not a case any theme reaches - a strip of five icons is about a fifth of the panel. */
enum fb_chip_labels fb_chip_strip_fit(const struct mesh_ui_backend_fb_state *state,
                                      const struct fb_chip *chips, size_t count, size_t active,
                                      int room, int scale);

/* Draws the strip from `x`, eliding to fit `room`, and returns the x after the last chip.
   `ground` is what the caller has filled behind it - see struct fb_button. */
int fb_draw_chip_strip(const struct mesh_ui_backend_fb_state *state, int x, int y,
                       const struct fb_chip *chips, size_t count, size_t active, int room,
                       enum mesh_ui_color ground, int scale);

/* ---- the badge ------------------------------------------------------------------------------
 *
 * A capsule of text, filled from a family. Two callers: a list row's trailing slot
 * (FB_TRAILING_BADGE) and the top app bar's, which is why it is a component of its own rather
 * than a few lines inside the slot - two places drawing their own round rect are two capsules
 * that end up different shapes.
 *
 * It takes a family rather than a tone because it *fills* something, and a fill and the label
 * on it are a pair the theme was validated as a pair. It follows that anything a badge does
 * not shout is a badge that should not be there: a state every row is in is a column of colour
 * reporting nothing, and on two of the themes here it collides with the row that has something
 * to say. See fb_render_devices().
 */

/* What the capsule takes, its padding included and with no gap after it. Zero for empty text,
   so a caller can lay a slot out without testing first. */
int fb_badge_width(const struct mesh_ui_backend_fb_state *state, const char *text, int scale);

/*
 * Draws it: the capsule fills `box`, the words start at `text_y` - the same origin
 * fb_draw_text() takes - inset by the padding fb_badge_width() charged for.
 *
 * The box is handed in rather than derived because the two callers measure it differently. A
 * list row's slot is the row's cursor fill, which is one shape on a one-line row and another
 * on a two-line one; the app bar's is centred on the title's glyph body.
 */
void fb_draw_badge(const struct mesh_ui_backend_fb_state *state, const struct fb_rect *box,
                   int text_y, const char *text, enum mesh_ui_family family, int scale);

/*
 * The badge's quieter sibling: a state, said as a capsule, in the middle of a row rather than
 * against its edge.
 *
 * Same shape and the same measurement - fb_badge_width() answers for both - and a different
 * half of the family. A badge fills with the BASE, because an unread count is meant to be seen
 * from across the room; a chip fills with the CONTAINER, because a column of them is *read*,
 * and eight saturated pills down one card is the wall of colour this was added to undo. A tone
 * that names no family gets the neutral cursor surface, which is the same pair a resting filled
 * button wears and is therefore already a pair the theme was validated on.
 *
 * A tone that names no family has no container to fill with - "the state it is normally in" is
 * not one of the six things a family means - so the neutral chip is a *ring* instead: the
 * theme's outline, the row's own `ground` inside it and the row's own `ink` on that. Which is
 * also the more honest shape, and Material's own: a filled chip is a state worth reporting, an
 * outlined one a state worth checking. `ground` and `ink` are the row's because that is what the
 * capsule's inside is; a caller that is not a list row hands in whatever it is drawing on.
 */
void fb_draw_state_chip(const struct mesh_ui_backend_fb_state *state, const struct fb_rect *box,
                        int text_y, const char *text, enum mesh_ui_tone tone,
                        enum mesh_ui_color ground, struct mesh_ui_rgb ink, int scale);

#endif /* MESH_UI_BACKENDS_FB_WIDGETS_BUTTON_H */
