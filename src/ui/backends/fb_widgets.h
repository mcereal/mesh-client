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

/* "Messages (12)", or "Messages (12, +40 older)" when a ring has dropped some. */
void fb_title_count(char *out, size_t out_len, const char *name, uint32_t count, uint32_t dropped);

/*
 * One label/value line that stops at the footer instead of drawing over it.
 *
 * The Status screen packs a variable number of these - blocks appear as the radio reports
 * them - so the alternative to clipping here is a layout that overwrites its own footer on
 * somebody's device.
 */
void fb_draw_status_row(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout, int *y, enum mesh_ui_tone tone,
                        enum mesh_str_id label, enum mesh_str_id value, ...);

/* The same row for a value that is already text - a device name, an age, a transport's own
   status word. It exists so no "%s" pass-through ends up in the catalog, where it would be a
   line for a translator to wonder about. */
void fb_draw_status_text(const struct mesh_ui_backend_fb_state *state,
                         const struct fb_layout *layout, int *y, enum mesh_ui_tone tone,
                         enum mesh_str_id label, const char *value);

#endif /* MESH_UI_BACKENDS_FB_WIDGETS_H */
