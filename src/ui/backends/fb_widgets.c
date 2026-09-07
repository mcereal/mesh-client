#define _POSIX_C_SOURCE 200809L

/*
 * The components. Everything here is built from fb_draw.c's primitives and knows nothing about
 * the snapshot - a widget takes what it draws, not where it came from, which is what lets the
 * same list component carry devices, nodes, messages and settings rows.
 */

#include "fb_widgets.h"

#include "mesh/ui/anim.h"
#include "mesh/ui/emoji.h"
#include "mesh/utils/text.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * The fill a variant wears in a state, and the ink that goes on it.
 *
 * One table rather than a chain of ifs at the draw site, because the point of a variant is
 * that its two colours travel together: every pair below is one mesh_ui_theme_validate()
 * holds to 4.5:1, and splitting them across branches is how a label ends up on a fill nothing
 * checked it against. `has_fill` false means the button draws no ground of its own and the
 * caller's idle tone is the ink.
 */
struct fb_button_paint {
    bool has_fill;
    enum mesh_ui_color fill;
    enum mesh_ui_color ink;
};

static struct fb_button_paint fb_button_paint(enum fb_button_variant variant, bool selected) {
    switch (variant) {
    case FB_BUTTON_FILLED:
        return (struct fb_button_paint){
            true, selected ? MESH_UI_COLOR_SURFACE_ACTIVE : MESH_UI_COLOR_SURFACE_SEL,
            MESH_UI_COLOR_TEXT_ON_SEL};
    case FB_BUTTON_TONAL:
        /* Under the cursor a tonal control commits to the full accent: the held-back fill is
           there so a label can be read over it, and a control being pressed has stopped being
           something to read. */
        return selected
                   ? (struct fb_button_paint){true, MESH_UI_COLOR_ACCENT, MESH_UI_COLOR_ON_ACCENT}
                   : (struct fb_button_paint){true, MESH_UI_COLOR_ACCENT_CONTAINER,
                                              MESH_UI_COLOR_ON_ACCENT_CONTAINER};
    case FB_BUTTON_TEXT:
    default:
        return selected ? (struct fb_button_paint){true, MESH_UI_COLOR_SURFACE_ACTIVE,
                                                   MESH_UI_COLOR_TEXT_ON_SEL}
                        : (struct fb_button_paint){false, MESH_UI_COLOR_BG, MESH_UI_COLOR_TEXT};
    }
}

/* What a button's content occupies: its icon, the gap after it, and its label - all in cells
   except that gap, which is half of one. */
static int fb_button_content_w(const struct mesh_ui_backend_fb_state *state,
                               const struct fb_button *button) {
    const int adv = fb_char_adv(state, button->scale);
    const bool has_label = button->label != NULL && button->label[0] != '\0';
    const bool has_icon = mesh_ui_icon_is_valid(button->icon);
    const int label_w = has_label ? (int)mesh_ui_text_cells(button->label) * adv : 0;
    return label_w + (has_icon ? fb_icon_box(state, button->scale) : 0) +
           ((has_icon && has_label) ? adv / 2 : 0);
}

void fb_draw_button(const struct mesh_ui_backend_fb_state *state, const struct fb_button *button) {
    const struct fb_button_paint paint = fb_button_paint(button->variant, button->selected);
    if (paint.has_fill) {
        fb_fill_round_rect(state, button->rect.x, button->rect.y, button->rect.w, button->rect.h,
                           fb_radius(state, button->shape), fb_color(state, paint.fill));
    }

    const bool has_label = button->label != NULL && button->label[0] != '\0';
    const bool has_icon = mesh_ui_icon_is_valid(button->icon);
    if (!has_label && !has_icon) {
        return;
    }

    /* Centre by cells, so a label holding an emoji sits where it looks centred rather than
       where its byte count says it does. An icon is one cell, so it centres by the same count.
       Vertically it is the glyph body that is centred, not the line advance: the advance
       carries the gap that accents hang in, and counting it would push every label low in its
       box. */
    const int adv = fb_char_adv(state, button->scale);
    /* Half a cell between a symbol and the word after it, which is the gap Material puts
       there and about what the eye needs to stop reading them as one shape. fb_button_content_w()
       is the same sum, so a caller sizing a box around this gets the box this fills. */
    const int text_w = fb_button_content_w(state, button);
    const int text_h = (int)fb_font(state)->height * button->scale;
    int x = button->rect.x + (button->rect.w - text_w) / 2;
    const int y = button->rect.y + (button->rect.h - text_h) / 2;
    /* Any fill at all means the label is drawn in the colour the theme validates against that
       fill. Only a button with no fill is free to take its idle tone, which is chosen against
       the ground - reading the tone over a fill is how the keyboard's action row came out
       white on white. */
    const struct mesh_ui_rgb ink =
        paint.has_fill ? fb_color(state, paint.ink) : fb_tone_color(state, button->idle_tone);
    if (has_icon) {
        /* Blended against the fill the button has just laid down, or against the ground when it
           laid none - the two colours the ink was chosen against. */
        fb_draw_icon(state, x, y, button->icon, button->scale, ink,
                     fb_color(state, paint.has_fill ? paint.fill : MESH_UI_COLOR_BG));
        x += fb_icon_box(state, button->scale) + (has_label ? adv / 2 : 0);
    }
    if (has_label) {
        fb_draw_text(state, x, y, button->label, button->scale, ink);
    }
}

/*
 * Padding enough to clear the capsule's own curve, and derived from the *glyph scale* rather
 * than from the cell advance.
 *
 * A capsule's ends eat into their own corners, so a label needs room the flat-sided chip did
 * not: the pill's radius is half its height - (7 + 2) / 2 scale steps for the 5x7 font - and at
 * the top and bottom of a glyph body the edge has curved inwards by about 1.7 steps. Two either
 * side clears that with room to spare.
 *
 * Why not measure it in cells, which is how everything else here is measured? Because a cell is
 * six scale steps wide, so a cell of padding either side is three times what the curve needs,
 * and the strip has to *fit*: five tabs at MESHCLIENT_FB_SCALE=5 leave about 60 px of slack
 * across a 1024 px panel, and a padding that generous spends 240 of it. The last tab and its
 * indicator then fall off the right-hand edge, which is a navigation tab the user can no longer
 * see rather than a cosmetic overflow.
 */
#define FB_CHIP_PAD_STEPS 4

int fb_chip_width(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_icon icon,
                  const char *label, int scale) {
    const struct fb_button button = {.icon = icon, .label = label, .scale = scale};
    /* The pill itself, then the gap before the next one. */
    return fb_button_content_w(state, &button) + FB_CHIP_PAD_STEPS * scale +
           fb_char_adv(state, scale);
}

int fb_draw_chip(const struct mesh_ui_backend_fb_state *state, int x, int y, enum mesh_ui_icon icon,
                 const char *label, bool active, int scale) {
    const int width = fb_chip_width(state, icon, label, scale);
    const struct fb_button button = {
        .rect = {.x = x,
                 .y = y - scale,
                 .w = width - fb_char_adv(state, scale), /* the pill, without the gap after it */
                 .h = fb_line_adv(state, scale)},
        .icon = icon,
        .label = label,
        .selected = false,
        .variant = active ? FB_BUTTON_TONAL : FB_BUTTON_TEXT,
        .shape = MESH_UI_SHAPE_FULL,
        .idle_tone = MESH_UI_TONE_DIM,
        .scale = scale,
    };
    fb_draw_button(state, &button);
    return x + width;
}

void fb_draw_title(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                   const char *title) {
    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", title);
    mesh_ui_line_fit(&line, layout->cols);
    fb_draw_text(state, fb_margin(state), layout->body_y, mesh_ui_line_text(&line), state->scale,
                 fb_tone_color(state, MESH_UI_TONE_ACCENT));
    layout->body_y += layout->line + state->scale;
    if (layout->rows > 1U) {
        layout->rows -= 1U;
    }
}

void fb_draw_empty(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   enum mesh_ui_icon icon, const char *text) {
    int y = layout->body_y;
    uint32_t rows = layout->rows;

    /*
     * The icon is drawn at three glyph scales - a cell is one line tall, so three of them is
     * three body rows and about a fifth of the panel - and it is only drawn when the screen has
     * the rows to spare. An empty state is the one place with room for it, and the one place
     * where a symbol says "nothing here yet" faster than the sentence under it does.
     */
    const int big = state->scale * 3;
    const uint32_t cost = 4U; /* three rows for the symbol, one of air under it */
    if (mesh_ui_icon_is_valid(icon) && big <= FB_ICON_SCALE_MAX && rows > cost + 1U) {
        const int box = fb_icon_box(state, big);
        fb_draw_icon(state, ((int)state->var.xres - box) / 2, y, icon, big,
                     fb_tone_color(state, MESH_UI_TONE_DIM), fb_color(state, MESH_UI_COLOR_BG));
        y += (int)cost * layout->line;
        rows -= cost;
    }

    /* Wrapped rather than drawn flat: these strings say which button to press next, and at a
       large glyph scale a flat one ran off the right edge with the verb on it. */
    (void)fb_draw_wrapped(state, y, text, layout->cols, (int)rows,
                          fb_tone_color(state, MESH_UI_TONE_DIM));
}

void fb_draw_rule(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int scale,
                  enum mesh_ui_color role) {
    fb_fill_rect(state, x, y, w, scale / 2 > 0 ? scale / 2 : 1, fb_color(state, role));
}

struct fb_list fb_list_begin_visible(const struct fb_layout *layout, uint32_t count,
                                     uint32_t cursor, uint32_t visible) {
    struct fb_list list;
    memset(&list, 0, sizeof list);
    list.model = mesh_ui_list_begin(count, cursor, visible);
    list.y = layout->body_y;
    list.line = layout->line;
    list.cols = layout->cols;
    return list;
}

struct fb_list fb_list_begin(const struct fb_layout *layout, uint32_t count, uint32_t cursor) {
    return fb_list_begin_visible(layout, count, cursor, layout->rows);
}

struct fb_list fb_list_begin_rows(const struct fb_layout *layout, uint32_t count, uint32_t cursor,
                                  uint32_t per_item) {
    const uint32_t rows = per_item > 0U ? layout->rows / per_item : layout->rows;
    return fb_list_begin_visible(layout, count, cursor, rows > 0U ? rows : 1U);
}

bool fb_list_next(struct fb_list *list, uint32_t *index) {
    return mesh_ui_list_next(&list->model, index);
}

void fb_list_row(const struct mesh_ui_backend_fb_state *state, struct fb_list *list, uint32_t index,
                 const char *text, enum mesh_ui_tone tone) {
    fb_draw_row(state, list->y, text, fb_tone_color(state, tone),
                mesh_ui_list_is_cursor(&list->model, index));
    list->y += list->line;
}

void fb_list_row_line(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                      uint32_t index, struct mesh_ui_line *line, enum mesh_ui_tone tone) {
    mesh_ui_line_fit(line, list->cols);
    fb_list_row(state, list, index, mesh_ui_line_text(line), tone);
}

/* ---- the conversation cell ----------------------------------------------------------------- */

/*
 * The disc and what is in it: a node's initials, or an icon for the rows that are not a person.
 * `size` is both its width and its height, so the radius is half of it and the shape is a
 * circle.
 *
 * What it holds is drawn at the largest multiplier that *fits inside the disc*, which is not
 * always the body's. A two-row item gives the disc two lines to be round in and the body scale
 * fits with room; a one-row item - a node, a picker row - gives it one, and two cells at the
 * body scale then overhang a circle barely taller than a single glyph. That drew initials
 * sliced off at both ends, which is worse than no disc at all: the whole job of the colour and
 * the two letters is to be recognised without being read.
 *
 * So the fit is measured rather than assumed. It is done here, once, because the caller cannot
 * answer it - which scale fits is a fact about this component's geometry, and a screen that had
 * to work it out would be computing a glyph size, which is the thing screens do not do. An icon
 * is measured by the same loop: it is one cell wide and drawn at the glyph body's height, which
 * is the taller of the two the loop tests.
 *
 * An icon takes the same ink the initials do - the ground colour, which every avatar tint is
 * validated against - and is blended over the tint it is standing on.
 */
static void fb_draw_avatar(const struct mesh_ui_backend_fb_state *state, int x, int y, int size,
                           const char *label, enum mesh_ui_icon icon, struct mesh_ui_rgb tint) {
    fb_fill_round_rect(state, x, y, size, size, size / 2, tint);
    const bool has_icon = mesh_ui_icon_is_valid(icon);
    if (!has_icon && (label == NULL || label[0] == '\0')) {
        return;
    }
    /*
     * The inset a circle owes its contents: at the corners of the text box the disc has already
     * curved away, so text measured against the full diameter still touches the edge. An eighth
     * on each side is what keeps two cells clear of it at every scale the theme allows.
     */
    const int room = size - size / 4;
    const size_t cells = has_icon ? 1U : mesh_ui_text_cells(label);
    int scale = state->scale;
    while (scale > 1 && ((int)cells * fb_char_adv(state, scale) > room ||
                         (int)fb_font(state)->height * scale > room)) {
        --scale;
    }

    /* Centred in cells, and vertically on the glyph body rather than the line advance - the
       advance carries the gap accents hang in, and counting it sits the initials low in the
       disc. The same reasoning as fb_draw_button's label. */
    const int text_h = (int)fb_font(state)->height * scale;
    const int content_y = y + (size - text_h) / 2;
    if (has_icon) {
        fb_draw_icon(state, x + (size - fb_icon_box(state, scale)) / 2, content_y, icon, scale,
                     fb_color(state, MESH_UI_COLOR_BG), tint);
        return;
    }
    const int text_w = (int)cells * fb_char_adv(state, scale);
    fb_draw_text(state, x + (size - text_w) / 2, content_y, label, scale,
                 fb_color(state, MESH_UI_COLOR_BG));
}

/* ---- the list item ------------------------------------------------------------------------ */

/*
 * An item's geometry, all of it derived once so the fill, the text and the slots cannot
 * disagree about where the row is. Every screen used to re-derive some part of this, and the
 * parts that drifted were exactly the ones nothing else could see: how far a trailing control
 * ate into the text, and which box a control centred itself on.
 */
struct fb_item_geom {
    uint32_t rows;
    int fill_top, fill_h; /* the box the cursor fill paints, and a control centres on */
    int head_y;           /* headline baseline */
    int supp_y;           /* supporting baseline; only meaningful on a two-row item */
    int slot_h;           /* height of a box-shaped trailing - a badge */
    int head_slot_top, supp_slot_top;
    int text_x, text_right;
    size_t cols; /* text columns between the leading slot and the trailing edge */
};

static struct fb_item_geom fb_item_measure(const struct mesh_ui_backend_fb_state *state,
                                           const struct fb_list *list,
                                           const struct fb_list_item *item) {
    const int scale = state->scale;
    const int adv = fb_char_adv(state, scale);
    const int margin = fb_margin(state);
    struct fb_item_geom g;
    memset(&g, 0, sizeof g);

    g.rows = item->supporting != NULL ? 2U : 1U;
    g.head_y = list->y;
    g.text_right = (int)state->var.xres - margin;
    g.slot_h = list->line - scale;

    if (g.rows == 2U) {
        /* Two rows set closer together than two items are: the supporting line sits a scale
           above where a second row would put it, and the space that frees becomes the gap to
           the next item. */
        g.supp_y = list->y + list->line - scale;
        g.fill_top = g.head_y - scale / 2;
        g.fill_h = 2 * list->line - 2 * scale;
        g.head_slot_top = g.fill_top;
        g.supp_slot_top = g.supp_y - scale / 2;
    } else {
        g.fill_top = g.head_y - scale;
        g.fill_h = list->line;
        g.head_slot_top = g.fill_top;
        g.supp_slot_top = g.fill_top;
    }

    g.text_x = margin;
    if (item->leading.kind == FB_LEADING_AVATAR) {
        g.text_x = margin + (g.fill_h - scale) + adv / 2;
    } else if (item->leading.kind == FB_LEADING_ICON) {
        /* Reserved whether or not this row filled it, so every row's words start in the same
           column - a list that indents only the rows with something to say is a list the eye
           cannot run down. */
        g.text_x = margin + fb_icon_box(state, scale) + adv / 2;
    }
    g.cols = g.text_right > g.text_x ? (size_t)((g.text_right - g.text_x) / adv) : 1U;
    return g;
}

/*
 * Cells a trailing slot takes out of a line `cols` wide, its breathing room included.
 *
 * Zero has one meaning and it covers both ways a slot can come to nothing: there is nothing in
 * it, or the line is too narrow to give it its room. Either way the slot is not drawn and the
 * line keeps every column it has - because a trailing figure is worth less than the row it
 * would be laid across, and a row clipped to one cell has lost the thing it was about.
 *
 * The point of one function answering that is that *measuring and drawing ask it once*. A slot
 * squeezed out of the line by one calculation and then painted over that line by another is
 * exactly the bug this component exists to make unwritable - it is what a narrow panel or a
 * large glyph scale used to turn an age into, drawn back across the avatar.
 */
static size_t fb_trailing_cols(const struct mesh_ui_backend_fb_state *state, size_t cols,
                               const struct fb_trailing *trailing) {
    const int adv = fb_char_adv(state, state->scale);
    size_t want = 0U;
    switch (trailing->kind) {
    case FB_TRAILING_TEXT: {
        const size_t cells = mesh_ui_text_cells(trailing->text);
        want = cells > 0U ? cells + 1U : 0U;
        break;
    }
    case FB_TRAILING_BADGE: {
        const size_t cells = mesh_ui_text_cells(trailing->text);
        want = cells > 0U ? cells + 2U : 0U;
        break;
    }
    case FB_TRAILING_SWITCH: {
        int width = 0;
        fb_switch_size(state, state->scale, &width, NULL);
        want = (size_t)((width + adv - 1) / adv) + 2U;
        break;
    }
    case FB_TRAILING_ICON:
        want = mesh_ui_icon_is_valid(trailing->icon) ? 2U : 0U;
        break;
    case FB_TRAILING_NONE:
    default:
        return 0U;
    }
    /* Strictly greater: the line keeps at least one cell of its own, which is the test the
       conversation cell made for its age before this was a shared slot. */
    return (want > 0U && cols > want) ? want : 0U;
}

static void fb_draw_trailing(struct mesh_ui_backend_fb_state *state, const struct fb_item_geom *g,
                             const struct fb_trailing *trailing, int baseline, int slot_top,
                             bool selected) {
    const int scale = state->scale;
    const int adv = fb_char_adv(state, scale);
    const size_t cells = (trailing->kind == FB_TRAILING_TEXT || trailing->kind == FB_TRAILING_BADGE)
                             ? mesh_ui_text_cells(trailing->text)
                             : 0U;

    switch (trailing->kind) {
    case FB_TRAILING_TEXT:
        if (cells == 0U) {
            return;
        }
        /* Always the quiet ink, on the ground and on the fill alike: a trailing figure is
           something the eye glances at on its way past, never the row's own words. */
        fb_draw_text(state, g->text_right - (int)cells * adv, baseline, trailing->text, scale,
                     selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                              : fb_tone_color(state, MESH_UI_TONE_DIM));
        return;
    case FB_TRAILING_BADGE: {
        if (cells == 0U) {
            return;
        }
        /* A capsule, the way every messenger draws a count. The shape does the work: a filled
           rectangle on the end of a row reads as part of it, and the same fill with its ends
           taken off reads as something sitting on top. */
        const int width = (int)(cells + 1U) * adv;
        const int x = g->text_right - width;
        fb_fill_round_rect(state, x, slot_top, width, g->slot_h,
                           fb_radius(state, MESH_UI_SHAPE_FULL),
                           fb_color(state, MESH_UI_COLOR_ACCENT));
        /* Whatever the theme says reads on its own accent fill - on the dark palette that is
           the ground colour, because white on that yellow is unreadable at this glyph size. */
        fb_draw_text(state, x + adv / 2, baseline, trailing->text, scale,
                     fb_color(state, MESH_UI_COLOR_ON_ACCENT));
        return;
    }
    case FB_TRAILING_ICON:
        /* The quiet ink a trailing figure takes, for the same reason: a chevron is something the
           eye passes on its way down the list, never one of the row's own words. */
        fb_draw_icon(state, g->text_right - fb_icon_box(state, scale), baseline, trailing->icon,
                     scale,
                     selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                              : fb_tone_color(state, MESH_UI_TONE_DIM),
                     fb_color(state, selected ? MESH_UI_COLOR_SURFACE_SEL : MESH_UI_COLOR_BG));
        return;
    case FB_TRAILING_SWITCH: {
        if (trailing->sw == NULL) {
            return;
        }
        int width = 0;
        int height = 0;
        fb_switch_size(state, scale, &width, &height);
        /*
         * Centred on the row's *fill* rather than on the glyph body. The two coincide for the
         * font that ships, but only the fill is what the control has to stay inside, and a
         * switch that overhangs it notches the highlight on the one row the cursor is on.
         */
        trailing->sw->rect.w = width;
        trailing->sw->rect.h = height;
        trailing->sw->rect.x = g->text_right - width;
        trailing->sw->rect.y = g->fill_top + (g->fill_h - height) / 2;
        trailing->sw->selected = selected;
        fb_draw_switch(state, trailing->sw);
        return;
    }
    case FB_TRAILING_NONE:
    default:
        return;
    }
}

/* Cells between the end of the label column and the start of the value: a space, the marker's
   own cell, a space. Reserved on every row of a list whether or not that row has a marker in
   it, which is what keeps the values in one column. */
#define FB_ITEM_MARKER_CELLS 3U

/* The headline, as one line: either the whole row, or a label column with the marker gutter and
   a value after it. The gutter is left blank here and the icon is drawn into it afterwards. */
static void fb_item_headline(struct mesh_ui_line *line, const struct fb_list_item *item) {
    mesh_ui_line_reset(line);
    if (item->label_cols > 0U) {
        /* The label occupies its column exactly - clipped when long, padded when short -
           measured in cells so a value column still lines up under a label that is not all
           ASCII. */
        mesh_ui_line_column(line, item->label != NULL ? item->label : "", item->label_cols);
        mesh_ui_line_pad_to(line, item->label_cols + FB_ITEM_MARKER_CELLS);
        mesh_ui_line_printf(line, "%s", item->value != NULL ? item->value : "");
        return;
    }
    mesh_ui_line_printf(line, "%s", item->text != NULL ? item->text : "");
}

void fb_list_item(struct mesh_ui_backend_fb_state *state, struct fb_list *list, uint32_t index,
                  const struct fb_list_item *item) {
    const int scale = state->scale;
    const bool selected = mesh_ui_list_is_cursor(&list->model, index);
    const struct fb_item_geom g = fb_item_measure(state, list, item);

    if (selected) {
        const int radius = fb_radius(state, MESH_UI_SHAPE_SM);
        const int row_x = fb_margin(state) / 2;
        const int row_w = (int)state->var.xres - fb_margin(state);
        if (item->accent_edge) {
            /*
             * The bar is laid the way a card's edge is: the accent shape first, the fill over
             * it a scale narrower on the left only. Both share their right edge, so the accent
             * survives just where the bar is meant to be - and it follows the corner instead
             * of poking a square end out of it, which is what a straight bar does once the row
             * has ends.
             */
            fb_fill_round_rect(state, row_x, g.fill_top, row_w, g.fill_h, radius,
                               fb_color(state, MESH_UI_COLOR_ACCENT));
            fb_fill_round_rect(state, row_x + scale, g.fill_top, row_w - scale, g.fill_h, radius,
                               fb_color(state, MESH_UI_COLOR_SURFACE_SEL));
        } else {
            fb_fill_round_rect(state, row_x, g.fill_top, row_w, g.fill_h, radius,
                               fb_color(state, MESH_UI_COLOR_SURFACE_SEL));
        }
    }

    /*
     * Under the cursor everything is drawn against that fill instead of against the ground,
     * which is a different pair of colours and not a dimmer version of the same one.
     */
    const struct mesh_ui_rgb head_ink =
        selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL) : fb_tone_color(state, item->tone);
    /* What every icon on this row is blended against: the fill if the cursor laid one down, the
       ground otherwise. The row is the only thing that knows. */
    const struct mesh_ui_rgb ground =
        fb_color(state, selected ? MESH_UI_COLOR_SURFACE_SEL : MESH_UI_COLOR_BG);

    if (item->leading.kind == FB_LEADING_AVATAR) {
        const int size = g.fill_h - scale;
        const struct mesh_ui_rgb tint =
            item->leading.role < MESH_UI_COLOR_COUNT
                ? fb_color(state, item->leading.role)
                : mesh_ui_theme_avatar(state->theme, item->leading.tint);
        fb_draw_avatar(state, fb_margin(state), g.fill_top + scale / 2, size, item->leading.label,
                       item->leading.icon, tint);
    } else if (item->leading.kind == FB_LEADING_ICON) {
        fb_draw_icon(state, fb_margin(state), g.head_y, item->leading.icon, scale, head_ink,
                     ground);
    }

    struct mesh_ui_line line;
    fb_item_headline(&line, item);
    const size_t head_take = fb_trailing_cols(state, g.cols, &item->trailing);
    mesh_ui_line_fit(&line, g.cols - head_take);
    fb_draw_text(state, g.text_x, g.head_y, mesh_ui_line_text(&line), scale, head_ink);
    /* Into the blank cell fb_item_headline() left between the label column and the value, and
       only when the value column actually got that far - a label column wider than the row is
       clipped, and a marker drawn at a column the line no longer reaches would sit on top of
       the label. */
    if (item->label_cols > 0U && mesh_ui_icon_is_valid(item->marker_icon) &&
        g.cols > item->label_cols + FB_ITEM_MARKER_CELLS) {
        fb_draw_icon(state, g.text_x + (int)(item->label_cols + 1U) * fb_char_adv(state, scale),
                     g.head_y, item->marker_icon, scale, head_ink, ground);
    }
    if (head_take > 0U) {
        fb_draw_trailing(state, &g, &item->trailing, g.head_y, g.head_slot_top, selected);
    }

    if (g.rows == 2U) {
        const struct mesh_ui_rgb supp_ink =
            selected ? fb_color(state, item->supporting_quiet ? MESH_UI_COLOR_TEXT_ON_SEL_DIM
                                                              : MESH_UI_COLOR_TEXT_ON_SEL)
                     : fb_tone_color(state, item->supporting_tone);
        const size_t supp_take = fb_trailing_cols(state, g.cols, &item->supporting_trailing);
        /* An icon on the supporting line takes the first cell and the words move over, which is
           what "> " did when it was two characters of the preview. */
        const bool supp_icon =
            mesh_ui_icon_is_valid(item->supporting_icon) && g.cols > supp_take + 1U;
        const int supp_x = g.text_x + (supp_icon ? fb_char_adv(state, scale) : 0);
        if (supp_icon) {
            fb_draw_icon(state, g.text_x, g.supp_y, item->supporting_icon, scale, supp_ink, ground);
        }
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%s", item->supporting);
        mesh_ui_line_fit(&line, g.cols - supp_take - (supp_icon ? 1U : 0U));
        fb_draw_text(state, supp_x, g.supp_y, mesh_ui_line_text(&line), scale, supp_ink);
        if (supp_take > 0U) {
            fb_draw_trailing(state, &g, &item->supporting_trailing, g.supp_y, g.supp_slot_top,
                             selected);
        }
    }

    /*
     * The divider goes in the gap the tightened leading opened up, inset to where the text
     * starts rather than run edge to edge: a leading disc already separates the items, and a
     * full-width rule under one reads as a box drawn around it.
     */
    const bool last = (index + 1U >= list->model.count) ||
                      (index + 1U >= list->model.first + list->model.visible);
    if (item->divider && !selected && !last) {
        fb_draw_rule(state, g.text_x, g.fill_top + g.fill_h + scale / 2, g.text_right - g.text_x,
                     scale, MESH_UI_COLOR_RULE);
    }

    list->y += (int)g.rows * list->line;
}

/*
 * A conversation is a two-line item with a disc on the front, a time on the first line and an
 * unread count on the second - which is the whole of what it is, now that the item can say
 * that. What is left here is the *translation*: which of a conversation's facts goes in which
 * slot, and which of them changes what the row says rather than how it looks.
 */
void fb_draw_conversation(struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                          uint32_t index, const struct fb_conversation *conversation) {
    struct mesh_ui_line preview;
    mesh_ui_line_reset(&preview);
    if (conversation->armed) {
        /* The armed row says what the next press does, in place of the preview it would take
           away. Nothing else on screen changes, so the warning is on the row it is about - and
           it stays loud under the cursor, which a preview does not. */
        mesh_ui_line_printf(&preview, "%s", mesh_str(MESH_STR_MESSAGES_DELETE_ARMED));
    } else if (conversation->preview[0] != '\0') {
        mesh_ui_line_printf(&preview, "%s", conversation->preview);
    } else {
        mesh_ui_line_printf(&preview, "%s", mesh_str(MESH_STR_MESSAGES_NO_MESSAGES_YET));
    }

    const struct fb_list_item item = {
        .leading =
            {
                .kind = FB_LEADING_AVATAR,
                .label = conversation->avatar,
                /* A row armed to be deleted says so inside its own disc, which is already
                   wearing the bad tone: the question and the answer in one place. */
                .icon = conversation->armed ? MESH_UI_ICON_DELETE : conversation->avatar_icon,
                .tint = conversation->tint,
                .role = conversation->armed    ? MESH_UI_COLOR_BAD
                        : conversation->accent ? MESH_UI_COLOR_ACCENT
                                               : MESH_UI_COLOR_COUNT,
            },
        .text = conversation->name,
        .tone = conversation->name_tone,
        /* The age sits against the right edge, quietly, the way a messenger dates a
           conversation - a fact you glance at, not one you read. */
        .trailing = {.kind = FB_TRAILING_TEXT, .text = conversation->age},
        .supporting = mesh_ui_line_text(&preview),
        /* The reply arrow says the last word in the thread was ours, which is what tells you
           whether a quiet thread is waiting on you or on them. It was "> " until it was an
           icon slot, and it is dropped on an armed row - what that row says is the warning. */
        .supporting_icon = (conversation->preview_outbound && !conversation->armed)
                               ? MESH_UI_ICON_REPLY
                               : MESH_UI_ICON_NONE,
        /* An unread preview is the row's own words at full weight - never the name's tone,
           which says what kind of conversation this is rather than how much of it is new. The
           half that still reads once the badge has been marked away. */
        .supporting_tone = conversation->armed    ? MESH_UI_TONE_BAD
                           : conversation->unread ? MESH_UI_TONE_STRONG
                                                  : MESH_UI_TONE_DIM,
        .supporting_quiet = !conversation->armed && !conversation->unread,
        .supporting_trailing = {.kind = FB_TRAILING_BADGE, .text = conversation->badge},
        .accent_edge = true,
        .divider = true,
    };
    fb_list_item(state, list, index, &item);
}

/* ---- chat bubbles ------------------------------------------------------------------------- */

/*
 * What a bubble works out to at this width, computed once and handed to both the measure and
 * the draw. Everything is in cells except `rows`, which is body rows.
 */
struct fb_bubble_metrics {
    size_t cols;        /* inner content width */
    uint32_t lines;     /* wrapped text lines */
    bool meta_own_line; /* the clock did not fit on the last one */
    uint32_t rows;
};

/* A bubble never spans the whole panel: the gutter down the other side is what says which end
   of the conversation it came from, so three quarters is a look rather than a limit. */
static size_t fb_bubble_max_cols(const struct mesh_ui_backend_fb_state *state,
                                 const struct fb_layout *layout) {
    const size_t pct = fb_metrics(state)->bubble_width_pct;
    const size_t max = layout->cols > 4U ? layout->cols * pct / 100U : layout->cols;
    return max > 0U ? max : 1U;
}

static bool fb_bubble_has(const char *text) { return text != NULL && text[0] != '\0'; }

/*
 * The tone for a bubble's quieter lines - the sender on one of ours, the clock on any of them.
 *
 * Dim while the bubble sits at rest, and the bubble's own body colour once the cursor is on it.
 * The selected fills are a step lighter by design, and dim over one of those is the pairing a
 * theme has least room for: on the dark palette it measured 1.9:1, well under the 3:1 a
 * secondary line is held to, and the body colour is a pair the theme is already validated on.
 */
static enum mesh_ui_tone fb_bubble_quiet_tone(const struct fb_bubble *bubble) {
    if (!bubble->selected || bubble->failed) {
        return MESH_UI_TONE_DIM;
    }
    return bubble->outbound ? MESH_UI_TONE_OUTBOUND : MESH_UI_TONE_INBOUND;
}

static struct fb_bubble_metrics fb_bubble_measure(const struct mesh_ui_backend_fb_state *state,
                                                  const struct fb_layout *layout,
                                                  const struct fb_bubble *bubble) {
    const size_t max = fb_bubble_max_cols(state, layout);
    struct fb_bubble_metrics metrics;
    memset(&metrics, 0, sizeof metrics);

    /* One wrap walk, and the draw makes the same one. Anything that measured the text a second
       way - a strlen, a second wrapper - is how a bubble comes to paint over the one below it. */
    size_t widest = 0U;
    size_t last = 0U;
    struct mesh_ui_wrap wrap;
    mesh_ui_wrap_begin(&wrap, bubble->text, max);
    while (mesh_ui_wrap_next(&wrap)) {
        last = mesh_ui_text_cells(wrap.line);
        if (last > widest) {
            widest = last;
        }
        metrics.lines += 1U;
    }
    if (metrics.lines == 0U) {
        metrics.lines = 1U; /* an empty message is still a bubble, just an empty one */
    }

    metrics.cols = widest;
    if (fb_bubble_has(bubble->name)) {
        const size_t name_cols = mesh_ui_text_cells(bubble->name);
        if (name_cols > metrics.cols) {
            metrics.cols = name_cols;
        }
    }

    /* The clock rides the last line when there is room for it there, which is what keeps a
       three-word message three words tall instead of doubling it. */
    if (fb_bubble_has(bubble->meta)) {
        const size_t meta_cols = mesh_ui_text_cells(bubble->meta);
        if (last + 1U + meta_cols <= max) {
            const size_t tucked = last + 1U + meta_cols;
            if (tucked > metrics.cols) {
                metrics.cols = tucked;
            }
        } else {
            metrics.meta_own_line = true;
            if (meta_cols > metrics.cols) {
                metrics.cols = meta_cols;
            }
        }
    }

    if (metrics.cols > max) {
        metrics.cols = max;
    }
    if (metrics.cols == 0U) {
        metrics.cols = 1U;
    }

    metrics.rows = metrics.lines + (fb_bubble_has(bubble->name) ? 1U : 0U) +
                   (metrics.meta_own_line ? 1U : 0U) + (fb_bubble_has(bubble->separator) ? 1U : 0U);
    return metrics;
}

uint32_t fb_bubble_rows(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout, const struct fb_bubble *bubble) {
    return fb_bubble_measure(state, layout, bubble).rows;
}

void fb_draw_separator(const struct mesh_ui_backend_fb_state *state, int y, const char *label) {
    const int adv = fb_char_adv(state, state->scale);
    const int rule_y = y + ((int)fb_font(state)->height * state->scale) / 2;
    const int left = fb_margin(state);
    const int right = (int)state->var.xres - left;

    if (!fb_bubble_has(label)) {
        fb_draw_rule(state, left, rule_y, right - left, state->scale, MESH_UI_COLOR_RULE);
        return;
    }

    /* Centred in cells, so a label with an emoji in it sits where it looks centred. */
    const int width = (int)mesh_ui_text_cells(label) * adv;
    const int x = left + (right - left - width) / 2;
    fb_draw_rule(state, left, rule_y, x - left - adv, state->scale, MESH_UI_COLOR_RULE);
    fb_draw_rule(state, x + width + adv, rule_y, right - (x + width + adv), state->scale,
                 MESH_UI_COLOR_RULE);
    fb_draw_text(state, x, y, label, state->scale, fb_tone_color(state, MESH_UI_TONE_DIM));
}

void fb_draw_bubble(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                    int y, const struct fb_bubble *bubble) {
    const struct fb_bubble_metrics metrics = fb_bubble_measure(state, layout, bubble);
    const int adv = fb_char_adv(state, state->scale);
    const int scale = state->scale;

    if (fb_bubble_has(bubble->separator)) {
        fb_draw_separator(state, y, bubble->separator);
        y += layout->line;
    }

    /* The box: content plus half a cell of padding each side, against the edge the direction
       names. The fill stops a scale short of the row it ends on, so stacked bubbles read as
       separate messages rather than as one block. */
    const int pad = adv / 2 > 0 ? adv / 2 : 1;
    const int box_w = (int)metrics.cols * adv + 2 * pad;
    const int box_x =
        bubble->outbound ? (int)state->var.xres - fb_margin(state) - box_w : fb_margin(state);
    const uint32_t box_rows = metrics.rows - (fb_bubble_has(bubble->separator) ? 1U : 0U);
    const int box_h = (int)box_rows * layout->line - scale;

    enum mesh_ui_color fill_role;
    if (bubble->failed) {
        fill_role = MESH_UI_COLOR_BUBBLE_FAILED;
    } else if (bubble->outbound) {
        fill_role = bubble->selected ? MESH_UI_COLOR_BUBBLE_OUT_SEL : MESH_UI_COLOR_BUBBLE_OUT;
    } else {
        fill_role = bubble->selected ? MESH_UI_COLOR_BUBBLE_IN_SEL : MESH_UI_COLOR_BUBBLE_IN;
    }
    const struct mesh_ui_rgb fill = fb_color(state, fill_role);
    /*
     * Rounded, because a bubble is the one shape in this UI that everybody already has a
     * picture of: a transcript of square boxes reads as a log, and the same boxes with their
     * corners off read as a conversation. It is the panel shape, so how round belongs to the
     * theme along with everything else about it.
     *
     * The cursor also gets a bar down its outer edge - on a small panel a fill one step lighter
     * is not by itself enough to find, and a colour-blind eye gets nothing from it at all - and
     * it is laid as the card's edge is: the accent shape first, the fill over it a scale
     * narrower on the outer side only, so the bar follows the corner rather than squaring it
     * off. The three edges the two shapes share leave no accent showing on them.
     */
    const int radius = fb_radius(state, MESH_UI_SHAPE_MD);
    if (bubble->selected) {
        fb_fill_round_rect(state, box_x, y - scale, box_w, box_h, radius,
                           fb_color(state, MESH_UI_COLOR_ACCENT));
    }
    const int fill_x = (bubble->selected && !bubble->outbound) ? box_x + scale : box_x;
    const int fill_w = bubble->selected ? box_w - scale : box_w;
    fb_fill_round_rect(state, fill_x, y - scale, fill_w, box_h, radius, fill);

    const int text_x = box_x + pad;
    const struct mesh_ui_rgb body =
        fb_tone_color(state, bubble->outbound ? MESH_UI_TONE_OUTBOUND : MESH_UI_TONE_INBOUND);

    if (fb_bubble_has(bubble->name)) {
        struct mesh_ui_line line;
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%s", bubble->name);
        mesh_ui_line_fit(&line, metrics.cols);
        /* Ours is dimmed and theirs is accented: on our own bubble the name is a reminder, on
           theirs it is the thing being looked for. An alert overrides both - it is the one
           bubble whose heading is the point rather than the label on the point. */
        enum mesh_ui_tone name_tone =
            bubble->outbound ? fb_bubble_quiet_tone(bubble) : MESH_UI_TONE_ACCENT;
        if (bubble->alert) {
            name_tone = MESH_UI_TONE_BAD;
        }
        fb_draw_text(state, text_x, y, mesh_ui_line_text(&line), scale,
                     fb_tone_color(state, name_tone));
        y += layout->line;
    }

    /* The same walk the measure made, so the rows painted are the rows reserved. */
    struct mesh_ui_wrap wrap;
    mesh_ui_wrap_begin(&wrap, bubble->text, fb_bubble_max_cols(state, layout));
    int last_y = y;
    size_t last_cols = 0U;
    uint32_t drawn = 0U;
    while (mesh_ui_wrap_next(&wrap)) {
        fb_draw_text(state, text_x, y, wrap.line, scale, body);
        last_y = y;
        last_cols = mesh_ui_text_cells(wrap.line);
        y += layout->line;
        drawn += 1U;
    }
    if (drawn == 0U) {
        y += layout->line; /* the measure reserves a row for an empty message; spend it */
    }

    if (!fb_bubble_has(bubble->meta)) {
        return;
    }
    const int meta_w = (int)mesh_ui_text_cells(bubble->meta) * adv;
    const struct mesh_ui_rgb meta_color =
        fb_tone_color(state, bubble->failed ? MESH_UI_TONE_BAD : fb_bubble_quiet_tone(bubble));
    if (metrics.meta_own_line) {
        fb_draw_text(state, box_x + box_w - pad - meta_w, y, bubble->meta, scale, meta_color);
    } else {
        /* Tucked against the right edge of the line it shares, which is where every messenger
           puts it - and which is why the measure widened the bubble to make room. */
        const int x = box_x + box_w - pad - meta_w;
        fb_draw_text(state, x > text_x + (int)last_cols * adv ? x : text_x + (int)last_cols * adv,
                     last_y, bubble->meta, scale, meta_color);
    }
}

size_t fb_field_label_cols(const struct mesh_ui_backend_fb_state *state,
                           const struct fb_layout *layout, size_t preferred) {
    const struct mesh_ui_metrics *metrics = fb_metrics(state);
    if (preferred == 0U) {
        preferred = metrics->field_label_cols;
    }
    return layout->cols < metrics->narrow_cols ? layout->cols / 2U : preferred;
}

/* ---- cards -------------------------------------------------------------------------------- */

/*
 * A card's geometry, all of it derived from the theme's scale and its two card metrics.
 *
 * Computed once and shared by the measure and the draw, for the reason struct fb_bubble_metrics
 * exists: a card that measured one height and painted another would either leave a gap under
 * itself or paint over the rows below, and the screen places the next card from the height this
 * one reported.
 */
struct fb_card_metrics {
    int x, width;  /* the panel, edge included */
    int pad;       /* the inset from the side edges to the content */
    int pad_y;     /* the inset from the top and bottom edges */
    int edge;      /* the hairline's thickness */
    int radius;    /* corner radius, clamped by fb_fill_round_rect() anyway */
    int heading_h; /* what the heading costs, 0 when there is none */
    int content_x; /* where a row's text starts */
    size_t cols;   /* content width in cells */
    size_t label_cols;
    int gap; /* to the next card */
};

static struct fb_card_metrics fb_card_measure(const struct mesh_ui_backend_fb_state *state,
                                              const struct fb_layout *layout,
                                              const struct fb_card *card) {
    const struct mesh_ui_metrics *metrics = fb_metrics(state);
    const int scale = state->scale;
    struct fb_card_metrics m;
    memset(&m, 0, sizeof m);

    m.x = fb_margin(state);
    m.width = (int)state->var.xres - 2 * m.x;
    m.pad = (int)metrics->card_pad * scale;
    /* Half as much above and below as at the sides, which is not a fudge: a row is a line
       *advance* tall, and the advance already carries the leading that accents hang in, so the
       full inset is counted twice at the top and bottom of a stack of rows and only once at
       either end of one. A card padded equally all round reads bottom-heavy for that reason,
       and on a panel with fifteen body rows it costs the better part of one per card. */
    m.pad_y = m.pad / 2 > 0 ? m.pad / 2 : m.pad;
    m.edge = fb_edge(state);
    m.radius = fb_radius(state, MESH_UI_SHAPE_MD);
    /* A heading is drawn at the chrome scale, the size the tab strip and the footer are: a
       section label is not something to read, it is something to find, and at the body scale it
       costs a whole row of content on a panel that has fifteen of them. */
    m.heading_h = card->heading[0] != '\0' ? fb_line_adv(state, layout->small) : 0;

    const int inset = m.pad + m.edge;
    m.content_x = m.x + inset;
    const int adv = fb_char_adv(state, state->scale);
    const int content_w = m.width - 2 * inset;
    m.cols = content_w >= adv ? (size_t)(content_w / adv) : 1U;

    /*
     * The label column is measured from the labels the card is actually holding, rather than
     * taken from the theme's preferred width the way a settings list takes it.
     *
     * That is the one thing declaring a card before drawing it buys that a row-at-a-time API
     * cannot: every label is already in hand, so the column is exactly as wide as the widest of
     * them and every value starts as far left as it can. A fixed column has to be wide enough
     * for the longest label any screen might use, which leaves a card of short labels - and the
     * Status cards are mostly one word - with a gutter of nothing in the middle of it.
     *
     * Still bounded: half the card is the most a label column may take, because past that a
     * value is being squeezed to line up labels that already line up.
     */
    size_t widest = 0U;
    for (uint32_t i = 0U; i < card->count; ++i) {
        if (card->rows[i].kind != FB_CARD_ROW_FIELD) {
            continue;
        }
        const size_t cells = mesh_ui_text_cells(card->rows[i].label);
        if (cells > widest) {
            widest = cells;
        }
    }
    m.label_cols = widest + 1U;
    if (m.label_cols > m.cols / 2U) {
        m.label_cols = m.cols / 2U;
    }
    if (m.label_cols == 0U) {
        m.label_cols = 1U;
    }
    /* Enough space that two stacked cards read as two, and no more: the gap is what a fill
       needs to stop being a continuous block, which is the same job the conversation cell's
       tighter leading does. */
    m.gap = m.pad;
    return m;
}

/* Body rows one row of content occupies. Only a note is ever more than one. */
static uint32_t fb_card_row_lines(const struct fb_card_row *row, size_t cols) {
    if (row->kind != FB_CARD_ROW_NOTE) {
        return 1U;
    }
    uint32_t lines = mesh_ui_wrap_lines(row->value, cols);
    if (lines > FB_CARD_NOTE_LINES) {
        lines = FB_CARD_NOTE_LINES;
    }
    return lines > 0U ? lines : 1U;
}

/*
 * How much of a card is being drawn: whole rows, and then however many lines of the row after
 * them a note may keep.
 *
 * The second half is not a refinement. A note is a sentence the radio wrote about itself, and it
 * is the row on the Status screen that explains something no counter can - so dropping it whole
 * because its third line did not fit leaves the age it arrived at and none of the words. The
 * renderer this replaced capped the paragraph to the lines it had room for, and so does this.
 */
struct fb_card_fit {
    uint32_t rows;       /* rows drawn whole */
    uint32_t tail_lines; /* lines kept of rows[rows]; 0 when that row is not drawn at all */
};

/* The painted box for that much of the card - the gap to the next card is not part of it,
   because the last card on a screen does not spend one and a fit test that charged it anyway
   lost a row to a card that had room for it. */
static int fb_card_box_height(const struct fb_card_metrics *m, const struct fb_layout *layout,
                              const struct fb_card *card, struct fb_card_fit fit) {
    int height = 2 * (m->pad_y + m->edge) + m->heading_h;
    for (uint32_t i = 0U; i < fit.rows && i < card->count; ++i) {
        height += (int)fb_card_row_lines(&card->rows[i], m->cols) * layout->line;
    }
    return height + (int)fit.tail_lines * layout->line;
}

/*
 * As much of the card as fits between `top` and `bottom`.
 *
 * Whole rows go first, dropped from the end. Then, when the row that did not fit is a note, as
 * many of its lines as the leftover room takes - and only a note, because a field row is a label
 * and a value and half of one is not half a fact. Nothing is drawn after a clipped note either:
 * a truncated paragraph with rows under it reads as a complete one.
 */
static struct fb_card_fit fb_card_clip(const struct fb_card_metrics *m,
                                       const struct fb_layout *layout, const struct fb_card *card,
                                       int top, int bottom) {
    struct fb_card_fit fit = {.rows = card->count, .tail_lines = 0U};
    while (fit.rows > 0U && top + fb_card_box_height(m, layout, card, fit) > bottom) {
        fit.rows -= 1U;
    }
    if (fit.rows < card->count && card->rows[fit.rows].kind == FB_CARD_ROW_NOTE) {
        const uint32_t whole = fb_card_row_lines(&card->rows[fit.rows], m->cols);
        for (uint32_t lines = whole > 0U ? whole - 1U : 0U; lines > 0U; --lines) {
            const struct fb_card_fit probe = {.rows = fit.rows, .tail_lines = lines};
            if (top + fb_card_box_height(m, layout, card, probe) <= bottom) {
                fit.tail_lines = lines;
                break;
            }
        }
    }
    return fit;
}

void fb_card_begin(struct fb_card *card, enum mesh_ui_icon icon, enum mesh_str_id heading,
                   enum mesh_ui_tone tone) {
    if (card == NULL) {
        return;
    }
    memset(card, 0, sizeof *card);
    card->tone = tone;
    if (heading != MESH_STR_NONE) {
        card->icon = icon;
        mesh_text_sanitise_str(mesh_str(heading), card->heading, sizeof card->heading);
    }
}

/* The next free row, or NULL once the card is full. */
static struct fb_card_row *fb_card_next_row(struct fb_card *card, enum fb_card_row_kind kind,
                                            enum mesh_ui_tone tone) {
    if (card == NULL || card->count >= FB_CARD_ROWS_MAX) {
        return NULL;
    }
    struct fb_card_row *row = &card->rows[card->count++];
    memset(row, 0, sizeof *row);
    row->kind = kind;
    row->tone = tone;
    return row;
}

void fb_card_row_text(struct fb_card *card, enum mesh_ui_tone tone, enum mesh_str_id label,
                      const char *value) {
    struct fb_card_row *row = fb_card_next_row(card, FB_CARD_ROW_FIELD, tone);
    if (row == NULL) {
        return;
    }
    /*
     * Sanitised rather than copied: both of these are drawn, so a fixed buffer has to be cut on
     * a character and not on a byte - mesh_str_copy() would leave the tail of a multi-byte
     * sequence behind and the font would draw the pieces. It also folds control bytes away,
     * which matters for one row in particular: what the radio last said about itself arrives
     * off the air, and this is the last place before it becomes glyphs.
     */
    mesh_text_sanitise_str(mesh_str(label), row->label, sizeof row->label);
    mesh_text_sanitise_str(value, row->value, sizeof row->value);
}

void fb_card_row(struct fb_card *card, enum mesh_ui_tone tone, enum mesh_str_id label,
                 enum mesh_str_id value, ...) {
    char text[MESH_UI_LINE_MAX];
    va_list args;
    va_start(args, value);
    (void)mesh_str_vformat(text, sizeof text, value, args);
    va_end(args);
    fb_card_row_text(card, tone, label, text);
}

void fb_card_note(struct fb_card *card, enum mesh_ui_tone tone, const char *text) {
    if (text == NULL || text[0] == '\0') {
        return;
    }
    struct fb_card_row *row = fb_card_next_row(card, FB_CARD_ROW_NOTE, tone);
    if (row == NULL) {
        return;
    }
    mesh_text_sanitise_str(text, row->value, sizeof row->value);
}

bool fb_card_is_empty(const struct fb_card *card) { return card == NULL || card->count == 0U; }

int fb_card_height(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   const struct fb_card *card) {
    if (fb_card_is_empty(card)) {
        return 0;
    }
    const struct fb_card_metrics m = fb_card_measure(state, layout, card);
    const struct fb_card_fit whole = {.rows = card->count, .tail_lines = 0U};
    return fb_card_box_height(&m, layout, card, whole) + m.gap;
}

/* One row of content, drawn at `y` and returning the rows it used. `max_lines` of 0 means the
   row's own count; anything else is the budget a clipped note has been given. */
static uint32_t fb_draw_card_row(const struct mesh_ui_backend_fb_state *state,
                                 const struct fb_card_metrics *m, const struct fb_layout *layout,
                                 int y, const struct fb_card_row *row, uint32_t max_lines) {
    const struct mesh_ui_rgb color = fb_tone_color(state, row->tone);
    if (row->kind == FB_CARD_ROW_NOTE) {
        /* fb_draw_wrapped() lays out from the left margin, and a card's content starts inside
           it, so the note is wrapped here against the card's own column. */
        const uint32_t budget = max_lines > 0U ? max_lines : FB_CARD_NOTE_LINES;
        struct mesh_ui_wrap wrap;
        mesh_ui_wrap_begin(&wrap, row->value, m->cols);
        uint32_t drawn = 0U;
        while (drawn < budget && mesh_ui_wrap_next(&wrap)) {
            fb_draw_text(state, m->content_x, y + (int)drawn * layout->line, wrap.line,
                         state->scale, color);
            drawn += 1U;
        }
        return drawn > 0U ? drawn : 1U;
    }

    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    mesh_ui_line_column(&line, row->label, m->label_cols);
    mesh_ui_line_printf(&line, " %s", row->value);
    mesh_ui_line_fit(&line, m->cols);
    fb_draw_text(state, m->content_x, y, mesh_ui_line_text(&line), state->scale, color);
    return 1U;
}

bool fb_draw_card(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                  int *y, const struct fb_card *card) {
    if (y == NULL || fb_card_is_empty(card)) {
        return false;
    }
    const struct fb_card_metrics m = fb_card_measure(state, layout, card);

    /*
     * How much of the card there is room for - the one place the "does another row fit" test
     * lives. Every dense screen used to write it out per row, and the notice paragraph on the
     * Status screen wrote a second, differently. A card with nothing left but its heading is not
     * a card, so that is the point at which it is refused outright rather than drawn as an empty
     * box; a single clipped line of a note is content, so it is not that point.
     *
     * The bound is the body's bottom, not the footer's first baseline: fb_render_snapshot() keeps
     * half a margin between the two when it counts the body rows, and a card that ran to the
     * baseline itself would put its edge against the footer text on the one screen dense enough
     * to reach it.
     */
    const int bottom = layout->footer_y - fb_margin(state) / 2;
    const struct fb_card_fit fit = fb_card_clip(&m, layout, card, *y, bottom);
    if (fit.rows == 0U && fit.tail_lines == 0U) {
        return false;
    }

    const int height = fb_card_box_height(&m, layout, card, fit);
    /*
     * The edge first, then the fill inside it: fb_fill_round_rect() fills rather than strokes,
     * so an outline is the larger shape with the smaller one laid over it. Two fills rather than
     * four rectangles because the corners have to follow the radius, and a stroked round rect is
     * a primitive nothing else here would use.
     *
     * The edge is not decoration. On a theme whose surface is a step off the ground - which is
     * every one that ships, because a surface far from the ground is a surface body text is no
     * longer validated against - the fill alone is nearly invisible in daylight, and the
     * hairline is the whole of what says a card is there. It is drawn in OUTLINE rather than
     * RULE for that reason: a separator may fade politely into what it divides, an edge may
     * not. mesh_ui_theme_validate() holds OUTLINE against both the ground and the surface.
     */
    const int top = *y;
    fb_fill_round_rect(state, m.x, top, m.width, height, m.radius + m.edge,
                       fb_color(state, MESH_UI_COLOR_OUTLINE));
    fb_fill_round_rect(state, m.x + m.edge, top + m.edge, m.width - 2 * m.edge, height - 2 * m.edge,
                       m.radius, fb_color(state, MESH_UI_COLOR_SURFACE));

    int row_y = top + m.pad_y + m.edge;
    if (m.heading_h > 0) {
        /* The heading takes the card's tone, which is how a card reports on what it holds
           without a second cue: the Link card goes bad when the radio has gone. Every tone a
           card can take is validated against the surface, so none of them can go quiet here. */
        struct mesh_ui_line line;
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%s", card->heading);
        mesh_ui_line_fit(&line, fb_cols(state, layout->small));
        const struct mesh_ui_rgb ink = fb_tone_color(state, card->tone);
        int heading_x = m.content_x;
        if (mesh_ui_icon_is_valid(card->icon)) {
            /* On the card's own surface, which is what it was just filled with - the icon is
               inside the panel, not on the ground the panel sits on. */
            fb_draw_icon(state, heading_x, row_y, card->icon, layout->small, ink,
                         fb_color(state, MESH_UI_COLOR_SURFACE));
            /* The same half-cell a button leaves between its symbol and its word. */
            heading_x += fb_icon_box(state, layout->small) + fb_char_adv(state, layout->small) / 2;
        }
        fb_draw_text(state, heading_x, row_y, mesh_ui_line_text(&line), layout->small, ink);
        row_y += m.heading_h;
    }

    for (uint32_t i = 0U; i < fit.rows; ++i) {
        row_y += (int)fb_draw_card_row(state, &m, layout, row_y, &card->rows[i], 0U) * layout->line;
    }
    if (fit.tail_lines > 0U) {
        (void)fb_draw_card_row(state, &m, layout, row_y, &card->rows[fit.rows], fit.tail_lines);
    }

    *y = top + height + m.gap;
    return true;
}

void fb_title_count(char *out, size_t out_len, const char *name, uint32_t count, uint32_t dropped) {
    if (dropped > 0U) {
        mesh_str_format(out, out_len, MESH_STR_LIST_TITLE_COUNT_OLDER, name, count, dropped);
    } else {
        mesh_str_format(out, out_len, MESH_STR_LIST_TITLE_COUNT, name, count);
    }
}

/* ---- the switch -------------------------------------------------------------------------- */

/*
 * How long a knob takes to cross, and on what curve.
 *
 * 140 ms is the range a control that answers a button press wants to be in: long enough that
 * the eye follows the knob across rather than seeing it teleport, short enough that nobody
 * waits for it. Ease-out because the press has already happened - the movement is the screen
 * catching up, so it should leave briskly and settle, not wind up first.
 */
#define FB_SWITCH_MS 140U

/* Shorter than the line advance, which carries the gap between rows: a control as tall as the
   advance touches the row above it. */
static int fb_switch_height(const struct mesh_ui_backend_fb_state *state, int scale) {
    const int height = fb_line_adv(state, scale) - 2 * scale;
    return height < 6 ? 6 : height;
}

/*
 * The whole of the control's geometry, derived from the glyph metrics.
 *
 * A switch is a wide pill: 9:5 is close to what the phone platforms use and is what stops it
 * reading as a circle at a small scale. Everything else - the corner radius, the ring the knob
 * sits in, how far it travels - falls out of the height below, so a theme that asks for bigger
 * text gets a proportionally bigger control and no renderer is touched.
 */
void fb_switch_size(const struct mesh_ui_backend_fb_state *state, int scale, int *w, int *h) {
    const int height = fb_switch_height(state, scale);
    if (h != NULL) {
        *h = height;
    }
    if (w != NULL) {
        *w = height * 9 / 5;
    }
}

void fb_draw_switch(struct mesh_ui_backend_fb_state *state, const struct fb_switch *sw) {
    if (sw == NULL || sw->rect.w <= 0 || sw->rect.h <= 0) {
        return;
    }

    /*
     * Where the knob is, which is the one thing the snapshot cannot say. The table answers
     * with the target on first sight of this id and with a position in between afterwards -
     * so a screen opening does not animate and a press does.
     */
    const int32_t position =
        mesh_ui_anim_track(&state->anim, sw->id, state->now_ms, sw->on ? MESH_UI_ANIM_ONE : 0,
                           FB_SWITCH_MS, MESH_UI_EASE_OUT);

    const int radius = sw->rect.h / 2;

    /*
     * Its own ground under a cursor fill. Both colour pairs below are contracted against the
     * ground rather than against the cursor fill, and on the dark and colorblind themes the
     * cursor fill and the resting track are the same colour - so without this the control
     * would be invisible on precisely the row being pointed at.
     */
    if (sw->selected) {
        /* The ring is a fraction of the glyph scale rather than of the control, because the row
           fill it has to stay inside is measured from the scale too - a ring sized off the
           control overhung the highlight bar top and bottom and notched it. */
        const int pad = state->scale / 2 > 0 ? state->scale / 2 : 1;
        fb_fill_round_rect(state, sw->rect.x - pad, sw->rect.y - pad, sw->rect.w + 2 * pad,
                           sw->rect.h + 2 * pad, radius + pad, fb_color(state, MESH_UI_COLOR_BG));
    }

    /*
     * The track takes its colour from the end the knob has passed, rather than fading between
     * the two as it travels.
     *
     * A fade was the first thing tried and it is wrong for a reason worth writing down: the
     * pairs below are validated against each other, and a track halfway between two of them is
     * not validated against either knob colour - so the one moment the eye is actually
     * following the control is the moment its contrast is unaccounted for. It also looks bad,
     * because on the dark theme the two ends are yellow and blue and everything between them
     * is mud. Flipping at the midpoint keeps every frame a pair a theme was held to.
     *
     * A dim switch reports a state rather than offering one, so it stays muted at both ends and
     * never takes the accent.
     */
    const bool past_middle = position > MESH_UI_ANIM_ONE / 2;
    const enum mesh_ui_color track_role =
        sw->dim ? (past_middle ? MESH_UI_COLOR_RULE_STRONG : MESH_UI_COLOR_RULE)
                : (past_middle ? MESH_UI_COLOR_ACCENT : MESH_UI_COLOR_SURFACE_SEL);
    const enum mesh_ui_color knob_role =
        sw->dim ? MESH_UI_COLOR_TEXT_DIM
                : (past_middle ? MESH_UI_COLOR_ON_ACCENT : MESH_UI_COLOR_TEXT_ON_SEL);
    fb_fill_round_rect(state, sw->rect.x, sw->rect.y, sw->rect.w, sw->rect.h, radius,
                       fb_color(state, track_role));

    /* The knob: a disc inside a ring of track, travelling the width less that ring. */
    const int inset = sw->rect.h / 8 + 1;
    const int knob = sw->rect.h - 2 * inset;
    const int travel = sw->rect.w - 2 * inset - knob;
    const int x = sw->rect.x + inset + (travel > 0 ? (travel * position) / MESH_UI_ANIM_ONE : 0);
    fb_fill_round_rect(state, x, sw->rect.y + inset, knob, knob, knob / 2,
                       fb_color(state, knob_role));
}
