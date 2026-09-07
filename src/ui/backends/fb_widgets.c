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
    struct mesh_ui_paint paint;
};

static struct fb_button_paint fb_button_paint(const struct mesh_ui_backend_fb_state *state,
                                              const struct fb_button *button) {
    const bool selected = button->selected;
    switch (button->variant) {
    case FB_BUTTON_FILLED:
        /* The neutral cursor surface, not a family: a keyboard key is a place to press, not a
           statement about what pressing it means. */
        return (struct fb_button_paint){
            true,
            {fb_color(state, selected ? MESH_UI_COLOR_SURFACE_ACTIVE : MESH_UI_COLOR_SURFACE_SEL),
             fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)}};
    case FB_BUTTON_TONAL:
        /* Under the cursor a tonal control commits to the family's full strength: the container
           is there so a label can be read over it, and a control being pressed has stopped
           being something to read. One call, so the fill and the ink cannot come from different
           halves of the family. */
        return (struct fb_button_paint){
            true,
            fb_paint(state, button->family, selected ? MESH_UI_SLOT_BASE : MESH_UI_SLOT_CONTAINER,
                     MESH_UI_STATE_REST)};
    case FB_BUTTON_TEXT:
    default:
        return selected ? (struct fb_button_paint){true,
                                                   {fb_color(state, MESH_UI_COLOR_SURFACE_ACTIVE),
                                                    fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)}}
                        : (struct fb_button_paint){false,
                                                   {fb_color(state, MESH_UI_COLOR_BG),
                                                    fb_color(state, MESH_UI_COLOR_TEXT)}};
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
    const struct fb_button_paint paint = fb_button_paint(state, button);
    if (paint.has_fill) {
        fb_fill_round_rect(state, button->rect.x, button->rect.y, button->rect.w, button->rect.h,
                           fb_radius(state, button->shape), paint.paint.fill);
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
        paint.has_fill ? paint.paint.ink : fb_tone_color(state, button->idle_tone);
    if (has_icon) {
        /* Blended against the fill the button has just laid down, or against whatever the
           caller says it is sitting on when it laid none - the two colours the ink was chosen
           against. */
        fb_draw_icon(state, x, y, button->icon, button->scale, ink,
                     paint.has_fill ? paint.paint.fill : fb_color(state, button->ground));
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

int fb_button_width(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_icon icon,
                    const char *label, int scale) {
    const struct fb_button button = {.icon = icon, .label = label, .scale = scale};
    return fb_button_content_w(state, &button) + FB_CHIP_PAD_STEPS * scale;
}

int fb_chip_width(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_icon icon,
                  const char *label, int scale) {
    /* The pill itself, then the gap before the next one. */
    return fb_button_width(state, icon, label, scale) + fb_char_adv(state, scale);
}

int fb_draw_chip(const struct mesh_ui_backend_fb_state *state, int x, int y, enum mesh_ui_icon icon,
                 const char *label, bool active, enum mesh_ui_color ground, int scale) {
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
        .ground = ground,
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
                 fb_tone_color(state, MESH_UI_TONE_PRIMARY));
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
    /* The window the rail measures, which is the body rather than the rows that happened to be
       filled: a list of three items in a body of fifteen has no rail at all, and a list of
       forty-two wants one the height of what a full window would have been. */
    list.track_y = layout->body_y;
    list.track_h = (int)layout->rows * layout->line;
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

/*
 * The scroll rail. Drawn once per list, by the first row that draws - see fb_widgets.h.
 *
 * It sits in the half-margin outside the row fill, so it costs no row a single cell: rows clip
 * their text at `xres - margin` and the cursor fill stops at `xres - margin / 2`, which leaves
 * this gutter free. Its width comes from the glyph scale like every other control here, so it
 * stays in proportion when a theme asks for bigger text.
 */
static void fb_list_rail(const struct mesh_ui_backend_fb_state *state, struct fb_list *list) {
    if (list == NULL || list->rail_drawn) {
        return;
    }
    list->rail_drawn = true;
    if (list->track_h <= 0) {
        return;
    }

    const int margin = fb_margin(state);
    /*
     * Sized from the gutter it lives in rather than from the glyph scale, unlike every other
     * control here. The gutter is half a margin wide and does not grow when a theme asks for
     * bigger text, so a rail measured in glyph steps ran out of clearance and ended up flush
     * against the panel edge at the larger scales. A quarter-margin leaves the same gap either
     * side at every scale, which is what makes it read as inset rather than as a screen edge.
     */
    int width = margin / 4;
    if (width < 2) {
        width = 2;
    }

    /* The proportion is mesh_ui_list_scroll()'s - no pixels in it, and unit tested there. A
       length of 0 is a list that fits, which draws nothing at all rather than a full track. */
    const struct mesh_ui_scroll scroll =
        mesh_ui_list_scroll(&list->model, list->track_h, 4 * width);
    if (scroll.length <= 0) {
        return;
    }

    /* Centred in the gutter between the row fill's right edge and the panel edge. */
    const int x = (int)state->var.xres - margin / 4 - width / 2;
    const int radius = fb_radius(state, MESH_UI_SHAPE_FULL);

    /* The track is the role that already means one - the same groove a meter's fill sits in -
       and the thumb is quiet ink, named as a tone the way an icon in a row slot is. That pairing
       is contract-checked: MESH_UI_TONE_DIM owes the ground 3:1 on every theme, so the thumb is
       findable on all four, which a second neutral role chosen by eye against the track was
       not. */
    fb_fill_round_rect(state, x, list->track_y, width, list->track_h, radius,
                       fb_color(state, MESH_UI_COLOR_METER_TRACK));
    fb_fill_round_rect(state, x, list->track_y + scroll.offset, width, scroll.length, radius,
                       fb_tone_color(state, MESH_UI_TONE_DIM));
}

bool fb_list_next(struct fb_list *list, uint32_t *index) {
    return mesh_ui_list_next(&list->model, index);
}

void fb_list_row(const struct mesh_ui_backend_fb_state *state, struct fb_list *list, uint32_t index,
                 const char *text, enum mesh_ui_tone tone) {
    fb_list_rail(state, list);
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
 * How wide an inline meter is, in cells.
 *
 * A length only means something against the container it is in, so the container has to be long
 * enough for the eye to divide: at four cells a half-full bar and a two-thirds-full one are the
 * same picture. Eight is about where they part company at this glyph scale, and it is still
 * short enough to leave a settings row its label and its value - which is the whole reason the
 * inline meter is a trailing slot and not a band across the row.
 */
#define FB_METER_INLINE_CELLS 8U

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
    case FB_TRAILING_METER:
        /* Stated in cells rather than measured from anything, because unlike a switch a bar has
           no natural width - it is as long as it is given. FB_METER_INLINE_CELLS is that
           choice, and the extra cell is the gap to the words. */
        want = trailing->meter != NULL ? FB_METER_INLINE_CELLS + 1U : 0U;
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
        /* One call for both halves: whatever the theme says reads on that family's own fill -
           on the dark palette that is the ground colour, because white on its yellow is
           unreadable at this glyph size. */
        const struct mesh_ui_paint badge =
            fb_paint(state, trailing->family, MESH_UI_SLOT_BASE, MESH_UI_STATE_REST);
        fb_fill_round_rect(state, x, slot_top, width, g->slot_h,
                           fb_radius(state, MESH_UI_SHAPE_FULL), badge.fill);
        fb_draw_text(state, x + adv / 2, baseline, trailing->text, scale, badge.ink);
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
    case FB_TRAILING_METER: {
        if (trailing->meter == NULL) {
            return;
        }
        /* Centred on the row's fill, exactly as the switch is and for the same reason: the fill
           is what the control has to stay inside. */
        const int height = fb_meter_thickness(state, scale);
        trailing->meter->rect.w = (int)FB_METER_INLINE_CELLS * adv;
        trailing->meter->rect.h = height;
        trailing->meter->rect.x = g->text_right - trailing->meter->rect.w;
        trailing->meter->rect.y = g->fill_top + (g->fill_h - height) / 2;
        trailing->meter->selected = selected;
        fb_draw_meter(state, trailing->meter);
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
    fb_list_rail(state, list);
    const int scale = state->scale;
    const bool selected = mesh_ui_list_is_cursor(&list->model, index);
    const struct fb_item_geom g = fb_item_measure(state, list, item);

    if (selected) {
        const int radius = fb_radius(state, MESH_UI_SHAPE_SM);
        const int row_x = fb_margin(state) / 2;
        const int row_w = (int)state->var.xres - fb_margin(state);
        if (item->accent_edge) {
            /*
             * The bar is laid the way a card's edge is: the marker shape first, the fill over
             * it a scale narrower on the left only. Both share their right edge, so the marker
             * survives just where the bar is meant to be - and it follows the corner instead
             * of poking a square end out of it, which is what a straight bar does once the row
             * has ends.
             */
            const enum mesh_ui_family edge_family = mesh_ui_tone_family(item->tone);
            fb_fill_round_rect(state, row_x, g.fill_top, row_w, g.fill_h, radius,
                               fb_tone_color(state, edge_family != MESH_UI_FAMILY_COUNT
                                                        ? item->tone
                                                        : MESH_UI_TONE_PRIMARY));
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
    fb_list_rail(state, list);
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
                .role = conversation->armed    ? MESH_UI_COLOR_ERROR
                        : conversation->accent ? MESH_UI_COLOR_PRIMARY
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
        .supporting_tone = conversation->armed    ? MESH_UI_TONE_ERROR
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
 * The fill and the ink a bubble is drawn in.
 *
 * Three cases, and each is a *container* rather than a colour of its own: ours is the secondary
 * family, one that failed is the error family, and one of theirs is the neutral raised tier -
 * because "somebody else said this" is not a verdict, and a hue there would compete with the
 * sender line, which is a verdict. The cursor is a state layer over whichever of the three it
 * is, not a second fill.
 *
 * This replaced five roles - three fills, two more for the same fills under the cursor - that
 * every theme had to state and match by eye against each other. The pairs it answers with are
 * the ones mesh_ui_theme_validate() already holds, selected included.
 */
static struct mesh_ui_paint fb_bubble_paint(const struct mesh_ui_backend_fb_state *state,
                                            const struct fb_bubble *bubble) {
    const enum mesh_ui_state ui_state =
        bubble->selected ? MESH_UI_STATE_SELECTED : MESH_UI_STATE_REST;
    if (bubble->failed) {
        return fb_paint(state, MESH_UI_FAMILY_ERROR, MESH_UI_SLOT_CONTAINER, ui_state);
    }
    if (bubble->outbound) {
        return fb_paint(state, MESH_UI_FAMILY_SECONDARY, MESH_UI_SLOT_CONTAINER, ui_state);
    }
    return (struct mesh_ui_paint){
        .fill = fb_state_layer(state, MESH_UI_COLOR_SURFACE_HIGH, MESH_UI_COLOR_TEXT, ui_state),
        .ink = fb_color(state, MESH_UI_COLOR_TEXT),
    };
}

/*
 * The colour for a bubble's quieter lines - the sender on one of ours, the clock on any of them.
 *
 * Dim while the bubble sits at rest, and the bubble's own ink once the cursor is on it or the
 * fill has gone red. The selected fills are a step towards their ink by design, and dim over
 * one of those is the pairing a theme has least room for: on the dark palette it measured
 * 1.9:1, well under the 3:1 a secondary line is held to, and the bubble's own ink is a pair the
 * theme is validated on by construction.
 */
static struct mesh_ui_rgb fb_bubble_quiet(const struct mesh_ui_backend_fb_state *state,
                                          const struct fb_bubble *bubble,
                                          struct mesh_ui_paint paint) {
    if (bubble->selected || bubble->failed) {
        return paint.ink;
    }
    return fb_tone_color(state, MESH_UI_TONE_DIM);
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

    const struct mesh_ui_paint paint = fb_bubble_paint(state, bubble);
    const struct mesh_ui_rgb fill = paint.fill;
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
                           fb_color(state, MESH_UI_COLOR_PRIMARY));
    }
    const int fill_x = (bubble->selected && !bubble->outbound) ? box_x + scale : box_x;
    const int fill_w = bubble->selected ? box_w - scale : box_w;
    fb_fill_round_rect(state, fill_x, y - scale, fill_w, box_h, radius, fill);

    const int text_x = box_x + pad;
    const struct mesh_ui_rgb body = paint.ink;

    if (fb_bubble_has(bubble->name)) {
        struct mesh_ui_line line;
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%s", bubble->name);
        mesh_ui_line_fit(&line, metrics.cols);
        /* Ours is dimmed and theirs takes the primary: on our own bubble the name is a
           reminder, on theirs it is the thing being looked for. An alert overrides both - it is
           the one bubble whose heading is the point rather than the label on the point. A
           bubble that failed takes its own ink for all three, because the fill has already said
           the only thing a hue on top of it could add. */
        struct mesh_ui_rgb name_color = fb_bubble_quiet(state, bubble, paint);
        if (!bubble->failed) {
            if (bubble->alert) {
                name_color = fb_tone_color(state, MESH_UI_TONE_ERROR);
            } else if (!bubble->outbound) {
                name_color = fb_tone_color(state, MESH_UI_TONE_PRIMARY);
            }
        }
        fb_draw_text(state, text_x, y, mesh_ui_line_text(&line), scale, name_color);
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
    const struct mesh_ui_rgb meta_color = fb_bubble_quiet(state, bubble, paint);
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
        /*
         * Every row that *uses* the label column, which is the question - not every row of one
         * kind. A note has no label and a meter may or may not have one, so the label itself is
         * what says whether the row is in this measurement, and a kind check here was a card of
         * labelled meters measuring its column against no labels at all and clipping each one
         * to a single cell. Measuring and drawing have to ask the same question.
         */
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

void fb_card_meter(struct fb_card *card, enum mesh_ui_tone tone, enum mesh_str_id label,
                   int32_t permille, uint32_t id) {
    struct fb_card_row *row = fb_card_next_row(card, FB_CARD_ROW_METER, tone);
    if (row == NULL) {
        return;
    }
    if (label != MESH_STR_NONE) {
        mesh_text_sanitise_str(mesh_str(label), row->label, sizeof row->label);
    }
    row->meter_value = permille;
    row->meter_id = id;
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
static uint32_t fb_draw_card_row(struct mesh_ui_backend_fb_state *state,
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

    if (row->kind == FB_CARD_ROW_METER) {
        /*
         * A label in its own column and the bar across the rest, or - with no label - the bar
         * across the card's whole content width. Which of the two is the caller's sentence
         * about what the bar is for; see fb_card_meter().
         *
         * Vertically it sits on the middle of the line the text would have used, which is what
         * keeps a card of rows evenly spaced whether or not one of them is a bar.
         */
        const int adv = fb_char_adv(state, state->scale);
        int bar_x = m->content_x;
        if (row->label[0] != '\0') {
            struct mesh_ui_line line;
            mesh_ui_line_reset(&line);
            mesh_ui_line_column(&line, row->label, m->label_cols);
            mesh_ui_line_fit(&line, m->cols);
            fb_draw_text(state, m->content_x, y, mesh_ui_line_text(&line), state->scale, color);
            bar_x = m->content_x + (int)(m->label_cols + 1U) * adv;
        }
        const int bar_right = m->content_x + (int)m->cols * adv;
        const int height = fb_meter_thickness(state, state->scale);
        if (bar_right - bar_x > 0) {
            const struct fb_meter meter = {
                .rect = {.x = bar_x,
                         .y = y + (layout->line - state->scale - height) / 2,
                         .w = bar_right - bar_x,
                         .h = height},
                .id = row->meter_id,
                .kind = FB_METER_DETERMINATE,
                .value = row->meter_value,
                .tone = row->tone,
            };
            fb_draw_meter(state, &meter);
        }
        return 1U;
    }

    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    mesh_ui_line_column(&line, row->label, m->label_cols);
    mesh_ui_line_printf(&line, " %s", row->value);
    mesh_ui_line_fit(&line, m->cols);
    fb_draw_text(state, m->content_x, y, mesh_ui_line_text(&line), state->scale, color);
    return 1U;
}

bool fb_draw_card(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout, int *y,
                  const struct fb_card *card) {
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

/* ---- the snackbar ------------------------------------------------------------------------ */

/*
 * The snackbar's key in the animation table.
 *
 * Every other id in there is a control's identity handed in by a screen - a settings field, a
 * row index - and those are small enumerator values. There is exactly one snackbar and no
 * screen owns it, so it takes a constant from the far end of the range, where nothing a screen
 * can pass will ever land.
 */
#define FB_ANIM_ID_SNACKBAR 0xFFFFFF01U

/*
 * How long it takes to arrive, and how long to leave.
 *
 * Not the same token, and the asymmetry is the point: arriving is the part that has to be
 * seen, leaving is the part that has to be out of the way. It is the shape of every platform's
 * transient-notice motion. The theme says how long each is (enum mesh_ui_motion); what belongs
 * here is only which of the two kinds of movement this is.
 */
#define FB_SNACKBAR_IN_MOTION MESH_UI_MOTION_MEDIUM
#define FB_SNACKBAR_OUT_MOTION MESH_UI_MOTION_SHORT

/* Material allows one line or two, and two is where a notice stops being one on a 3.2" panel.
   Anything longer is clipped rather than allowed to grow into the body. */
#define FB_SNACKBAR_LINES_MAX 2U

void fb_draw_snackbar(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                      const struct fb_snackbar *bar) {
    if (state == NULL || layout == NULL) {
        return;
    }

    const char *const text = (bar != NULL && bar->text != NULL) ? bar->text : "";
    const bool showing = (text[0] != '\0');

    if (showing &&
        (bar->until_ms != state->snackbar_until_ms || strcmp(text, state->snackbar) != 0)) {
        /*
         * A different notice, so it *arrives* rather than swapping its words.
         *
         * The table is put back to nothing with a zero duration first, then aimed at the
         * resting place below - which is the only way to get an entrance out of it, since
         * re-aiming at a target it already holds is deliberately a no-op and a slot seen for
         * the first time adopts its target rather than animating to it. Both of those are what
         * stops a switch sliding on the frame a screen opens; here they have to be stepped
         * around, because a notice appearing is exactly the thing worth showing.
         */
        (void)mesh_str_copy(state->snackbar, sizeof state->snackbar, text);
        state->snackbar_until_ms = bar->until_ms;
        (void)mesh_ui_anim_track(&state->anim, FB_ANIM_ID_SNACKBAR, state->now_ms, 0, 0U,
                                 MESH_UI_EASE_OUT);
    }
    if (state->snackbar[0] == '\0') {
        return;
    }

    const int32_t position = mesh_ui_anim_track(
        &state->anim, FB_ANIM_ID_SNACKBAR, state->now_ms, showing ? MESH_UI_ANIM_ONE : 0,
        fb_motion(state, showing ? FB_SNACKBAR_IN_MOTION : FB_SNACKBAR_OUT_MOTION),
        MESH_UI_EASE_OUT);
    if (!showing && position == 0) {
        /* All the way out. The store forgot the words several frames ago; now so does this,
           and the next notice starts from an empty slot rather than from this one's. */
        state->snackbar[0] = '\0';
        state->snackbar_until_ms = 0U;
        return;
    }

    const int scale = state->scale;
    const int adv = fb_char_adv(state, scale);
    const int line = fb_line_adv(state, scale);
    const int margin = fb_margin(state);
    /* A cell in from each edge and a proportional band above and below: a container's padding
       is measured in the same units as what it holds, so a theme asking for bigger text gets a
       proportionally roomier bar rather than a tighter one. */
    const int pad_x = adv;
    const int pad_y = scale * 2;

    const int room = (int)state->var.xres - 2 * margin - 2 * pad_x;
    if (room < adv || line <= 0) {
        return; /* a geometry too small to hold one cell of it; nothing to say here */
    }
    const size_t max_cols = (size_t)(room / adv);

    uint32_t lines = mesh_ui_wrap_lines(state->snackbar, max_cols);
    if (lines == 0U) {
        lines = 1U;
    }
    if (lines > FB_SNACKBAR_LINES_MAX) {
        lines = FB_SNACKBAR_LINES_MAX;
    }
    /* Sized to its own words rather than to the panel, the way a bubble is: a bar the full
       width of the screen is a region of the chrome, and a notice is one thing that arrived. */
    size_t cols = mesh_ui_wrap_widest(state->snackbar, max_cols);
    if (cols == 0U) {
        cols = 1U;
    }

    const int box_w = (int)cols * adv + 2 * pad_x;
    /* Centred on the panel rather than against the leading margin. A bar sized to its own words
       and pinned to the left edge reads as the start of a row that ran out of things to say -
       which is what the footer line it replaced was. Centred, it reads as one object placed
       over the screen, and it stays put as the wording changes length instead of growing
       rightwards out of a fixed corner. */
    const int box_x = ((int)state->var.xres - box_w) / 2;
    const int box_h = (int)lines * line + 2 * pad_y;
    /* Where it comes to rest: over the bottom of the body, a full margin clear of the footer.
       A card stops half a margin short of the footer because a card is *in* the body and the
       body's own bottom edge is where it belongs; this one is over everything, so it keeps the
       distance the panel edge keeps rather than the one the body does. */
    const int rest_y = layout->footer_y - margin - box_h;
    /* And where it comes from: entirely below the panel. A container that slid in from just
       off its resting place reads as a nudge; one that comes up from off-screen reads as
       something arriving, which is the whole of what this shape is for. */
    const int off_y = (int)state->var.yres;
    const int y = off_y + (int)(((int64_t)(rest_y - off_y) * position) / MESH_UI_ANIM_ONE);

    /*
     * The inverted surface, and no edge on it.
     *
     * A card needs a hairline because its fill is one step off the ground and the step alone is
     * not findable in daylight. This one is the other end of the palette - the furthest from
     * the ground the theme has - so the fill is the whole cue, and an outline over it would be
     * drawing a border around the most obvious thing on the panel.
     */
    fb_fill_round_rect(state, box_x, y, box_w, box_h, fb_radius(state, MESH_UI_SHAPE_SM),
                       fb_color(state, MESH_UI_COLOR_SURFACE_INVERSE));

    const struct mesh_ui_rgb ink = fb_color(state, MESH_UI_COLOR_TEXT_ON_INVERSE);
    struct mesh_ui_wrap wrap;
    mesh_ui_wrap_begin(&wrap, state->snackbar, max_cols);
    int text_y = y + pad_y;
    for (uint32_t drawn = 0U; drawn < lines && mesh_ui_wrap_next(&wrap); ++drawn) {
        fb_draw_text(state, box_x + pad_x, text_y, wrap.line, scale, ink);
        text_y += line;
    }
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
 * The short token: what a control answering a button press wants - long enough that the eye
 * follows the knob across rather than seeing it teleport, short enough that nobody waits for
 * it. Ease-out because the press has already happened; the movement is the screen catching up,
 * so it should leave briskly and settle rather than wind up first.
 */
#define FB_SWITCH_MOTION MESH_UI_MOTION_SHORT

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
                           fb_motion(state, FB_SWITCH_MOTION), MESH_UI_EASE_OUT);

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
    const struct mesh_ui_paint on_paint =
        fb_paint(state, sw->family, MESH_UI_SLOT_BASE, MESH_UI_STATE_REST);
    struct mesh_ui_rgb track;
    struct mesh_ui_rgb knob;
    if (sw->dim) {
        track = fb_color(state, past_middle ? MESH_UI_COLOR_RULE_STRONG : MESH_UI_COLOR_RULE);
        knob = fb_color(state, MESH_UI_COLOR_TEXT_DIM);
    } else if (past_middle) {
        /* The on end is the family, fill and knob taken from the one pair. */
        track = on_paint.fill;
        knob = on_paint.ink;
    } else {
        track = fb_color(state, MESH_UI_COLOR_SURFACE_SEL);
        knob = fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL);
    }
    fb_fill_round_rect(state, sw->rect.x, sw->rect.y, sw->rect.w, sw->rect.h, radius, track);

    /* The knob: a disc inside a ring of track, travelling the width less that ring. */
    const int inset = sw->rect.h / 8 + 1;
    const int knob_size = sw->rect.h - 2 * inset;
    const int travel = sw->rect.w - 2 * inset - knob_size;
    const int x = sw->rect.x + inset + (travel > 0 ? (travel * position) / MESH_UI_ANIM_ONE : 0);
    fb_fill_round_rect(state, x, sw->rect.y + inset, knob_size, knob_size, knob_size / 2, knob);
}

/* ---- the meter ----------------------------------------------------------------------------- */

/*
 * How long a determinate fill takes to reach a new reading.
 *
 * Longer than the switch's, and for the opposite reason. A switch answers a press, so it has to
 * feel immediate; a meter answers a *sample*, and the samples are seconds apart - the airtime
 * figures arrive on the radio's own schedule, the download's byte count is whatever the file on
 * disk had grown to when the loop last looked. Easing across that gap is the whole trick: the
 * bar spends its time moving between two readings instead of sitting still and then jumping,
 * and what the eye gets is the rate rather than the samples.
 */
#define FB_METER_MOTION MESH_UI_MOTION_LONG

/* One pass of an indeterminate pill. Slow enough to read as travel rather than as flicker, fast
   enough that a screen with one on it does not feel stalled. */
#define FB_METER_LOOP_MOTION MESH_UI_MOTION_LOOP

/* How much of the track the pill covers. A third is the proportion Material's indeterminate bar
   settles at, and it is about the shortest that still reads as a bar rather than as a dot. */
#define FB_METER_PILL_PCT 34

/*
 * Where in its cycle the pill starts, in permille.
 *
 * A loop begins at 0, which for the travel below is the pill entirely off the leading edge -
 * so a bar drawn on the frame it first appears would be an empty track, and the one frame that
 * has to say "this is working" would say the opposite. Roughly two fifths in is where the eased
 * travel first brings the whole pill onto the track, so the widget appears with something in
 * it and loops normally from there.
 *
 * A rotation of a periodic function, not a special case for the first frame: every cycle starts
 * here, so nothing has to remember whether this is the first one.
 */
#define FB_METER_PILL_PHASE 400

int fb_meter_thickness(const struct mesh_ui_backend_fb_state *state, int scale) {
    const int thickness = (int)fb_metrics(state)->meter_thickness * (scale > 0 ? scale : 1);
    return thickness > 1 ? thickness : 1;
}

/*
 * The fill's colour.
 *
 * A family tone and nothing else. The neutral three - normal, dim, strong - fold back to the
 * primary rather than being drawn as asked, and that is not defensiveness: a fill nobody has
 * validated against MESH_UI_COLOR_METER_TRACK is a bar that vanishes on some theme somebody has
 * not opened yet, and the primary is the one answer that is always right for "something is
 * here". Every family *is* validated against the track, so this is now a question about the
 * kind of tone rather than a list to keep in step with the validator - which is what the list
 * that used to be here was, and it went stale the moment a fourth fill existed.
 */
static enum mesh_ui_tone fb_meter_tone(enum mesh_ui_tone tone) {
    return mesh_ui_tone_family(tone) != MESH_UI_FAMILY_COUNT ? tone : MESH_UI_TONE_PRIMARY;
}

void fb_draw_meter(struct mesh_ui_backend_fb_state *state, const struct fb_meter *meter) {
    if (meter == NULL || meter->rect.w <= 0 || meter->rect.h <= 0) {
        return;
    }
    const struct fb_rect r = meter->rect;
    /* A pill, always: a bar with square ends reads as a region of the screen that has been
       filled in, and one with round ends reads as a quantity in a container. */
    const int radius = fb_radius(state, MESH_UI_SHAPE_FULL);

    /*
     * Its own ground under a cursor fill, for the reason the switch lays one.
     *
     * The track is validated against the body and against a card, which are the two grounds a
     * meter is drawn on - not against the cursor fill, and on two of the four themes it *is*
     * the cursor fill. Without this the track would disappear on precisely the row being
     * pointed at, leaving a fill floating in space with no length to be read against.
     */
    if (meter->selected) {
        const int pad = state->scale / 2 > 0 ? state->scale / 2 : 1;
        fb_fill_round_rect(state, r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad, radius + pad,
                           fb_color(state, MESH_UI_COLOR_BG));
    }

    fb_fill_round_rect(state, r.x, r.y, r.w, r.h, radius,
                       fb_color(state, MESH_UI_COLOR_METER_TRACK));

    const struct mesh_ui_rgb ink = fb_tone_color(state, fb_meter_tone(meter->tone));

    if (meter->kind == FB_METER_INDETERMINATE) {
        /*
         * A pill crossing the track, from entirely off the leading edge to entirely off the
         * trailing one. Both ends of the travel are off the track on purpose: the loop's wrap
         * from ONE back to 0 then happens while nothing is drawn, so a bar that never finishes
         * also never visibly restarts.
         *
         * Eased rather than linear, so it accelerates in and settles out instead of sliding at
         * one speed - which is the difference between a thing that is working and a thing on a
         * conveyor belt.
         */
        const int pill = r.w * FB_METER_PILL_PCT / 100 > 1 ? r.w * FB_METER_PILL_PCT / 100 : 1;
        /* The phase is added before the curve, not after: shifting the sawtooth rotates where
           the cycle begins and leaves the wrap exactly where it was - at the point the pill is
           off the track entirely, which is what keeps a loop that never ends from visibly
           restarting. Shifting the eased value instead would put a jump in the middle of the
           travel. */
        const int32_t phase = (mesh_ui_anim_loop(&state->anim, meter->id, state->now_ms,
                                                 fb_motion(state, FB_METER_LOOP_MOTION)) +
                               FB_METER_PILL_PHASE) %
                              MESH_UI_ANIM_ONE;
        const int32_t t = mesh_ui_ease(MESH_UI_EASE_IN_OUT, phase);
        const int travel = r.w + pill;
        int x = r.x - pill + (int)(((int64_t)travel * t) / MESH_UI_ANIM_ONE);
        int w = pill;
        /* Clipped to the track rather than drawn past it: fb_fill_round_rect() is happy to fill
           outside a container it knows nothing about, and the container here is the thing that
           gives the pill its meaning. */
        if (x < r.x) {
            w -= r.x - x;
            x = r.x;
        }
        if (x + w > r.x + r.w) {
            w = r.x + r.w - x;
        }
        if (w > 0) {
            fb_fill_round_rect(state, x, r.y, w, r.h, radius, ink);
        }
        return;
    }

    /* Where the fill has got to, which is not where the reading is: see FB_METER_MOTION. */
    int32_t value = meter->value;
    if (value < 0) {
        value = 0;
    } else if (value > MESH_UI_ANIM_ONE) {
        value = MESH_UI_ANIM_ONE;
    }
    const int32_t position =
        mesh_ui_anim_track(&state->anim, meter->id, state->now_ms, value,
                           fb_motion(state, FB_METER_MOTION), MESH_UI_EASE_OUT);

    int fill = (int)(((int64_t)r.w * position) / MESH_UI_ANIM_ONE);
    if (fill <= 0) {
        /* A reading that is not zero draws something, however small. Rounding a real 0.4% down
           to no pixels at all says "nothing is happening", which is the one thing the bar is
           there to distinguish from. Exactly zero draws an empty track, as it should. */
        fill = position > 0 ? 1 : 0;
    }
    if (fill > 0) {
        fb_fill_round_rect(state, r.x, r.y, fill, r.h, radius, ink);
    }
}

/* ---- the text field ------------------------------------------------------------------------ */

/* The box's own height, without the label over it or the counter under it. */
static int fb_text_field_box_h(const struct mesh_ui_backend_fb_state *state,
                               const struct fb_layout *layout, const struct fb_text_field *field) {
    const uint32_t lines = field->lines > 0U ? field->lines : 1U;
    return (int)lines * layout->line + state->scale;
}

/* The label's line, and the counter's. Both are chrome: they report on the field rather than
   being part of what is in it, so they are drawn at the smaller scale the footer and the tab
   strip use. Zero when the field carries neither. */
static int fb_text_field_label_h(const struct mesh_ui_backend_fb_state *state,
                                 const struct fb_layout *layout,
                                 const struct fb_text_field *field) {
    return (field->label != NULL && field->label[0] != '\0') ? fb_line_adv(state, layout->small)
                                                             : 0;
}

static int fb_text_field_counter_h(const struct mesh_ui_backend_fb_state *state,
                                   const struct fb_layout *layout,
                                   const struct fb_text_field *field) {
    return (field->counter != NULL && field->counter[0] != '\0') ? fb_line_adv(state, layout->small)
                                                                 : 0;
}

int fb_text_field_height(const struct mesh_ui_backend_fb_state *state,
                         const struct fb_layout *layout, const struct fb_text_field *field) {
    if (field == NULL) {
        return 0;
    }
    return fb_text_field_label_h(state, layout, field) + fb_text_field_box_h(state, layout, field) +
           fb_text_field_counter_h(state, layout, field) + state->scale;
}

void fb_draw_text_field(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout, int *y, const struct fb_text_field *field) {
    if (field == NULL || y == NULL) {
        return;
    }
    const int scale = state->scale;
    const int margin = fb_margin(state);
    const int box_x = margin / 2;
    const int box_w = (int)state->var.xres - margin;
    const int box_h = fb_text_field_box_h(state, layout, field);
    const uint32_t lines = field->lines > 0U ? field->lines : 1U;
    int top = *y;

    if (fb_text_field_label_h(state, layout, field) > 0) {
        fb_draw_text(state, margin, top, field->label, layout->small,
                     fb_tone_color(state, field->error ? MESH_UI_TONE_ERROR : MESH_UI_TONE_DIM));
        top += fb_text_field_label_h(state, layout, field);
    }

    /*
     * The edge first and the fill inside it, exactly as a card is built: there is no stroke
     * primitive here, and a rounded outline is the shape drawn once in the edge colour with the
     * shape drawn again a hairline smaller over the top of it.
     */
    const int edge = fb_edge(state);
    const int radius = fb_radius(state, MESH_UI_SHAPE_MD);
    fb_fill_round_rect(state, box_x, top, box_w, box_h, radius + edge,
                       fb_color(state, field->error ? MESH_UI_COLOR_ERROR : MESH_UI_COLOR_OUTLINE));
    fb_fill_round_rect(state, box_x + edge, top + edge, box_w - 2 * edge, box_h - 2 * edge, radius,
                       fb_color(state, MESH_UI_COLOR_SURFACE_HIGH));

    /*
     * The value, with the caret on the end of it, showing the *tail* when it is longer than the
     * box holds: the caret is where the next press lands, so it is the end that has to stay on
     * screen. Measured and cut in cells, so a draft of emoji scrolls a glyph at a time rather
     * than splitting one down the middle.
     */
    char shown[MESH_UI_LINE_MAX];
    snprintf(shown, sizeof shown, "%s%s", field->value != NULL ? field->value : "",
             field->caret ? "_" : "");
    const size_t visible = layout->cols * (size_t)lines;
    const char *tail = shown;
    const size_t width = mesh_ui_text_cells(shown);
    if (width > visible) {
        tail = shown + mesh_ui_text_cell_offset(shown, width - visible);
    }
    /* The value sits a scale down from the box's own top edge, which is the inset every other
       container here gives its contents. */
    fb_draw_wrapped(state, top + scale, tail, layout->cols, (int)lines,
                    fb_tone_color(state, MESH_UI_TONE_STRONG));
    top += box_h;

    if (fb_text_field_counter_h(state, layout, field) > 0) {
        /* Against the box's own trailing edge rather than the body's - it belongs to the field,
           and a figure that does not line up with the container it reports on reads as loose. */
        const int adv = fb_char_adv(state, layout->small);
        const int x = box_x + box_w - (int)mesh_ui_text_cells(field->counter) * adv;
        fb_draw_text(state, x, top, field->counter, layout->small,
                     fb_tone_color(state, field->error ? MESH_UI_TONE_ERROR : MESH_UI_TONE_DIM));
        top += fb_text_field_counter_h(state, layout, field);
    }

    *y = top + scale;
}

/* ---- the dialog ---------------------------------------------------------------------------- */

/* How large the dialog's icon is drawn, in glyph-scale steps. Big enough to be the first thing
   read - it is the only thing on the panel that says what *kind* of question this is before a
   word of it has been - and clamped by fb_draw_icon() to what the sprite can carry. */
#define FB_DIALOG_ICON_SCALE 3

/*
 * `src` shortened until the button holding it fits `max_w`.
 *
 * Measured with fb_button_width() rather than by dividing the width by a cell, because a
 * button's padding and the gap after its icon are the button's business - a caller that did the
 * arithmetic itself would disagree with the thing it is sizing the moment either changes.
 *
 * Cut on cell boundaries, so a label with an emoji or an accented character in it loses a whole
 * glyph rather than half of a UTF-8 sequence. A label that cannot be made to fit at all keeps
 * its first cell: something is drawn, and the button still marks where the press lands.
 */
static void fb_fit_button_label(const struct mesh_ui_backend_fb_state *state,
                                enum mesh_ui_icon icon, const char *src, int scale, int max_w,
                                char *out, size_t out_len) {
    mesh_str_copy(out, out_len, src != NULL ? src : "");
    while (fb_button_width(state, icon, out, scale) > max_w) {
        const size_t cells = mesh_ui_text_cells(out);
        if (cells <= 1U) {
            return;
        }
        mesh_ui_text_cell_truncate(out, cells - 1U);
    }
}

void fb_draw_dialog(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                    const struct fb_dialog *dialog) {
    if (dialog == NULL) {
        return;
    }
    const int scale = state->scale;
    const int margin = fb_margin(state);
    const int line = layout->line;
    const int edge = fb_edge(state);
    const int pad = margin;
    const int adv = fb_char_adv(state, scale);

    /* One decision for the whole panel: the icon, the headline and the accept button's fill all
       come off this, so a destructive dialog cannot end up half red. */
    const enum mesh_ui_family accept_family =
        dialog->destructive ? MESH_UI_FAMILY_ERROR : MESH_UI_FAMILY_PRIMARY;
    const enum mesh_ui_tone accent_tone = mesh_ui_family_tone(accept_family);

    const int panel_x = margin / 2;
    const int panel_w = (int)state->var.xres - margin;
    /* The panel's own text column, which is narrower than the body's - a dialog is inset from
       the screen and its words are inset again from its edge. */
    const int text_w = panel_w - 2 * pad;
    const size_t text_cols = text_w > adv ? (size_t)(text_w / adv) : 1U;

    const int icon_scale = scale * FB_DIALOG_ICON_SCALE;
    const bool has_icon = mesh_ui_icon_is_valid(dialog->icon);
    const int icon_h = has_icon ? fb_line_adv(state, icon_scale) : 0;
    const bool has_headline = dialog->headline != NULL && dialog->headline[0] != '\0';
    /* A little more than a line: the headline and the paragraph under it are the same glyph
       size, so the gap is the only thing distinguishing a question from its explanation. */
    const int head_h = has_headline ? line + scale : 0;
    const int button_h = line + 2 * scale;
    const int room = layout->footer_y - layout->body_y - line;

    /*
     * The action row, measured before the panel is: how tall the panel has to be depends on
     * whether the two answers fit beside each other.
     *
     * They do not always. "Reset the node database" at the largest glyph scale wants more than
     * the whole panel on its own, and side by side with Cancel it ran the cancel button off the
     * left-hand edge of the *screen* - on a destructive confirmation, where the safe answer is
     * the one that disappeared. So the row stacks when it has to, which is what every platform
     * does with dialog actions too long to sit in a line, and each label is fitted to the panel
     * so a single button can never overhang it either.
     */
    const int gap = scale * 2;
    char accept_label[MESH_UI_LINE_MAX];
    char cancel_label[MESH_UI_LINE_MAX];
    fb_fit_button_label(state, MESH_UI_ICON_CHECK, dialog->accept, scale, text_w, accept_label,
                        sizeof accept_label);
    fb_fit_button_label(state, MESH_UI_ICON_CLOSE, dialog->cancel, scale, text_w, cancel_label,
                        sizeof cancel_label);
    const int cancel_w = fb_button_width(state, MESH_UI_ICON_CLOSE, cancel_label, scale);
    const int accept_w = fb_button_width(state, MESH_UI_ICON_CHECK, accept_label, scale);
    /* Stacked puts the answer that acts on top, the way a stacked dialog orders them - the
       dismissive one stays nearest the thumb. */
    const bool stacked = (accept_w + gap + cancel_w) > text_w;
    const int actions_h = stacked ? (2 * button_h + gap) : button_h;

    /*
     * Measure before drawing, as a card does and for the same reason: the fill has to go down
     * before the text, and how tall it is depends on how far the paragraph wraps.
     *
     * The paragraph is also the only part that gives way. Everything else on the panel is
     * either the question or the answers, and a dialog that dropped one of its buttons to fit
     * its explanation would be unanswerable - so the buttons, the headline and the icon are
     * reserved first and the supporting text takes what is left.
     */
    const int fixed = pad + icon_h + head_h + line / 2 + actions_h + pad;
    const int text_room = room - fixed;
    const uint32_t fits = text_room > 0 ? (uint32_t)(text_room / line) : 0U;
    uint32_t text_lines = (dialog->text != NULL && dialog->text[0] != '\0')
                              ? mesh_ui_wrap_lines(dialog->text, text_cols)
                              : 0U;
    if (text_lines > fits) {
        text_lines = fits;
    }

    int panel_h = fixed + (int)text_lines * line;
    if (panel_h > room) {
        panel_h = room;
    }
    /* Centred in what is left of the body, which is what makes it read as something placed over
       the screen rather than as the screen's own first rows. */
    const int panel_y = layout->body_y + (room > panel_h ? (room - panel_h) / 2 : 0);

    const int radius = fb_radius(state, MESH_UI_SHAPE_LG);
    fb_fill_round_rect(state, panel_x, panel_y, panel_w, panel_h, radius + edge,
                       fb_color(state, MESH_UI_COLOR_OUTLINE));
    fb_fill_round_rect(state, panel_x + edge, panel_y + edge, panel_w - 2 * edge,
                       panel_h - 2 * edge, radius, fb_color(state, MESH_UI_COLOR_SURFACE_HIGH));

    const int content_x = panel_x + pad;
    int y = panel_y + pad;

    if (has_icon) {
        fb_draw_icon(state, content_x, y, dialog->icon, icon_scale,
                     fb_tone_color(state, accent_tone),
                     fb_color(state, MESH_UI_COLOR_SURFACE_HIGH));
        y += icon_h;
    }

    if (has_headline) {
        struct mesh_ui_line headline;
        mesh_ui_line_reset(&headline);
        mesh_ui_line_printf(&headline, "%s", dialog->headline);
        mesh_ui_line_fit(&headline, text_cols);
        fb_draw_text(state, content_x, y, mesh_ui_line_text(&headline), scale,
                     fb_tone_color(state, accent_tone));
        y += head_h;
    }

    if (text_lines > 0U) {
        fb_draw_wrapped_at(state, content_x, y, dialog->text, text_cols, (int)text_lines,
                           fb_tone_color(state, MESH_UI_TONE_NORMAL));
    }

    /*
     * Side by side, the answer that does nothing goes on the left and the one that acts on the
     * right, so the press that costs something is never the one nearest a thumb resting where
     * it was. Stacked, the same reasoning puts the acting answer on top.
     */
    const int right = panel_x + panel_w - pad;
    const int accept_y = panel_y + panel_h - pad - (stacked ? (2 * button_h + gap) : button_h);
    const int cancel_y = stacked ? (accept_y + button_h + gap) : accept_y;
    const int accept_x = right - accept_w;
    const int cancel_x = stacked ? (right - cancel_w) : (accept_x - gap - cancel_w);

    const struct fb_button cancel = {
        .rect = {cancel_x, cancel_y, cancel_w, button_h},
        .icon = MESH_UI_ICON_CLOSE,
        .label = cancel_label,
        .selected = dialog->cursor != 0U,
        .variant = FB_BUTTON_TEXT,
        .shape = MESH_UI_SHAPE_FULL,
        .idle_tone = MESH_UI_TONE_NORMAL,
        .ground = MESH_UI_COLOR_SURFACE_HIGH,
        .scale = scale,
    };
    fb_draw_button(state, &cancel);

    /*
     * Exactly one of the two carries a fill, and it is always the focused one.
     *
     * The obvious design gives the accept a standing fill so it reads as the proposed answer,
     * and lets the cursor pick between them. That works on three of the four themes and fails
     * on the one where it matters most: the high-contrast palette deliberately collapses
     * PRIMARY, PRIMARY_CONTAINER and SURFACE_ACTIVE onto a single yellow, because a theme built
     * for legibility does not have a "held back" version of its one accent. Two buttons whose
     * fills both resolve to that yellow are two buttons a user cannot tell apart, on the theme
     * chosen by the people least able to guess.
     *
     * So the fill means focus and nothing else, and what marks the accept as the affirmative is
     * its check and its family-coloured label - a cue that survives every palette because it is
     * ink on the panel rather than one fill against another.
     */
    const bool accept_selected = dialog->cursor == 0U;
    const struct fb_button accept = {
        .rect = {accept_x, accept_y, accept_w, button_h},
        .icon = MESH_UI_ICON_CHECK,
        .label = accept_label,
        .selected = accept_selected,
        .variant = accept_selected ? FB_BUTTON_TONAL : FB_BUTTON_TEXT,
        /* The dialog's family, so a destructive confirm is a red pill rather than a red word on
           the ordinary one - and the ink on it is the one checked against that red. */
        .family = accept_family,
        .shape = MESH_UI_SHAPE_FULL,
        .idle_tone = accent_tone,
        .ground = MESH_UI_COLOR_SURFACE_HIGH,
        .scale = scale,
    };
    fb_draw_button(state, &accept);
}
