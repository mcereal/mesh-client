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
    bool on;
    bool selected; /* the row under it carries the cursor fill */
    bool dim;      /* it reports a state rather than offering one: drawn muted */
};

/* The size a switch wants at `scale`. Both out params may be NULL. */
void fb_switch_size(const struct mesh_ui_backend_fb_state *state, int scale, int *w, int *h);

/* Draws it, advancing the knob towards its target. Needs the mutable state: the animation it
   is stepping lives there. */
void fb_draw_switch(struct mesh_ui_backend_fb_state *state, const struct fb_switch *sw);

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
    int32_t value; /* DETERMINATE: permille, 0..MESH_UI_ANIM_ONE, clamped */
    /* The fill. ACCENT, GOOD or BAD - the three mesh_ui_theme_validate() holds against
       MESH_UI_COLOR_METER_TRACK - and anything else is drawn in the accent. */
    enum mesh_ui_tone tone;
    bool selected; /* the row under it carries the cursor fill */
};

/* The height a meter wants at `scale`, in pixels. From the theme's metrics, so a bar keeps its
   proportion to the text beside it when a theme changes the glyph scale. */
int fb_meter_thickness(const struct mesh_ui_backend_fb_state *state, int scale);

/* Draws it, advancing the fill towards its target - or the pill along its loop. Needs the
   mutable state for the same reason the switch does. */
void fb_draw_meter(struct mesh_ui_backend_fb_state *state, const struct fb_meter *meter);

/*
 * The accented heading a screen opens with. Consumes the body row it occupies, so a screen
 * calls this and then lays its list out against the layout it hands back.
 */
void fb_draw_title(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                   const char *title);

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
    int line; /* row advance */
    size_t cols;
};

/* One row per item, filling the body. */
struct fb_list fb_list_begin(const struct fb_layout *layout, uint32_t count, uint32_t cursor);

/* `per_item` rows per item - the conversation list spends two, a name and a preview. */
struct fb_list fb_list_begin_rows(const struct fb_layout *layout, uint32_t count, uint32_t cursor,
                                  uint32_t per_item);

/* An explicit window, for a screen that reserves body rows for something else. */
struct fb_list fb_list_begin_visible(const struct fb_layout *layout, uint32_t count,
                                     uint32_t cursor, uint32_t visible);

bool fb_list_next(struct fb_list *list, uint32_t *index);

/* Draws the row - highlighted when `index` is the cursor - and advances. */
void fb_list_row(const struct mesh_ui_backend_fb_state *state, struct fb_list *list, uint32_t index,
                 const char *text, enum mesh_ui_tone tone);

/* Same, taking the line builder directly, which is how most rows are assembled. */
void fb_list_row_line(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                      uint32_t index, struct mesh_ui_line *line, enum mesh_ui_tone tone);

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
};

struct fb_trailing {
    enum fb_trailing_kind kind;
    const char *text;       /* TEXT and BADGE */
    enum mesh_ui_icon icon; /* ICON */
    struct fb_switch *sw;   /* SWITCH. Its rect is filled in by the row: where the value column
                               ends is the row's business, not the caller's. */
    struct fb_meter *meter; /* METER. Its rect is filled in by the row, as the switch's is. */
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
    /* AVATAR: a stated fill instead of a tint - the accent for "all traffic", the bad tone for a
       row armed to be deleted. MESH_UI_COLOR_COUNT means "use the tint". */
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
     */
    enum mesh_ui_icon marker_icon;
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

    /* A bar down the leading edge in the accent when the cursor is on the row. Not decoration:
       a fill one step off the ground is not by itself findable on a small panel in sunlight,
       and gives a colour-blind eye nothing at all. */
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
 * A chat bubble: the component the thread screen is made of.
 *
 * A bubble sizes itself to its own text - never to the panel - and sits against the edge its
 * direction names, which is the whole of what makes a transcript readable at a glance without
 * reading a single word of it. Everything optional is omitted rather than blanked, so a run of
 * messages from one sender stacks with the name said once.
 *
 * The measure and the draw share one wrap walk (struct mesh_ui_wrap), so the rows a bubble
 * reserves and the rows it paints cannot disagree - which they must not, because the transcript
 * places the next bubble from the count this one reported.
 */
struct fb_bubble {
    const char *separator; /* dim centred label above the bubble ("Today", "14:05"); "" for none */
    const char *name;      /* sender line inside the bubble; "" when it repeats the one above */
    const char *text;      /* the message */
    const char *meta;      /* clock and delivery state, tucked onto the last line when it fits */
    bool outbound;         /* ours: drawn against the right edge */
    bool selected;         /* the cursor is on it */
    bool failed;           /* the radio said it did not get there */
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

/* A dim centred label with a hairline either side, filling one body row. What separates one
   day - or one long silence - from the next. */
void fb_draw_separator(const struct mesh_ui_backend_fb_state *state, int y, const char *label);

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
 *     fb_card_begin(&card, MESH_STR_STATUS_CARD_LINK, MESH_UI_TONE_ACCENT);
 *     fb_card_row_text(&card, MESH_UI_TONE_NORMAL, MESH_STR_STATUS_LABEL_TRANSPORT, status);
 *     if (connected) {
 *         fb_card_row_text(&card, MESH_UI_TONE_GOOD, MESH_STR_STATUS_LABEL_RADIO, name);
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
 * Colours are the theme's: the fill is MESH_UI_COLOR_SURFACE, which every theme already owes
 * body text 4.5:1 and the four card tones 3:1, and the edge is MESH_UI_COLOR_OUTLINE, which has
 * to be visible against both. Its corners are MESH_UI_SHAPE_MD, so how round a card is belongs
 * to the theme like everything else about it. The heading takes the card's own tone, so a card
 * reports the state of what it holds - the Link card goes bad when the radio is gone - without
 * a second cue to invent.
 */

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
};

struct fb_card_row {
    enum fb_card_row_kind kind;
    enum mesh_ui_tone tone;
    char label[FB_CARD_LABEL_MAX];
    char value[FB_CARD_VALUE_MAX];
    /* METER: what the bar reads and what it is keyed on. Held by value rather than by pointer
       because a card is built, handed over and drawn - there is no caller-owned control to
       point at, unlike a list row's switch. */
    int32_t meter_value;
    uint32_t meter_id;
};

struct fb_card {
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
};

/* Starts a card. `heading` of MESH_STR_NONE is a card with no heading - a panel, not a
   section - and takes MESH_UI_ICON_NONE with it. Always call this first: it is what clears the
   row list. */
void fb_card_begin(struct fb_card *card, enum mesh_ui_icon icon, enum mesh_str_id heading,
                   enum mesh_ui_tone tone);

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
 * `permille` is 0..MESH_UI_ANIM_ONE and `id` keys the animation, on the same terms as a
 * switch's - stable while the row is on screen, unique within the frame, 0 for a bar that never
 * moves of its own accord.
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
                   int32_t permille, uint32_t id);

/* Whether anything was added. A card with no rows is not drawn, so a screen can build one
   unconditionally and let it disappear when the radio has reported nothing. */
bool fb_card_is_empty(const struct fb_card *card);

/* Pixels the card occupies, the gap to the next card included. What fb_draw_card() measures
   with; exposed because a screen laying cards out against something else needs the same
   answer, and a second way of measuring one is how the two come to disagree. */
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
 */
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
 * accept is a filled button while the cancel shows no fill at all, which is how every dialog
 * says which answer it is proposing without the words having to.
 */

struct fb_dialog {
    /* Over the headline, drawn large in the accent - or in the bad tone when `destructive`.
       MESH_UI_ICON_NONE for none, and the panel closes up the room it would have taken. */
    enum mesh_ui_icon icon;
    const char *headline;
    /* The supporting paragraph, wrapped across the panel. "" for a question that needs none. */
    const char *text;
    const char *accept;
    const char *cancel;
    /* 0 is accept, 1 is cancel - the same index nav.confirm_cursor carries, which every
       direction toggles. */
    uint32_t cursor;
    /* What it goes through with cannot be undone: the icon and the accept button take the bad
       tone instead of the accent. */
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
