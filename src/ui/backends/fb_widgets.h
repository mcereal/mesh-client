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
 * A pressable cell with a centred label.
 *
 * The on-screen keyboard's character keys and its action row are both this, and so is a tab
 * once it is given a rect. `filled` is what separates a key that only shows itself when the
 * cursor is on it from one that is always visibly a button.
 */
struct fb_button {
    struct fb_rect rect;
    const char *label;
    bool selected; /* the cursor is on it */
    bool filled;   /* keep a resting fill when it is not selected */
    /* Label tone for a button with no fill at all. A filled one - resting or selected - draws
       its label in MESH_UI_COLOR_TEXT_ON_SEL instead, because that is the pair the theme is
       validated on; a tone chosen against the ground says nothing about a fill over it. */
    enum mesh_ui_tone idle_tone;
    int scale; /* glyph multiplier for the label */
};

void fb_draw_button(const struct mesh_ui_backend_fb_state *state, const struct fb_button *button);

/*
 * A pill sized to its own label, laid out left to right. Returns the x the next chip starts
 * at, so a strip of them is a loop with no arithmetic in it.
 */
int fb_draw_chip(const struct mesh_ui_backend_fb_state *state, int x, int y, const char *label,
                 bool active, int scale);

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

/*
 * The accented heading a screen opens with. Consumes the body row it occupies, so a screen
 * calls this and then lays its list out against the layout it hands back.
 */
void fb_draw_title(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                   const char *title);

/* What a screen says instead of a list when it has nothing to show. */
void fb_draw_empty(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   const char *text);

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

/*
 * The same row with a filled count badge flush against the right edge - an unread count, said
 * the way every messenger says it. The line is clipped to leave the badge room rather than
 * drawn under it. `badge` of NULL or "" draws the plain row.
 */
void fb_list_row_line_badge(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                            uint32_t index, struct mesh_ui_line *line, enum mesh_ui_tone tone,
                            const char *badge);

/*
 * A continuation line under the row just drawn: never highlighted and never the cursor,
 * because it is part of the item above it rather than something to select.
 */
void fb_list_sub_row(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                     const char *text, enum mesh_ui_tone tone);

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
    const char *avatar;    /* one or two cells inside the disc: initials, "#", "+" */
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

/* Draws one conversation into the next two rows of `list` and advances past them. */
void fb_draw_conversation(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
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

/*
 * A label column and a value, which is the shape both the Settings rows and the node detail
 * rows have. `marker` is the "> " / "* " gutter that says a row is editable or edited; pass
 * "" for a plain row.
 */
void fb_list_field_row(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                       uint32_t index, const char *label, size_t label_cols, const char *marker,
                       const char *value, enum mesh_ui_tone tone);

/*
 * The same row with a switch where the value would be, flush against the right edge.
 *
 * The row is clipped to leave the control its room rather than drawn under it, exactly as the
 * badge row is. Everything a switch needs beyond the row itself - identity, the two states -
 * comes in through `sw`; its rect is filled in here, because where the value column ends is
 * the row's business and not the caller's.
 */
void fb_list_field_row_switch(struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                              uint32_t index, const char *label, size_t label_cols,
                              const char *marker, enum mesh_ui_tone tone, struct fb_switch *sw);

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
 * body text 4.5:1 and the four card tones 3:1, and the edge is MESH_UI_COLOR_RULE, which has to
 * be visible against both. The heading takes the card's own tone, so a card reports the state
 * of what it holds - the Link card goes bad when the radio is gone - without a second cue to
 * invent.
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
};

struct fb_card_row {
    enum fb_card_row_kind kind;
    enum mesh_ui_tone tone;
    char label[FB_CARD_LABEL_MAX];
    char value[FB_CARD_VALUE_MAX];
};

struct fb_card {
    char heading[FB_CARD_LABEL_MAX];
    enum mesh_ui_tone tone; /* the heading's, and so the card's own report on itself */
    /* Rows offered past the last are dropped. The cap is well above what any screen here fills
       - the densest is the Status tab's Radio card at six - so reaching it means a screen has
       outgrown one card rather than that the panel ran out, and wants two. */
    struct fb_card_row rows[FB_CARD_ROWS_MAX];
    uint32_t count;
};

/* Starts a card. `heading` of MESH_STR_NONE is a card with no heading - a panel, not a
   section. Always call this first: it is what clears the row list. */
void fb_card_begin(struct fb_card *card, enum mesh_str_id heading, enum mesh_ui_tone tone);

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
bool fb_draw_card(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                  int *y, const struct fb_card *card);

/* "Messages (12)", or "Messages (12, +40 older)" when a ring has dropped some. */
void fb_title_count(char *out, size_t out_len, const char *name, uint32_t count, uint32_t dropped);

#endif /* MESH_UI_BACKENDS_FB_WIDGETS_H */
