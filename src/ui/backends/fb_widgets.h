#ifndef MESH_UI_BACKENDS_FB_WIDGETS_H
#define MESH_UI_BACKENDS_FB_WIDGETS_H

/*
 * The components the screens are assembled from.
 *
 * The layering under src/ui/backends/ is:
 *
 *   fb_draw.c     pixels, glyphs, the theme lookups, page geometry   "how to put ink down"
 *   fb_widgets.c  buttons, chips, list rows, field rows, rules       "what things look like"
 *   fb_screens.c  one renderer per screen                            "what is on this screen"
 *
 * A screen renderer should read as a description of its content: what the list holds, what
 * each row says, which rows are actions. If it is computing a pixel coordinate, a scroll
 * offset or a padding width, that belongs down here instead - those are the three things every
 * screen used to re-derive, and the three things that were subtly wrong in a different way on
 * each of them.
 *
 * Not public API. include/mesh/ui/backends/fb.h is; nothing outside src/ui/backends/ should
 * include this.
 */

#include "fb_internal.h"

#include "mesh/ui/actions.h"
#include "mesh/ui/icon.h"
#include "mesh/ui/layout.h"
#include "mesh/ui/theme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Tones - what a thing *is*, rather than which colour to draw it - live in
 * include/mesh/ui/theme.h as `enum mesh_ui_tone`, because they are the UI's vocabulary rather
 * than this backend's. A screen names one, the theme answers, and fb_tone_color() on the state
 * is the only place the two meet.
 *
 * A widget takes a tone, never a colour, for the same reason a stylesheet has a token called
 * "danger" instead of the hex for red: it is what lets a theme change the answer.
 */

/* A box in pixels. */
struct fb_rect {
    int x, y, w, h;
};

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
 * control are one shape. The strip was written out inside fb_screens.c for the tab bar's sake
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

/* ---- the navigation bar ---------------------------------------------------------------------
 *
 * The chrome across the top: a recessed bar, one chip per tab, and the rule that closes it off.
 *
 * The bar is the point. The strip used to float on the body's own ground, which left the tabs
 * reading as the first row of content rather than as the frame around it; a recessed tier
 * behind them says "this is chrome" before a word of it is read, which is what every phone's
 * navigation bar is doing. It is the theme's lowest surface, so a palette decides how far from
 * the ground that is - on the high-contrast theme it is barely anywhere, which is correct.
 *
 * Consumes the room it occupies: `layout->body_y` comes back pointing at the first body row.
 */
void fb_draw_nav_bar(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                     const struct fb_chip *tabs, size_t count, size_t active);

/* ---- the screen progress bar -----------------------------------------------------------------
 *
 * A hairline across the panel, under the navigation bar's rule: the client is waiting on
 * something it has already asked for.
 *
 * Not a new drawing - it is fb_draw_meter() at FB_METER_INDETERMINATE, full bleed and one
 * hairline tall - and that is deliberate rather than lazy. There is exactly one "a thing is
 * working" motion in this UI, and a second implementation of a travelling pill would be a
 * second one to keep in step with the theme's timings.
 *
 * What it is for: open Settings before the radio has answered and eight sections say "not
 * loaded", which reads identically whether a request is on its way back or nothing was ever
 * sent. This is the difference, and because it is chrome it answers for every screen at once
 * rather than for the one that happened to be waiting.
 *
 * **It never moves the body.** The navigation bar already leaves a gap between its rule and the
 * first body row, and the bar hangs in that gap - so `layout` is const here, the rows a list
 * gets are the same rows whether or not anything is in flight, and a save going out does not
 * reflow the screen it was saved from. It is the same rule the card's focus ring is drawn by:
 * an indicator that changes the layout is an indicator that moves what it is pointing at.
 *
 * Which states count is not this file's business - see mesh_ui_chrome_busy() in
 * include/mesh/ui/chrome.h, which is where the UI layer answers it for every backend.
 */
void fb_draw_progress(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                      bool busy);

/* ---- the banner ------------------------------------------------------------------------------
 *
 * The persistent notice: something is true of the whole client and stays true until it is
 * resolved.
 *
 * The snackbar is the transient half of this and is correctly transient - it is for what just
 * happened. What it cannot say is what is *still the case*: a release waiting to be installed
 * was visible only inside Settings > About, so the one screen that already knew was the only
 * screen that said so. A banner is the other half: it costs body rows, it does not go away on a
 * timer, and it sits in the chrome under the tab strip where a statement about the client
 * belongs.
 *
 * Why it is above the screen's own app bar rather than below it, which is where Material puts
 * one. The navigation bar is this client's app-level chrome and the top app bar is the
 * *screen's* heading; a banner is a statement about the client, so it goes with the first. The
 * practical half of the same answer: drawn below the app bar it would have to be called by
 * every screen renderer, and the four overlays would each need their own copy - which is the
 * duplication fb_render_snapshot()'s single tail exists to prevent.
 *
 * Nothing here animates, and that is a decision rather than an omission. The container consumes
 * body rows, so a height that eased open would reflow the list underneath it for the length of
 * the animation - and unlike the snackbar, which arrives over the UI and has to be *noticed*,
 * a banner is read whenever the eye next reaches the top of the panel.
 *
 * Which banner, if any, is mesh_ui_chrome_banner()'s answer (include/mesh/ui/chrome.h).
 */
struct fb_banner {
    /* The leading symbol, in the container's own ink. */
    enum mesh_ui_icon icon;
    /* The headline: what is true. NULL or empty draws nothing at all, which is what makes "no
       banner" a struct rather than a branch at the call site. */
    const char *text;
    /* What to do about it, on a second line at the label scale. NULL for none - and dropped
       before the headline is when the body cannot spare the row for it. */
    const char *supporting;
    /* A fact stated in its own units against the trailing edge of the headline: a version
       number. Untranslated by design, so it is a string rather than an id. NULL for none. */
    const char *detail;
    /* The container's fill and the ink on it, taken together from one theme call. A family
       rather than a tone for the reason a badge takes one: this thing fills something. */
    enum mesh_ui_family family;
};

/*
 * Draws it at the top of the body and consumes the rows it took, so a screen renderer that
 * follows lays out against a shorter body without knowing this happened.
 *
 * Recomputes `rows` from the body's real bottom rather than deducting a row count, for the
 * reason fb_draw_app_bar() does - see the comment on its tail.
 */
void fb_draw_banner(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                    const struct fb_banner *banner);

/* ---- the action bar -------------------------------------------------------------------------
 *
 * The chrome across the bottom: what the buttons do here, as keycaps, over the line that says
 * what the radio is doing.
 *
 * This was two lines of plain text, and it was the last screen-level renderer laying out its
 * own pixels - which is also why it was the piece of chrome that most made the UI read as a
 * terminal rather than as a handheld OS. A hint sentence is a row of controls written down as
 * words: the letters in it are things on the case, and the verbs after them are what those
 * things do. Drawing it as keycaps says both without the eye having to parse a sentence to
 * find the one letter it was looking for.
 *
 * The keycap is FB_BUTTON_FILLED at MESH_UI_SHAPE_SM, which is the component set's existing
 * answer for "a place to press" - the on-screen keyboard's keys are the same button - so a
 * keycap here and a key there cannot drift apart.
 *
 * **What it draws is `struct mesh_ui_button_action`, never a sentence.** The bar has to iterate the
 * pairs, so the pairs have to exist before the drawing does; that is why the hint catalog
 * entries were retired in favour of the table in src/ui/actions.c. See
 * include/mesh/ui/actions.h.
 *
 * It owns the whole bottom bar - the surface, the rule above it, the keycaps and the status
 * line - for the same reason the card owns its own inset: a screen that placed the status line
 * itself would be back to computing a y coordinate in fb_screens.c.
 */

struct fb_action_bar {
    const struct mesh_ui_button_action *items;
    size_t count;
    /*
     * The line under the keycaps: the transport state, and either the radio it is attached to
     * or how to quit.
     *
     * It used to share a row with the transient notice, which took it whenever there was one -
     * so every action blanked the answer to "is there a radio attached?" for four seconds. The
     * notice has somewhere of its own now (fb_draw_snackbar), and this row says one thing,
     * always. Clipped to the panel rather than wrapped: the bar is a fixed height.
     */
    const char *status;
    enum mesh_ui_tone status_tone;
};

/* The room the bar wants at the foot of the panel - what a caller subtracts from the panel
   height to find where the body ends. */
int fb_action_bar_height(const struct mesh_ui_backend_fb_state *state,
                         const struct fb_layout *layout);

/*
 * Draws it, with its top edge at `layout->footer_y`.
 *
 * Actions that do not fit are dropped from the *end*, which is why struct mesh_ui_action_bar
 * is documented as being in priority order: on a narrow panel or in a long translation, the
 * press the screen is for survives and "L/R tabs" - true everywhere, and therefore the least
 * worth the room - is what goes.
 */
void fb_draw_action_bar(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout, const struct fb_action_bar *bar);

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
 * A note before the enum, because §5 of the component roadmap is about exactly this: a slot that
 * is implemented, documented and unused reads exactly like a slot that is in use.
 *
 * FB_SELECTION_CHECKBOX and FB_TRAILING_CHECKBOX have **no caller**. The checkbox is here
 * because it is the radio's drawing with a different corner radius, and it is unwired because
 * nothing in this client is multi-select: a list that can arm more than one row is a nav change
 * with a component on the end of it, and the component is the half that was already free. When
 * one arrives - "forget these nodes" is the likely first - the kind is waiting and this note
 * goes. Until then, do not read the checkbox as a thing the UI does.
 */
enum fb_selection_shape {
    /* A square: any number of these may be on, and this one's state says nothing about its
       neighbours. No caller yet - see above. */
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

/* ---- the meter --------------------------------------------------------------------------- *
 *
 * A quantity as a length: how much of the air the mesh is using, how much of a download has
 * arrived.
 *
 * The two are the same widget on purpose, and this file predicted it before either existed - a
 * meter and a progress bar want the same table. What separates them is not the drawing but
 * *what the number means*: a meter reports a level that will go up and down on its own, a
 * progress bar reports a job that only goes forwards and then stops. Both are a track with a
 * fill in it, both take their fill from a tone, and both animate through the same keyed slot,
 * so there is one of them.
 *
 * Why a bar and not the number it sits next to. Channel utilization was a coloured percentage,
 * and a percentage has to be read and then compared against a threshold nobody carries around;
 * a length is compared against the track it is in, which is right there. The number is still
 * drawn - it is what says *how* busy - but the bar is what says *busy*, and that is the part
 * that should not need reading.
 *
 * What it is not: a spinner with a percentage bolted on. When the extent of the work is
 * unknown the widget says so with FB_METER_INDETERMINATE and moves without claiming a
 * position, rather than inventing a fraction. A bar that sat at 30% because somebody had to
 * pick a number is worse than no bar.
 */

enum fb_meter_kind {
    /* A known fraction of a known whole: `value` is where the fill ends. */
    FB_METER_DETERMINATE = 0,
    /*
     * Something is happening and how much of it is left cannot be known - a request out on the
     * network, a hash being taken. A pill travels the track instead of a fill growing, which is
     * the one shape that says "working" without also saying "this far along".
     *
     * It costs a repaint timer for as long as it is on screen, which is the reason it is a
     * separate kind rather than the default: a screen asks for motion deliberately.
     */
    FB_METER_INDETERMINATE,
};

/*
 * A meter's band is `struct mesh_ui_band` (include/mesh/ui/theme.h), and it lives there rather
 * than here because it is not this backend's idea: it is what a number means, in the same
 * vocabulary a tone is, and the node detail's row model states one without knowing a
 * framebuffer exists. mesh_ui_band_tone() is what reads it.
 *
 * The bar with no marks on it was the gap this closes. "Is 31% a lot?" is the question the
 * meter exists to answer without arithmetic, and a bare track answers it only for somebody who
 * already carries the threshold around: the fill turned amber at a quarter, but nothing on
 * screen said where a quarter *was*, so the colour reported a boundary that could not be
 * located. A notch cut into the track at each boundary is that boundary, drawn where it is.
 */

struct fb_meter {
    struct fb_rect rect; /* the track; fb_meter_thickness() is the height one wants */
    /*
     * Identity for the animation, 0 for none - the same contract the switch has.
     *
     * It matters more here than it does there. A determinate meter *eases towards* each value
     * it is given, which is what lets a reading sampled once a second look like a bar moving
     * rather than a bar jumping; without an id it draws each sample exactly and stutters.
     */
    uint32_t id;
    enum fb_meter_kind kind;
    /* DETERMINATE: the reading, in whatever units `scale` is stated in. Clamped to the ends. */
    int32_t value;
    /* The domain `value` and `band` are on. A zeroed scale means permille, which is what every
       meter that already holds a fraction wants and why it costs those callers nothing. */
    struct mesh_ui_scale scale;
    /*
     * Where the reading changes meaning, or NULL for a plain bar.
     *
     * A banded meter takes its fill's tone from where the reading falls and draws the
     * boundaries on its track, so the colour and the marks are two readings of one statement
     * rather than two statements. `tone` is then what it rests in - see fb_band_tone().
     */
    const struct mesh_ui_band *band;
    /* The fill. ACCENT, GOOD or BAD - the three mesh_ui_theme_validate() holds against
       MESH_UI_COLOR_METER_TRACK - and anything else is drawn in the accent. */
    enum mesh_ui_tone tone;
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
};

/* The height a meter wants at `scale`, in pixels. From the theme's metrics, so a bar keeps its
   proportion to the text beside it when a theme changes the glyph scale. */
int fb_meter_thickness(const struct mesh_ui_backend_fb_state *state, int scale);

/* Draws it, advancing the fill towards its target - or the pill along its loop. Needs the
   mutable state for the same reason the switch does. */
void fb_draw_meter(struct mesh_ui_backend_fb_state *state, const struct fb_meter *meter);

/* ---- the slider ------------------------------------------------------------------------------
 *
 * A quantity the reader is *choosing*, where the meter is a quantity they are being told.
 *
 * That is the whole of what separates the two components, and it is why this is not a flag on
 * the meter. A meter reports and eases towards each reading it is handed; a slider says where a
 * value sits among the values that could have been picked instead, marks those choices on its
 * own track, and shows which one the cursor is on. The first is a picture, the second is a
 * control, and a control has a state the picture has no word for.
 *
 * What it replaced: a NUMBER setting was Left/Right over a preset list with the chosen value in
 * the value column - "5m", and nothing at all about whether 5m was near the short end of what
 * this field offers or near the long one. The figure is still drawn, because the figure is what
 * says *how long*; the track is what says *how far along*, and that is the half a row of
 * durations could not answer without the reader already carrying the list around.
 *
 * The stops are evenly spaced and the reading between them is interpolated - see
 * mesh_ui_settings_number_track(), which is where that arithmetic lives so a test can reach it. Two
 * consequences the drawing depends on: a preset list that climbs geometrically still gives an
 * aimable track, and a value the list does not contain lands between two stops rather than being
 * refused. The segmented button had to fall back to words for an unknown value because a set of
 * alternatives has no room between its members; an axis has room, so this one does not need the
 * fallback.
 */

struct fb_slider {
    struct fb_rect rect; /* the track's box; fb_slider_height() is the height one wants */
    /* Identity for the animation, 0 for none - the meter's contract, and it matters here for
       the same reason it matters on the switch: a press should move the handle, and a screen
       opening on a value should not animate up to it from zero. */
    uint32_t id;
    /* Where the value sits, in permille of the track. The caller's, not derived here: which
       values a field offers is the settings model's business and the arithmetic that places one
       among them is unit-tested there. */
    int32_t position;
    /* How many choices to mark on the track, 0 for an unmarked one. The component decides
       whether they are drawn - see fb_draw_slider() - because a mark the eye cannot separate
       from its neighbour is worse than no mark, and the width that decides it is not known
       until the row has laid the track out. */
    uint32_t stops;
    /* The active track and the handle, from one tone. ACCENT, GOOD or BAD - the meter's three,
       validated against MESH_UI_COLOR_METER_TRACK - and anything else falls back to the
       accent. */
    enum mesh_ui_tone tone;
    /*
     * The value the row is showing is not one this track has room for, so the control draws its
     * stops and nothing else - no fill, and no handle anywhere.
     *
     * §10's rule arriving on an axis: *a control that shows a set has to be able to say "not one
     * of these"*. Two things reach it. Almost every settings scale here opens with a value that
     * is a word rather than a quantity - a "default" the firmware picks, LoRa's "max" - and
     * neither belongs at the bottom of a bar; and two lists start above zero because the thing
     * receiving the setting refuses anything below that, while an unconfigured radio still
     * reports 0. An empty track is not ambiguous with a value at the minimum, because a value at
     * the minimum has a handle sitting on it.
     */
    bool unplaced;
    /* The cursor is on this row: the handle stands up to its full height, and the track gets
       its own ground under the cursor fill. A slider is the one control on a settings row that
       the reader is about to change, so it says so rather than looking the same everywhere. */
    bool selected;
};

/* The height the whole control wants at `scale` - the handle's, which is taller than its track.
   Reserved whether or not the cursor is on the row, so a handle standing up under the cursor
   does not make the row it is on grow and shift every row below it. */
int fb_slider_height(const struct mesh_ui_backend_fb_state *state, int scale);

/* Draws the track, its stops, and the handle at `position`, easing the handle towards it. Needs
   the mutable state for the reason the meter and the switch do. */
void fb_draw_slider(struct mesh_ui_backend_fb_state *state, const struct fb_slider *slider);

/* ---- the signal staircase -------------------------------------------------------------------
 *
 * Its own component rather than a variant of the meter, because it is answering a different
 * question. A meter reports a level on a continuum and eases between samples; a staircase
 * reports a *bucket*, and easing between buckets would be inventing the intermediate values
 * that quantising them was meant to refuse. Nothing here animates, and that is the design.
 *
 * The rungs it does not light are drawn rather than left out, in the quiet ink the slot's other
 * furniture takes: an indicator that shortened as the signal fell would be a length, and a
 * length is a claim about proportion that four buckets cannot support. What the eye counts is
 * lit rungs against a constant total.
 */

/* Cells a staircase occupies, its trailing gap excluded. Stated rather than measured, for the
   reason the inline meter's width is: rungs have no natural width, and two cells is where four
   of them are still individually countable at the smallest glyph scale a theme may pick. */
#define FB_SIGNAL_CELLS 2U

/*
 * Draws `level` of MESH_UI_SIGNAL_STEPS rungs inside `box`, rising left to right.
 *
 * `ink` and `unlit` are handed in rather than looked up, so that a staircase takes its pair
 * from the row it is on - which is what the trailing slot already does for every other thing it
 * draws, and what keeps this from being a fifth colour every theme has to be validated for.
 */
void fb_draw_signal(const struct mesh_ui_backend_fb_state *state, const struct fb_rect *box,
                    uint8_t level, struct mesh_ui_rgb ink, struct mesh_ui_rgb unlit);

/* ---- the sparkline --------------------------------------------------------------------------
 *
 * The same reading over time, in the room a row has: which *way* it is going.
 *
 * The third quantitative component and the first that is not about now. A meter says how much,
 * a staircase says how well, and both are read against a track that is right there - but the
 * two questions this client is actually opened for are "is the mesh getting worse" and "is this
 * battery going to last the night", and neither is answerable from a level. A reader who
 * happened to look an hour ago can answer them; nobody else can, and the client is the thing
 * that was looking.
 *
 * So it draws a memory, and the memory is `struct mesh_ui_series` (include/mesh/ui/layout.h) -
 * where the whole cost of this component is, and where the three rules that keep the picture
 * honest are stated. What is left here is the drawing, which is small.
 *
 * Four decisions in it, and each is the staircase's rules arriving on a second axis:
 *
 *   - **The vertical is the reading's own domain**, the same `struct mesh_ui_scale` the bar
 *     beside it fills against - never the range these particular samples happened to span. A
 *     line that rescaled itself to its data would draw a battery that fell two percent overnight
 *     as a cliff, which is what a spreadsheet does and what "a picture cannot be wrong quietly"
 *     forbids. It also means the line and the bar can be read against each other, because they
 *     are measured on one scale.
 *   - **Nothing animates.** A meter eases towards each reading because the value it is drawing
 *     replaces the last one; a trend *keeps* them, so there is nothing to move between. Easing
 *     the newest point into place would show a shape that was never a reading.
 *   - **The pen lifts at a gap.** A break in the line is a period nothing was reported, and
 *     drawing a slope across it claims readings nobody took - see `gap` on struct mesh_ui_point.
 *   - **It takes the row's pair, not a colour.** The line is a tone, and the floor under it is
 *     the meter's track - which is the pairing every theme is already validated for, and the
 *     reason a trend costs no new contract.
 *
 * Fewer than two points draws nothing at all, including no floor: one reading is a level, there
 * is a component for that, and an empty track in a list row is furniture reporting that nothing
 * has happened yet.
 */

/* Cells a trailing sparkline occupies, its gap to the words excluded. Stated rather than
   measured, for the reason the staircase's width is: a line has no natural width. Six is where
   two dozen samples are still individually placeable at the smallest glyph scale, and it is the
   inline meter's eight less the two the gap and the figure want back. */
#define FB_SPARK_CELLS 6U

struct fb_sparkline {
    struct fb_rect rect; /* the box the line is drawn in; fb_sparkline_height() is the height */
    /*
     * The samples, already normalised - x across the box, y up it, both in permille, oldest
     * first. mesh_ui_series_project() is what produces one, so the arithmetic that decides
     * where a reading lands is a unit test's to reach rather than a renderer's to hold.
     *
     * Borrowed for the call. Nothing here keeps it.
     */
    const struct mesh_ui_polyline *points;
    /* The line. A family tone, on the meter's terms: the neutral three fall back to the accent,
       because a stroke nobody validated against the track is a line that vanishes on a theme
       somebody has not opened yet. */
    enum mesh_ui_tone tone;
    bool selected; /* the row under it carries the cursor fill */
};

/*
 * The height the line wants at `scale`, in pixels: the glyph body's, which is taller than the
 * icon box the staircase beside it takes.
 *
 * Not an inconsistency between two things in one column. An icon has to stand as tall as the
 * capitals it is read among and no taller, and four rungs are counted rather than measured; a
 * line's whole reading is in its height, so every pixel of it is a pixel of the answer. This is
 * as tall as a row's own text, which is as tall as a slot can be without the row growing.
 */
int fb_sparkline_height(const struct mesh_ui_backend_fb_state *state, int scale);

/* Draws the floor and the line. Const state, unlike the meter and the slider: there is no
   animation to step - see above. */
void fb_draw_sparkline(const struct mesh_ui_backend_fb_state *state,
                       const struct fb_sparkline *spark);

/* ---- the proportion bar ----------------------------------------------------------------------
 *
 * A whole and the parts it is made of: the fourth quantitative component, and the one that says
 * what a reading is *composed of* rather than how large it is, how well it is doing or which way
 * it is going.
 *
 * The Status card is why it exists. The radio reports what it heard as three counters - packets
 * that were new, packets that were duplicates of something already relayed to us, and packets
 * that were malformed - and printed as three numbers those answer no question anybody has. The
 * question is a *ratio*: upstream's own comment on the duplicate counter says "if this number is
 * high, there are nodes in the mesh relaying packets when it's unnecessary", and high is not a
 * property of 4,812. It is a property of 4,812 out of 6,140, which is a length beside two other
 * lengths and is read without arithmetic - the meter's argument, on a whole with more than one
 * part in it.
 *
 * Four things about it are decisions rather than details:
 *
 *   - **It starts at three parts, because two parts is a meter.** A whole split in two is a
 *     fraction, a fraction is what a meter draws, and a meter can carry a band and a domain that
 *     this cannot. Heap free against heap total is a meter for that reason and not a composition,
 *     and nothing is gained by making it one.
 *   - **The parts must be disjoint, and that is the caller's promise.** It is the one way this
 *     component can be wrong quietly and it is not checkable from here: three counters that
 *     overlap still add up to something, and the bar drawn from them is a confident picture of a
 *     whole that does not exist. The radio's own transmit counters are exactly that trap -
 *     `num_tx_relay` is a subset of `num_packets_tx` rather than a sibling of it - so "tx, rx,
 *     relayed" is three numbers and not three parts, and there is no bar under that row.
 *   - **The colours are the theme's series palette, not tones.** A part means nothing except
 *     which part it is; a tone means good, bad or caution. Filling three slices from three
 *     families would report a judgement the data never made - and on the high-contrast theme,
 *     where the three non-status families are one yellow, it would report nothing at all. See
 *     MESH_UI_SERIES_COLORS.
 *   - **Nothing animates.** The meter eases because each reading it is handed replaces the last;
 *     these are counters that only ever climb, so between two frames a boundary moves by a
 *     fraction of a pixel. Easing it would spend an animation slot per boundary on motion nobody
 *     could see, and the slots are keyed per control.
 *
 * The slices are laid out by mesh_ui_proportion_split() (include/mesh/ui/layout.h), which is
 * where the two rules that keep the picture honest live: they sum to the bar exactly, and a part
 * that is there is never rounded away to nothing.
 */

struct fb_proportion {
    struct fb_rect rect; /* fb_proportion_thickness() is the height one wants */
    /* The parts, in the order they are drawn - which is left to right, and which is also the
       order the row above names them in. That correspondence is the whole legend: the bar has no
       words of its own, and a label per slice would not fit in a row's height on this panel even
       if it did. Nothing enforces it, for the same reason nothing enforces disjointness. */
    uint32_t values[MESH_UI_PROPORTION_PARTS];
    uint32_t count;
    /*
     * What is behind the bar, which the gaps between the parts are cut in.
     *
     * Stated by the caller rather than assumed, and it is the one field here with no sensible
     * default: a composition is drawn *inside* something - a card whose fill depends on its
     * variant - and a gap painted in the body's ground on a card is not a gap, it is a stripe
     * of a colour from somewhere else. The meter can get away with assuming, because a band
     * notch is a mark whose job is to divide a bar it is already inside; these gaps have to
     * *disappear*, which is a claim about what surrounds them.
     *
     * A zeroed struct is black rather than unset, so there is nothing to detect here: a caller
     * that forgets this draws black gaps, which is visible immediately.
     */
    struct mesh_ui_rgb ground;
    /* The row under it carries the cursor fill, so the bar lays a ground of its own - the
       meter's move, and needed here for the same reason: the series palette is validated against
       the body and against a card, and on two themes the cursor fill is neither. The gaps follow
       it: what is behind the bar on a selected row is the pad, not `ground`. */
    bool selected;
};

/* The height a composition wants at `scale` - the meter's, deliberately. A card that carried a
   bar of one weight and a bar of another would be reporting a difference between them that is
   not there: both are a reading drawn as a length. */
int fb_proportion_thickness(const struct mesh_ui_backend_fb_state *state, int scale);

/* Draws it. Const state, like the sparkline and unlike the meter: there is no animation to step
   - see above. Fewer than two parts, or parts that sum to nothing, draws nothing at all. */
void fb_draw_proportion(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_proportion *bar);

/* ---- the chart ------------------------------------------------------------------------------
 *
 * The same readings the sparkline draws in a row, in a whole body: what a trend *was*, with its
 * axes named and more than one line on it.
 *
 * The fifth quantitative component and the first that is a screen rather than a slot. Everything
 * before it fits in a row and pays for that by having no numbers on it - a sparkline is a shape,
 * and the reader has to already know what it is a shape of. That is the right trade in a list,
 * where the row above says which reading it is and the bar beside it says how far along. It
 * stops being the right trade the moment somebody stops to look, which is the press this exists
 * for, and it was the thing docs/components-roadmap.md said an axis frame would cost: not a
 * component, a *route*.
 *
 * What the room buys, in the order it matters:
 *
 *   - **The vertical says what it is measuring.** Its two ends are labelled, in the reading's own
 *     units, so "high" is a number rather than a feeling. The domain is still the reading's own
 *     `struct mesh_ui_scale` and never the range these samples happened to span - the sparkline's
 *     first rule, and it is *more* load-bearing here, not less: an axis with numbers on it is
 *     believed, so an axis that rescaled itself would be a labelled lie rather than a misleading
 *     shape.
 *   - **The horizontal says how long.** A shape with no time under it cannot distinguish a
 *     battery that fell ten percent in an hour from one that fell ten percent in a week.
 *   - **The thresholds are drawn.** The band the meter cuts notches into becomes a rule across
 *     the plot, so where a reading stops being comfortable is a line the trend can be seen
 *     crossing rather than a colour that changed at a moment nobody can locate. This is the one
 *     thing a chart says that no row-height component can.
 *   - **More than one line fits.** Which is what the legend is for, and why two lines could not
 *     be drawn in a row: the words naming them have to be somewhere, and a row has no somewhere.
 *
 * Three things it deliberately does not do:
 *
 *   - **No end mark.** A sparkline marks its newest reading because a stroke does not say which
 *     end is now, and a line falling left to right and one rising are the same picture read
 *     backwards. A chart has the answer written under it: the axis is labelled with the span it
 *     covers, so "now" is the right-hand edge by construction.
 *   - **No grid.** Two threshold rules and an axis are the marks that mean something; a lattice
 *     of evenly spaced lines is furniture that makes a picture look measured without measuring
 *     anything, and on a panel with no anti-aliasing it competes with the data for pixels.
 *   - **Nothing animates.** The sparkline's reason, unchanged: a trend keeps its readings rather
 *     than replacing them, so there is nothing to move between.
 */

/* Lines one chart may carry - the palette's count, because a line takes a series colour and two
   lines sharing one is a chart that cannot be read. The compile-time assertion beside
   fb_draw_proportion() holds the two halves of that seam equal. */
#define FB_CHART_LINES MESH_UI_SERIES_COLORS

struct fb_chart_line {
    /*
     * The samples, already normalised, exactly as the sparkline takes them - but projected by
     * mesh_ui_series_project_over() rather than by mesh_ui_series_project(), and that difference
     * is the whole of what makes two lines comparable. A series projected on its own span fills
     * whatever box it is given, so a series that stopped reporting half an hour ago would be
     * drawn as though it were still arriving. See mesh_ui_series_window().
     *
     * Borrowed for the call. Nothing here keeps it.
     */
    const struct mesh_ui_polyline *points;
    /* What the legend calls it. MESH_STR_NONE draws the line and names it nowhere, which is
       honest only when there is exactly one line - with two it is the picture asking the reader
       to guess. */
    enum mesh_str_id label;
};

struct fb_chart {
    /* Everything: the plot, the words down its side and the two lines of chrome under it. The
       caller hands over a body and this divides it, which is the one place in this component set
       where that is the right way round - a chart is the whole screen, so there is nothing else
       laying claim to the room. */
    struct fb_rect rect;
    struct fb_chart_line lines[FB_CHART_LINES];
    uint32_t count;
    /*
     * The two ends of the vertical, already formatted in the reading's own units - a percentage,
     * a temperature, a count. Formatted by the caller because the units are the caller's: this
     * knows where the top of the domain is and has no idea what it is the top *of*.
     */
    const char *top;
    const char *bottom;
    /* What the horizontal covers, as words ("45m", "3h 20m"), or NULL when the readings share
       one clock tick and there is no span to name. NULL draws no label rather than a zero: an
       axis whose span is unknown says nothing about it, and "0m" is a claim. */
    const char *span;
    /*
     * Where the reading stops being comfortable, drawn as rules across the plot, and the domain
     * they are stated in. Optional: a NULL band draws no rules.
     *
     * The scale is the same one the lines were projected against, and the component cannot check
     * that - a band placed on one domain over lines placed on another is this component's way of
     * being wrong quietly, and it is the caller's promise in the way disjointness is
     * fb_draw_proportion()'s.
     */
    const struct mesh_ui_band *band;
    struct mesh_ui_scale scale;
};

/*
 * The least room a chart is worth drawing in, in pixels of height.
 *
 * Below it the plot is shorter than the two lines of chrome under it, which is a caption with a
 * smear above it rather than a picture. A caller with less room draws something else; there is
 * no clipped chart, for the reason there is no clipped card.
 */
int fb_chart_min_height(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout);

/* Draws the frame, the threshold rules, the lines and the legend. Const state, like the
   sparkline and the composition: there is nothing here to animate. */
void fb_draw_chart(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   const struct fb_chart *chart);

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

/* ---- the top app bar ------------------------------------------------------------------------
 *
 * The heading a screen opens with: where you are, how to get out, and one fact about the whole
 * screen. Consumes the body rows it occupies, so a screen calls this and then lays its list out
 * against the layout it hands back.
 *
 * It used to take a `const char *`, which meant a screen's heading was a *string* and
 * everything a heading had to carry got glued into it. Settings built "Settings > %s%s%s" out
 * of two catalog entries and an unsaved marker, and that is a whole-sentence string id doing
 * structural work in the same way the button hints were: the `>` separators handed a translator
 * the breadcrumb's grammar along with its words, and a badge glued into a title with %s cannot
 * be a badge. What is left in the catalog is one word per level.
 *
 * The four slots, and what each is for:
 *
 *   leading    the back affordance. Not a field - it is layout->back, from the action bar's own
 *              table, so the arrow and the B keycap cannot disagree about whether B leaves.
 *   overline   the trail of levels above this one, separated by a drawn chevron. Two levels is
 *              the deepest anything here goes ("Settings > Modules" over "Telemetry").
 *   title      what this screen is. One line, at MESH_UI_TYPE_TITLE.
 *   trailing   a badge: a fact about the screen rather than about any row of it.
 */

/* Settings is the deepest trail in the tree and it is two levels; three is one level of slack
   so that a screen growing one is a call-site change rather than a component change. */
#define FB_APP_BAR_TRAIL_MAX 3U

struct fb_app_bar {
    /* Outermost first: {"Settings", "Modules"} above a title of "Telemetry". Each entry is one
       level's own name - the separators belong to the component. */
    const char *trail[FB_APP_BAR_TRAIL_MAX];
    size_t trail_count;
    const char *title;
    /* The trailing capsule, empty for most screens. `badge_family` is what it is filled with:
       the warning family for edits the radio has not been told about, which is the one this
       exists for. */
    const char *badge;
    enum mesh_ui_family badge_family;
};

void fb_draw_app_bar(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                     const struct fb_app_bar *bar);

/*
 * How far down the body the bar pushes it, without drawing anything.
 *
 * Beside fb_action_bar_height() and for the same reason: a screen that has to know how big its
 * content area will be *before* it has something to put in the bar cannot get there by drawing
 * the bar first. The map is the one such screen - its badge counts the markers on the panel,
 * which is not answerable until the panel has been measured - and the alternative is drawing the
 * bar twice, once with a wrong number and once over the top of it.
 *
 * A trail is one extra line, which is the only thing about a bar that changes its height. What
 * the title says, and whether there is a badge, do not.
 */
int fb_app_bar_height(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                      size_t trail_count);

/*
 * What a screen says instead of a list when it has nothing to show, under the icon of whatever
 * the list would have held.
 *
 * The icon is drawn large and dim above the words, which is the shape an empty state has
 * everywhere: the screen is blank, so the one thing on it can afford to be the size that says
 * "this is empty on purpose" rather than "this failed to load".
 */
void fb_draw_empty(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   enum mesh_ui_icon icon, const char *text);

/* How tall a hairline is at `scale` - what a caller stacking something under one has to clear.
   Beside the call that draws one because two expressions for one thickness is how a bar ends up
   overlapping the rule above it. */
int fb_rule_height(const struct mesh_ui_backend_fb_state *state, int scale);

/* A hairline separator - under the tab strip, above a detail pane. The role says which of the
   theme's two rule colours it is: MESH_UI_COLOR_RULE for a separator inside the body,
   MESH_UI_COLOR_RULE_STRONG for the one that closes the chrome off. */
void fb_draw_rule(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int scale,
                  enum mesh_ui_color role);

/*
 * A scrolling list of rows, drawn top to bottom.
 *
 * Owns the window arithmetic (via struct mesh_ui_list) and the y cursor, which is the whole of
 * what the eight screen renderers used to repeat. The shape is always:
 *
 *     struct fb_list list = fb_list_begin(layout, count, nav->cursor[SCREEN]);
 *     uint32_t i;
 *     while (fb_list_next(&list, &i)) {
 *         ... build the row ...
 *         fb_list_row(state, &list, i, text, tone);
 *     }
 */
struct fb_list {
    struct mesh_ui_list model;
    int y;    /* next row's baseline */
    int line; /* one step's advance - a body row */
    size_t cols;
    /* The body the list was opened against, for the scroll rail: where it starts and how tall
       it is. Taken from the layout at fb_list_begin*() rather than accumulated as rows are
       drawn, because a rail has to be the length of the *window* whether or not the items
       filled it. */
    int track_y;
    int track_h;
    /*
     * The card ordinal per item, or NULL for a list drawn straight onto the panel. Borrowed for
     * the life of the list, on the same terms as the model's heights - see the card-list note
     * below.
     */
    const uint8_t *cards;
    /* Whether the chrome - the card surfaces, then the scroll rail - has been drawn for this
       list. It is drawn by the first row that draws, not by the screen: see fb_list_chrome() in
       fb_widgets.c. */
    bool chrome_drawn;
};

/* One row per item, filling the body. */
struct fb_list fb_list_begin(const struct fb_layout *layout, uint32_t count, uint32_t cursor);

/* `per_item` rows per item - the conversation list spends two, a name and a preview. */
struct fb_list fb_list_begin_rows(const struct fb_layout *layout, uint32_t count, uint32_t cursor,
                                  uint32_t per_item);

/*
 * ---- a list drawn as a column of cards ----
 *
 * The same scrolling list, with its groups standing on card surfaces instead of on the panel.
 *
 * It exists for the reason fb_card does one screen over: a hundred and twenty label-and-value
 * rows separated by dimmed words is a wall, and nothing in it says that Long name and Short name
 * are one subject while Temperature and Humidity are another. A dimmed heading is a group
 * distinguished from its own rows by *colour alone*, which is the one thing the type scale
 * landed to stop; a fill and an edge say it the way every phone and desktop platform says it.
 *
 * What it is not is fb_draw_card(). That component is declared-then-drawn and measures itself
 * against the body, which is right for the Status tab's fixed column of four and impossible
 * here: this list is longer than the panel by a factor of eight, the window moves a row at a
 * time, and a card is routinely cut by both edges of it at once. So a card here is a *surface
 * behind a run of rows the list already knows how to place* - no second measure, no second
 * clip, and above all no second opinion about how tall a row is. The model stays the authority
 * on every height, exactly as it is for fb_list_begin_heights(), and the cards are painted from
 * the window it settled.
 *
 * A screen declares the grouping the same way it declares the heights: one byte per item,
 * borrowed for the life of the list. Items carrying the same card ordinal *and lying next to
 * each other* are one card; FB_LIST_NO_CARD is an item standing on the bare panel, which is
 * what a list with no grouping at all passes for every row.
 *
 * Three things follow from the surface being painted before the rows rather than by them:
 *
 *   - **The cards are drawn by the first row that draws, not by the screen.** The scroll rail's
 *     rule, for the rail's reason: which card covers which rows is derived entirely from the
 *     model and the array, so a screen has nothing to say about it and a screen that had to
 *     remember the call is a screen that would forget on one list out of nine.
 *   - **A card cut by the window keeps its corners square on the cut end.** A rounded corner
 *     halfway down a scroll is a card claiming to end where the panel merely stopped, and a
 *     reader cannot tell that from a card that really did end there. The cut end keeps its
 *     inset as well as its corners, or the hairline would run across the cut and say it again -
 *     in a straight line this time. fb_fill_round_rect_ends() is what draws it.
 *   - **Rows on a card are drawn against the card's surface**, not against the background, so
 *     the ink they blend their edges into is the colour actually under them. Every list entry
 *     point below takes that from the model, so a screen cannot get it wrong by forgetting.
 *
 * The gap between two cards is the air at the top of a heading row, which is air that was
 * already there: fb_list_subheader() draws at the label scale sat on the bottom of its step
 * precisely so the space the smaller glyphs free becomes the section break. A column of cards
 * spends half of it as the card's own top padding and leaves the other half as the gap - so the
 * grouping costs **no rows at all**, and the nav, the row budget and every count in the
 * ui_nav_nodes suite are untouched by it.
 */

/* An item standing on the panel rather than on a card. The whole array, for a list with no
   grouping - which is every list that passes no array at all. */
#define FB_LIST_NO_CARD 0xFFU

/*
 * Rows that are not all the same height: `heights` is one row count per item, and it is
 * borrowed for the life of the list.
 *
 * The measure is the caller's because the caller is the only thing that knows: whether a row
 * carries a bar under its words is a fact about that row's content, and the screen has already
 * walked its items to build them. What must not happen is the screen measuring one way and the
 * component drawing another, so the *list* is the authority once it has been told - every entry
 * point below advances by the height the model holds for that index, never by what the item it
 * was handed looks like. A row whose height the screen forgot to declare therefore draws short
 * rather than over the row beneath it.
 *
 * mesh_ui_transcript_window() takes the same shape, for the same reason - see include/mesh/ui/
 * layout.h, where the window arithmetic lives and is unit tested.
 */
struct fb_list fb_list_begin_heights(const struct fb_layout *layout, uint32_t count,
                                     uint32_t cursor, const uint8_t *heights);

/*
 * The same, with the items grouped onto card surfaces: `cards` is one ordinal per item and is
 * borrowed for the life of the list, exactly as `heights` is. See the card-list note above.
 *
 * `heights` may be NULL for a list whose rows are all one row tall, and `cards` may be NULL for
 * no grouping - in which case this is fb_list_begin_heights() and nothing is painted.
 */
struct fb_list fb_list_begin_cards(const struct fb_layout *layout, uint32_t count, uint32_t cursor,
                                   const uint8_t *heights, const uint8_t *cards);

/*
 * The colour item `index` is standing on: a card's surface, or the panel's background.
 *
 * Every row entry point below already asks this for itself, so a screen needs it only when it
 * draws something of its own beside a row - and when it does, it must ask rather than assume,
 * because a glyph blended against the wrong ground keeps its shape and gains a halo.
 */
enum mesh_ui_color fb_list_ground(const struct fb_list *list, uint32_t index);

/* An explicit window, for a screen that reserves body rows for something else. */
struct fb_list fb_list_begin_visible(const struct fb_layout *layout, uint32_t count,
                                     uint32_t cursor, uint32_t visible);

bool fb_list_next(struct fb_list *list, uint32_t *index);

/* Rows item `index` occupies, from the list model. What a screen measuring something of its own
   against a row - a divider, a second column - has to advance by. */
uint32_t fb_list_row_height(const struct fb_list *list, uint32_t index);

/*
 * ---- the scroll rail ----
 *
 * A list longer than its window draws a rail in the right-hand gutter: a track the height of
 * the body, with a thumb whose length is the fraction of the list on screen and whose position
 * is how far down it is. Nothing else in the frame says either of those - a screen title that
 * carries a count is answering a different question, and on the Nodes tab a very different one
 * (how much of the radio's NodeDB we hold, which is not a scroll position and is not this).
 * A window that gives no sign there is more of the list is the one thing every list UI on every
 * platform has an answer for.
 *
 * **No screen asks for it.** It is drawn by the first row that draws, from the list model,
 * because a rail is derived entirely from `count`, `first` and `visible` - a screen has nothing
 * to say about it and a screen that had to remember the call is a screen that would forget on
 * one list out of nine. That is the same reasoning the list item's clipping follows: geometry
 * belongs down here, and the screen describes content.
 *
 * It lives in the half-margin *outside* the row fill, so it overlaps nothing: rows clip their
 * text at `xres - margin` and the cursor fill stops at `xres - margin / 2`. It is therefore
 * invisible to every measurement a row already makes, which is why adding it changed no row.
 *
 * It does not animate. The row entry points that draw it take the state immutably - unlike the
 * switch and the meter, whose sliding is the reason they take it mutably - and a rail that
 * eased would need a keyed slot per list. A thumb that jumps a row when the cursor moves a row
 * is not the thing motion was for.
 */

/* Draws the row - highlighted when `index` is the cursor - and advances. */
void fb_list_row(const struct mesh_ui_backend_fb_state *state, struct fb_list *list, uint32_t index,
                 const char *text, enum mesh_ui_tone tone);

/* Same, taking the line builder directly, which is how most rows are assembled. */
void fb_list_row_line(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                      uint32_t index, struct mesh_ui_line *line, enum mesh_ui_tone tone);

/*
 * A section header inside a list: the row that names the group under it.
 *
 * It was a dimmed row of body text, which is a heading distinguished from the rows it heads by
 * colour alone - the one thing the type scale landed to stop, and the half of it that could not
 * be done at the time because the list model counted rows of one height. It is drawn at
 * MESH_UI_TYPE_LABEL and sat on the *bottom* of its step, so the space the smaller glyphs free
 * becomes air above it: the gap is what separates one group from the last one's rows, and it
 * costs nothing because the row was already that tall.
 *
 * Still a row of the list, and still highlightable - the cursor walks onto these on both
 * screens that draw them - so the fill is the step, whatever size the words in it are.
 *
 * On a list drawn as a column of cards this is the card's heading, and the air above it is what
 * separates one card from the last - which is why the grouping costs no rows. Its leading slot
 * carries a symbol there and nowhere else: a heading over rows already carrying icons of their
 * own would be a second thing saying what the words under it say, but a *card* heading is the
 * one cell the eye finds when it is looking for Signal rather than Identity, which is the same
 * argument struct fb_card's own icon is there for.
 */
void fb_list_subheader(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                       uint32_t index, const char *text);

/*
 * ---- the note row ----
 *
 * A paragraph as a list row: a heading line at the label scale and the sentences under it,
 * wrapped across the list's whole width. What the help screen is made of, and the one row shape
 * here whose height is not a property of the row's *kind* but of the words in it.
 *
 * It exists because the two shapes that already wrap were both the wrong one. fb_card_note()
 * stops at FB_CARD_NOTE_LINES, which is right for a sentence the radio wrote into a card of
 * other rows and wrong for the only content on a screen; and a list item's supporting line is
 * one line, elided, which is the shape for a reminder rather than for an explanation.
 *
 * fb_list_note_steps() is the measure and fb_list_note() is the draw, and the screen must ask
 * the first before it opens the list - a note whose height the model was not told about draws
 * over the row beneath it. Two calls rather than one for the reason the settings slider has
 * two: the model is the authority on every height, and it can only be if it is told before the
 * first row is placed.
 *
 * `heading` may be NULL for a paragraph that names nothing, which is what the topic's own
 * opening note is.
 */
uint32_t fb_list_note_steps(const struct mesh_ui_backend_fb_state *state, const char *heading,
                            const char *body);

void fb_list_note(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                  uint32_t index, const char *heading, const char *body);

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
       one of a set, a radio for one alternative among a column of them. CHECKBOX has no caller
       yet and the comment above enum fb_selection_shape says why. */
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

/* What sits at the row's leading edge. */
enum fb_leading_kind {
    FB_LEADING_NONE = 0,
    /* A tinted disc with one or two cells - or one icon - in it. What lets the eye find a row
       by colour and two letters long before it has read a name. */
    FB_LEADING_AVATAR,
    /*
     * One icon in the gutter before the words: the star on a pinned node, the broken link on a
     * node the radio has forgotten, the bluetooth rune on a device row.
     *
     * The slot is reserved whenever the kind is set, `icon` of MESH_UI_ICON_NONE included -
     * which is the point. A list where some rows have an icon and some do not is a list whose
     * text starts in two different columns, so a screen declares the slot for the whole list
     * and the rows with nothing to say leave it empty.
     */
    FB_LEADING_ICON,
};

struct fb_leading {
    enum fb_leading_kind kind;
    const char *label;      /* AVATAR: initials */
    enum mesh_ui_icon icon; /* ICON, and AVATAR when a disc holds a symbol rather than letters */
    uint32_t tint;          /* AVATAR: seeds the disc's colour through the theme's avatar palette */
    /* AVATAR: a stated fill instead of a tint - the primary for "all traffic", the error
       colour for a row armed to be deleted. MESH_UI_COLOR_COUNT means "use the tint". */
    enum mesh_ui_color role;
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
 * The same heading, indented to a list that declares a leading slot.
 *
 * `leading` is the row's gutter and, on a card, its symbol. The gutter half is why it exists at
 * all: a heading over rows whose words begin an icon-box in would otherwise name a column
 * nothing is in.
 *
 * Whether the slot is *filled* is the card distinction. On a flat list it stays empty, because a
 * group there is a break between runs of rows and a symbol on it would repeat the words beside
 * it - the icons on such a list say what each row is about, and a group has no single answer to
 * that. On a column of cards the heading is the card's own, and there the symbol is what the eye
 * finds first when it is looking for Signal rather than Identity: the argument struct fb_card
 * states for the icon beside *its* heading, which this is. The list knows which it is drawing,
 * so a caller passes the icon either way and nothing has to decide twice.
 *
 * fb_list_subheader() is this with no slot, which is every list that has no icons in it.
 */
void fb_list_subheader_icon(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                            uint32_t index, const char *text, struct fb_leading leading);

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
       src/ui/delivery.c answers with. MESH_UI_ICON_NONE on anything inbound, and on a broadcast
       that went out without want_ack - there is nothing to be waiting for. */
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

/* The label column width for a body this wide - narrow scales give the value more room, at the
   width the theme calls narrow. `preferred` of 0 takes the theme's own. */
size_t fb_field_label_cols(const struct mesh_ui_backend_fb_state *state,
                           const struct fb_layout *layout, size_t preferred);

/* ---- cards --------------------------------------------------------------------------------
 *
 * A card: a titled panel that groups rows which belong together.
 *
 * Every screen that is not a list is a column of label/value lines on the bare ground, and a
 * column of eighteen of them is a wall - nothing in it says that Transport, Radio and Sync are
 * one subject and Packets and Dropped are another. A card says it with a fill and a heading,
 * which is what the phone and desktop platforms settled on for the same reason.
 *
 * It is *declared, then drawn*, unlike every other component here, and that is forced by the
 * framebuffer: a card's fill has to go down before its text or it paints over it, and its
 * height is not known until the last row is in. So a screen fills a struct and hands it over:
 *
 *     struct fb_card card;
 *     fb_card_begin(&card, MESH_STR_STATUS_CARD_LINK, MESH_UI_TONE_PRIMARY);
 *     fb_card_row_text(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_TRANSPORT, status);
 *     if (connected) {
 *         fb_card_row_text(&card, MESH_UI_TONE_SUCCESS, MESH_STR_STATUS_LABEL_RADIO, name);
 *     }
 *     fb_draw_card(state, layout, &y, &card);
 *
 * which is worth more than the drawing it saves: a conditional row is an `if` around one call
 * rather than a branch that has to remember to advance a y cursor by the right amount.
 *
 * The card owns its own geometry - the inset, the corner radius, the label column, where the
 * next card starts - and it owns the footer. A screen cannot lay a card over the footer: rows
 * that do not fit are dropped and the card says how many, rather than the screen re-deriving
 * the "does another row fit" test that was written out by hand on every dense screen.
 *
 * Colours are the theme's: the fill is a surface role picked by the card's variant, which every
 * theme already owes body text 4.5:1 and the four card tones 3:1, and the edge is
 * MESH_UI_COLOR_OUTLINE, which has to be visible against all of them. Its corners are
 * MESH_UI_SHAPE_MD, so how round a card is belongs to the theme like everything else about it.
 * The heading takes the card's own tone, so a card reports the state of what it holds - the
 * Link card goes bad when the radio is gone - without a second cue to invent.
 */

/*
 * How much weight a card is asking for.
 *
 * A column of cards drawn at one weight has no shape: Link and Mesh sat side by side on the
 * Status tab with nothing to say which one to read first, which is the same complaint the
 * button variants answer for a screen full of controls. Material has the same three and uses
 * them for the same thing.
 *
 * Here they are three *surface tiers* rather than three shadows. The Brick's display engine
 * composites fb0 against its own background layer, so there is no alpha and nothing to cast a
 * shadow into - the distance a card is off the ground is carried by its fill alone, which is
 * how Material's tonal elevation works and why it survives a light palette as well as a dark
 * one (see the tier comment in include/mesh/ui/theme.h).
 *
 * All three keep the hairline. The edge is not decoration on a theme whose surface is a step
 * off the ground - it is the whole of what says a card is there - and an outlined card, whose
 * fill *is* the ground, would otherwise not be a card at all.
 */
enum fb_card_variant {
    /* A panel on the ground: MESH_UI_COLOR_SURFACE. The ordinary weight, and the zero value. */
    FB_CARD_FILLED = 0,
    /* A step further up: MESH_UI_COLOR_SURFACE_HIGH. The card to read first. */
    FB_CARD_ELEVATED,
    /* The ground itself, held by its edge. The card that is on screen because the set would be
       incomplete without it, not because it has something to say. */
    FB_CARD_OUTLINED,
};

#define FB_CARD_ROWS_MAX 12U
#define FB_CARD_LABEL_MAX 24U
/* Wide enough for the longest thing a row carries whole, which is a radio notice
   (MESH_UI_RADIO_NOTICE_TEXT_MAX). A value longer than this is clipped on a cell boundary. */
#define FB_CARD_VALUE_MAX 132U
/* A note is a sentence the radio wrote, not a value, and three lines is where it stops being
   worth the rows it costs on a 3.2" panel. */
#define FB_CARD_NOTE_LINES 3U

enum fb_card_row_kind {
    FB_CARD_ROW_FIELD = 0, /* a label column and a value, as the field rows have */
    FB_CARD_ROW_NOTE,      /* a wrapped paragraph across the card's full width, no label */
    /* A label column and a meter across the rest of the row, in place of the value. One line
       like a field row, so a card with one costs no more room and the clip arithmetic above is
       unchanged - a bar is thinner than the text it sits among, not taller. */
    FB_CARD_ROW_METER,
    /* And the same shape again for a whole divided into its parts. One line, like the meter and
       for the meter's reason: a composition is read as lengths, and lengths are as thin as the
       theme's bar. It is the trend that needs two rows, because a shape needs a second
       dimension and a length does not. */
    FB_CARD_ROW_PROPORTION,
};

struct fb_card_row {
    enum fb_card_row_kind kind;
    enum mesh_ui_tone tone;
    char label[FB_CARD_LABEL_MAX];
    char value[FB_CARD_VALUE_MAX];
    /* METER: what the bar reads, the domain it reads it on, and what it is keyed on. Held by
       value rather than by pointer because a card is built, handed over and drawn - there is no
       caller-owned control to point at, unlike a list row's switch. The band goes the same way
       and for the same reason, with `meter_banded` standing in for the NULL a pointer would
       have carried. */
    int32_t meter_value;
    struct mesh_ui_scale meter_scale;
    struct mesh_ui_band meter_band;
    bool meter_banded;
    uint32_t meter_id;
    /* PROPORTION: the parts, held by value on the terms the band and the polyline are - a card
       is built, handed over and drawn, so a row pointing at a caller's array would be a card
       that only works while the frame that built it is still on the stack. */
    uint32_t parts[MESH_UI_PROPORTION_PARTS];
    uint32_t part_count;
};

/*
 * A verb the card offers, drawn as a button against the far edge of its heading's line.
 *
 * "Radio actions" was a settings row that opened a screen because a card could not offer a
 * verb, and disconnecting meant walking to the Devices tab to press X on a card that was
 * already naming the radio. A card that reports on something is the place to act on it.
 *
 * The heading's line rather than a row under the content, which is where a phone puts card
 * actions and where this was first written. That cost a row per card carrying a verb, and the
 * screen it cost them on is the one that can outgrow the panel - so the two rows the Status
 * tab lost were a refused packet and a reboot count, which are the rows that card exists to
 * show. A heading is three or four cells of an otherwise empty line; the verbs go in the rest
 * of it and cost nothing.
 *
 * They are drawn at the chrome scale for the same reason the action bar's verbs are: a verb is
 * read beside the content rather than as part of it. A body-scale button here would also have
 * grown the header line by the difference and given a third of a row back.
 *
 * A button carries a word and no icon. The verbs the Status tab spends are the same ones the
 * action bar names for the same press, and the bar draws a keycap beside each - a symbol here
 * as well would be a third thing on the frame saying one press.
 */
struct fb_card_action {
    char label[FB_CARD_LABEL_MAX];
    bool selected; /* the screen's cursor is on this button */
};

/* What fits beside a heading at the largest glyph scale with room for the words. A card wanting
   a fourth verb is a card that wants a screen. */
#define FB_CARD_ACTIONS_MAX 3U

struct fb_card {
    enum fb_card_variant variant;
    char heading[FB_CARD_LABEL_MAX];
    /* Beside the heading: what the card is about, said in one cell. A column of cards is a
       column of headings otherwise, and the icon is what the eye finds first when it is looking
       for the radio rather than the mesh. */
    enum mesh_ui_icon icon;
    enum mesh_ui_tone tone; /* the heading's, and so the card's own report on itself */
    /* Rows offered past the last are dropped. The cap is well above what any screen here fills
       - the densest is the Status tab's Radio card at six - so reaching it means a screen has
       outgrown one card rather than that the panel ran out, and wants two. */
    struct fb_card_row rows[FB_CARD_ROWS_MAX];
    uint32_t count;
    /* The verbs. They are on the header line, so they are never among the rows dropped to make
       a card fit - a card that shed its buttons would leave the action bar naming a press with
       nothing behind it. A card whose heading and verbs together overrun its width keeps the
       verbs and cuts the heading, for the same reason. */
    struct fb_card_action actions[FB_CARD_ACTIONS_MAX];
    uint32_t action_count;
};

/* Starts a card. `heading` of MESH_STR_NONE is a card with no heading - a panel, not a
   section - and takes MESH_UI_ICON_NONE with it. Always call this first: it is what clears the
   row list. The variant is stated here rather than defaulted, because which of three weights a
   card is asking for is a decision about the column it sits in and not a property of the card
   on its own. */
void fb_card_begin(struct fb_card *card, enum fb_card_variant variant, enum mesh_ui_icon icon,
                   enum mesh_str_id heading, enum mesh_ui_tone tone);

/* A label and a value formatted from the catalog, which is the shape most rows have. */
void fb_card_row(struct fb_card *card, enum mesh_ui_tone tone, enum mesh_str_id label,
                 enum mesh_str_id value, ...);

/* The same row for a value that is already text - a device name, an age, a percentage a caller
   has formatted. It exists so no "%s" pass-through ends up in the catalog, where it would be a
   line for a translator to wonder about. Same split as fb_draw_status_row/_text. */
void fb_card_row_text(struct fb_card *card, enum mesh_ui_tone tone, enum mesh_str_id label,
                      const char *value);

/* A sentence, wrapped across the card's whole width with no label column. What the radio said
   about itself goes here: a firmware sentence in the value gutter is three words and a cut. */
void fb_card_note(struct fb_card *card, enum mesh_ui_tone tone, const char *text);

/*
 * A bar: the row for a number whose *level* is the point.
 *
 * `value` is a reading on `scale` - a zeroed scale meaning it is already permille - and `id`
 * keys the animation, on the same terms as a switch's: stable while the row is on screen,
 * unique within the frame, 0 for a bar that never moves of its own accord.
 *
 * `band` may be NULL for a plain bar. When it is not, the bar marks the boundaries on its track
 * and takes its fill from where the reading falls, resting in `tone` - so a card that colours
 * its heading by the same band is stating one threshold rather than agreeing with itself by
 * hand. It is copied, not retained.
 *
 * `label` of MESH_STR_NONE gives the bar the card's whole content width instead of a label
 * column, and that is the shape to reach for when the bar is *about the row above it* - which
 * is what the airtime pair on the Status card is. The words there already say what the number
 * is and how large it is; a label on the bar would be the third time, and it would cost the
 * track the third of its length that makes a fill readable as a proportion. A label is for a
 * bar that stands alone in a card of unrelated rows.
 *
 * It never carries the figure either way. A row that drew both would spend the card's width
 * saying one thing twice.
 */
void fb_card_meter(struct fb_card *card, enum mesh_ui_tone tone, enum mesh_str_id label,
                   int32_t value, struct mesh_ui_scale scale, const struct mesh_ui_band *band,
                   uint32_t id);

/*
 * A divided bar: the row for what a reading is *made of*.
 *
 * `values` are the parts in the order they are drawn, and `tone` colours the label rather than
 * the bar - the parts take the theme's series palette, because which part a slice is is not a
 * judgement about it. See struct fb_proportion for what a caller is promising by calling this:
 * that the parts are disjoint, and that whatever row names them names them in this order.
 *
 * Fewer than two parts, or parts summing to zero, adds no row at all - the sparkline's rule, for
 * the sparkline's reason. A bar with nothing in it says the radio heard nothing; a radio that
 * has not reported yet has said nothing, and those are different.
 *
 * `label` of MESH_STR_NONE gives the bar the card's whole content width, and that is the shape
 * to reach for: this row goes under the row that names its parts, exactly as the airtime meter
 * goes under the airtime figures, and a label here would be naming the subject a third time.
 */
void fb_card_proportion(struct fb_card *card, enum mesh_ui_tone tone, enum mesh_str_id label,
                        const uint32_t *values, uint32_t count);

/*
 * A verb, as a button on the card's heading line. Declared left to right: the first call is the
 * leftmost button, which is also the first the screen cursor reaches.
 *
 * `selected` says the cursor is on it, which is also what makes the card itself read as
 * focused - see fb_draw_card().
 *
 * A card carries the whole verb and nothing about the press: which button runs it is the action
 * bar's business, and a keycap drawn twice on one frame is a screen disagreeing with itself.
 */
void fb_card_action(struct fb_card *card, enum mesh_str_id label, bool selected);

/* Whether anything was added. A card with no rows is not drawn, so a screen can build one
   unconditionally and let it disappear when the radio has reported nothing. A card with verbs
   and no rows is still empty: an action row alone is a button strip, not a card. */
bool fb_card_is_empty(const struct fb_card *card);

/*
 * The room a card needs to say *everything* it holds: its heading, every row, and its padding.
 * The other half of the reservation pair below - and on the same terms, so a screen can hand
 * either to fb_draw_card_reserving() and get what it asked for. Neither carries the gap between
 * the two cards, because that gap belongs to the card doing the reserving.
 *
 * Which of the two a screen reserves is an editorial decision and is allowed to be a *reading*
 * rather than a constant. The Status tab is the worked example: its Radio card is a heading over
 * a battery figure while the radio is well and five rows of explanation when it is not, so the
 * card above it promises the minimum in the first case and the whole in the second. See
 * fb_render_status().
 */
int fb_card_height(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   const struct fb_card *card);

/*
 * Draws the card with its top edge at `*y` and advances `*y` past it.
 *
 * Rows that would fall past the body's bottom are dropped from the end and the card shrinks to
 * what is left, so a screen may hand over more cards than the panel holds and get the ones that
 * fit. A *note* is the exception: when it is the row that did not fit, it keeps as many of its
 * lines as the leftover room takes, because a note is a sentence explaining something no other
 * row can and half of it beats none of it. A field row is never halved - a label and half a
 * value is not half a fact - and nothing is drawn below a clipped note, since a truncated
 * paragraph with rows under it reads as a complete one.
 *
 * Returns false when not even the heading and one row - or one line of a note - fit, in which
 * case nothing is drawn and `*y` is untouched. That is also the answer for every card after it,
 * so a screen can stop.
 *
 * A card holding the selected action draws its edge in the primary instead of in
 * MESH_UI_COLOR_OUTLINE, and draws it thicker: that is the focus ring, and it is the one cue
 * here that is not a state layer. A layer mixed into a fill this large is a change nobody
 * notices from across a table, and every tone written on the card would owe the result its own
 * contrast contract; the accent edge is the indicator Material uses for focus, it is read at a
 * glance, and PRIMARY already owes both grounds 3:1.
 */
/*
 * The least a card can be drawn as and still be one: its heading, its first row and its padding.
 * What a screen reserves for a card that must not disappear but has nothing urgent to say.
 *
 * The first *row* rather than a line, because a card is refused outright at the point it has
 * nothing but a heading - so this is the smallest height that actually draws something.
 */
int fb_card_min_height(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                       const struct fb_card *card);

/*
 * fb_draw_card(), keeping `reserve` pixels of the body free below this card.
 *
 * Why a card needs to be told this at all. A column of cards is drawn in order and each takes
 * what it wants, so the *last* card pays for everything above it - and paying means not being
 * drawn, because fb_draw_card() refuses a card it cannot fit rather than drawing an empty box.
 * Losing a card is worse than losing a row, and not only because it is more content: a card
 * carries **verbs**, and which verbs a screen offers is a table (src/ui/status.c) with no idea
 * how tall anything came out. The cursor therefore keeps walking onto a button that is not on
 * the frame - which is exactly the failure "a card that can end up with no rows must not be
 * given a verb" names, reached from the layout side instead of the row-count side.
 *
 * A reservation turns it around: the card that can afford to drop a row drops one, and the card
 * that would have vanished survives. It is also the right order editorially, because rows are
 * clipped from the end and a screen declares its least important rows last.
 *
 * Pair it with fb_card_min_height() of whatever comes next rather than with that card's full
 * height: the promise worth making is *that the card exists*, and a card given more room than
 * its minimum will use it.
 *
 * A reservation that cannot be afforded is dropped rather than honoured - two cards missing is
 * not an improvement on one - so this never draws less than fb_draw_card() would have.
 */
bool fb_draw_card_reserving(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                            int *y, const struct fb_card *card, int reserve);

bool fb_draw_card(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout, int *y,
                  const struct fb_card *card);

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

/* ---- the dialog -------------------------------------------------------------------------------
 *
 * A raised panel that asks one question and offers two answers.
 *
 * The confirmation screen was a title, four lines of wrapped text on the bare ground, and the
 * two answers as ordinary list rows under them - which is to say it looked exactly like every
 * other list in the app, at the one moment the app is asking rather than showing. A dialog is
 * the shape that difference has everywhere else: the question lifted onto its own surface, and
 * the answers as *buttons* rather than as rows.
 *
 * It fills the body rather than floating over it, and that is deliberate rather than a
 * shortcut. A dialog elsewhere dims what is behind it with a scrim, and a scrim is alpha; the
 * Brick's display engine composites fb0 against its own layer, so there is nothing to blend
 * against and no scrim to draw. What stands in for it is that nothing else is on screen -
 * fb_render_confirm() is a screen, not an overlay - so there is nothing left to dim.
 *
 * The action row is the reason the two answers move off the list. Both are one press away
 * whichever is under the cursor, so the pair reads as a choice rather than as a menu; and the
 * accept is a tonal button while the cancel shows no fill at all, which is how every dialog
 * says which answer it is proposing without the words having to.
 */

struct fb_dialog {
    /* Over the headline, drawn large in the primary - or in the error colour when
       `destructive`. MESH_UI_ICON_NONE for none, and the panel closes up the room it would
       have taken. */
    enum mesh_ui_icon icon;
    const char *headline;
    /* The supporting paragraph, wrapped across the panel. "" for a question that needs none. */
    const char *text;
    const char *accept;
    const char *cancel;
    /* 0 is accept, 1 is cancel - the same index nav.confirm_cursor carries, which every
       direction toggles. */
    uint32_t cursor;
    /* What it goes through with cannot be undone: the whole dialog switches from the primary
       family to the error one, so the icon, the headline and the accept button's fill all
       change together rather than each being decided separately. */
    bool destructive;
};

/* Draws the dialog into the body. It owns the whole of it, so there is no `y` to advance. */
void fb_draw_dialog(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                    const struct fb_dialog *dialog);

/* ---- the snackbar -------------------------------------------------------------------------
 *
 * The transient notice: "Sent to BRVO", "Not connected", "Rebooting".
 *
 * It used to be the footer's second line, in the accent, sharing that row with the link
 * summary - which meant the two competed for one line and the notice won, so for four seconds
 * after every action the frame stopped saying whether there was a radio attached. Neither is
 * secondary to the other; they were only sharing a row because a row was what a notice had.
 *
 * So it is a container of its own, over the body rather than in the chrome, and it arrives by
 * sliding up from below the panel and leaves the same way. That is the whole point of the
 * shape: a notice that appears in place has to be *noticed* to be read, and on a handheld the
 * eye is usually somewhere else at the moment it appears. Movement is what gets it back.
 *
 * Everything about it that cannot come from the snapshot - where it has slid to, what it says
 * while it slides back out after the store has forgotten it - is remembered on the state, next
 * to the animation table and for the same reason. See struct mesh_ui_backend_fb_state.
 *
 * Drawn last of everything on the frame, because it is over the UI rather than in it.
 */
struct fb_snackbar {
    /* The notice; NULL or empty means there is none, which is also what makes one already on
       screen start sliding back out. */
    const char *text;
    /*
     * When the store means to take it away, which the widget uses as the notice's *identity*
     * rather than as a deadline - expiry is the nav's business and it has already done it by
     * the time the text arrives empty.
     *
     * It is here because two notices can read the same: pressing send twice with no radio
     * attached raises "Not connected" twice, and the second one has to arrive rather than sit
     * there looking like the first never left. A deadline moves every time a notice is raised,
     * so it tells them apart when the words cannot.
     */
    uint64_t until_ms;
};

/*
 * Draws the notice, advancing it towards its resting place - or off the bottom of the panel
 * when there is none left to show. Nothing is drawn once it has gone.
 *
 * Mutable state, like every animated component here: the position it is coming from and the
 * words it is still carrying both live on the state.
 */
void fb_draw_snackbar(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                      const struct fb_snackbar *bar);

/* "Messages (12)", or "Messages (12, +40 older)" when a ring has dropped some. */
void fb_title_count(char *out, size_t out_len, const char *name, uint32_t count, uint32_t dropped);

#endif /* MESH_UI_BACKENDS_FB_WIDGETS_H */
