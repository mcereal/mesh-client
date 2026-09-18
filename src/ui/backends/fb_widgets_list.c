#define _POSIX_C_SOURCE 200809L

/*
 * The list window and its furniture - and the disc, which a card heading draws here and a row's
 * leading slot draws in fb_widgets_item.c, and which is therefore neither's.
 */

#include "fb_widgets_list.h"
#include "fb_widgets_list_internal.h"

#include "mesh/ui/emoji.h"
#include "mesh/ui/layout.h"

#include <string.h>

/* ---- the disc ------------------------------------------------------------------------------
 *
 * Shared by two components and therefore neither's: a list row's leading slot draws one, and so
 * does a card heading over rows that carry them.
 */

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
 * An icon takes the same ink the initials do and is blended over the fill it is standing on.
 *
 * The fill and that ink arrive as a pair rather than as a tint, which is what lets one disc
 * serve two meanings. An avatar's is a tint from the theme's avatar palette with the ground
 * colour on it, every one of which is validated that way; a leading tonal container's is a
 * family's held-back half with the ink that family states - and a component that took a colour
 * and chose the ink itself would be drawing the second combination against the first's contract.
 */
void fb_draw_avatar(const struct mesh_ui_backend_fb_state *state, int x, int y, int size,
                    const char *label, enum mesh_ui_icon icon, struct mesh_ui_paint paint) {
    fb_fill_round_rect(state, x, y, size, size, size / 2, paint.fill);
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
                     paint.ink, paint.fill);
        return;
    }
    const int text_w = (int)cells * fb_char_adv(state, scale);
    fb_draw_text(state, x + (size - text_w) / 2, content_y, label, scale, paint.ink, paint.fill);
}

/* ---- the list window ------------------------------------------------------------------------ */

/* Everything but the window, which is the one thing the three entry points differ in. */
static struct fb_list fb_list_open(const struct fb_layout *layout, struct mesh_ui_list model) {
    struct fb_list list;
    memset(&list, 0, sizeof list);
    list.model = model;
    list.y = layout->body_y;
    list.line = layout->line;
    /* The window the rail measures, which is the body rather than the rows that happened to be
       filled: a list of three items in a body of fifteen has no rail at all, and a list of
       forty-two wants one the height of what a full window would have been. */
    list.track_y = layout->body_y;
    list.track_h = (int)layout->rows * layout->line;
    return list;
}

struct fb_list fb_list_begin_visible(const struct fb_layout *layout, uint32_t count,
                                     uint32_t cursor, uint32_t visible) {
    return fb_list_open(layout, mesh_ui_list_begin(count, cursor, visible));
}

struct fb_list fb_list_begin(const struct fb_layout *layout, uint32_t count, uint32_t cursor) {
    return fb_list_begin_visible(layout, count, cursor, layout->rows);
}

struct fb_list fb_list_begin_rows(const struct fb_layout *layout, uint32_t count, uint32_t cursor,
                                  uint32_t per_item) {
    /*
     * The division is the model's now rather than this line's, and that is not tidying: a body
     * of fifteen rows holding two-row items used to arrive here as a window of seven, and the
     * fifteenth row was rounded away before anything could know it had been there. The model is
     * told fifteen and two, so the slack stays a fact about the window - which is what lets a
     * list of mixed heights spend it on a one-row item.
     */
    const uint32_t step = per_item > 0U && per_item < 0xFFU ? per_item : 1U;
    return fb_list_open(layout,
                        mesh_ui_list_begin_step(count, cursor, layout->rows, (uint8_t)step));
}

struct fb_list fb_list_begin_heights(const struct fb_layout *layout, uint32_t count,
                                     uint32_t cursor, const uint8_t *heights) {
    return fb_list_open(layout, mesh_ui_list_begin_heights(count, cursor, layout->rows, heights));
}

struct fb_list fb_list_begin_cards(const struct fb_layout *layout, uint32_t count, uint32_t cursor,
                                   const uint8_t *heights, const uint8_t *cards) {
    struct fb_list list = heights != NULL ? fb_list_begin_heights(layout, count, cursor, heights)
                                          : fb_list_begin(layout, count, cursor);
    list.cards = cards;
    return list;
}

struct fb_list fb_list_begin_focus(const struct fb_layout *layout, uint32_t count, uint32_t cursor,
                                   const uint8_t *heights, const uint8_t *cards, uint32_t first,
                                   uint32_t last, bool card) {
    struct fb_list list = fb_list_open(
        layout, mesh_ui_list_begin_span(count, cursor, first, last, layout->rows, heights));
    list.cards = cards;
    list.focus_card = card && cards != NULL && count > 0U;
    return list;
}

/* Whether item `index` draws the cursor's highlight. Never on a list whose card is focused
   instead - see fb_list_begin_focus(). */
bool fb_list_is_cursor(const struct fb_list *list, uint32_t index) {
    return !list->focus_card && mesh_ui_list_is_cursor(&list->model, index);
}

/* Which card item `index` is on, or FB_LIST_NO_CARD. Past the end counts as no card, which is
   what lets the run walk below terminate without knowing the list's length. */
static uint8_t fb_list_card_of(const struct fb_list *list, uint32_t index) {
    if (list == NULL || list->cards == NULL || index >= list->model.count) {
        return FB_LIST_NO_CARD;
    }
    return list->cards[index];
}

/* Whether item `index` stands on a card at all - the ground a row is drawn against, where the
   run walk breaks, and whether the leading slot gives the cards beside it their hairline back. */
bool fb_list_on_card(const struct fb_list *list, uint32_t index) {
    return fb_list_card_of(list, index) != FB_LIST_NO_CARD;
}

/* Whether this list draws its groups as cards at all, which is a question about the list and
   not about one row of it - see fb_list_subheader_icon(), where a heading stands *between* two
   cards and so has a card list's ground under it either way. */
bool fb_list_has_cards(const struct fb_list *list) { return list != NULL && list->cards != NULL; }

/*
 * A card's vertical inset, and the gap it leaves between one card and the next.
 *
 * The same number fb_draw_card() insets by, so a card in a list and a card on the Status tab are
 * padded alike. Asked in two places - the surface takes it below its last row, and a group's
 * heading is centred in what it leaves - which is why it is a function rather than a local.
 *
 * Never more than the heading's own step can spare, and that bound is not a belt-and-braces
 * clamp: at MESH_UI_SCALE_MIN the type scale clamps the label *onto* the body - a label cannot
 * be rasterised below the smallest size the registry has - so a heading's cell is exactly as
 * tall as a row's and the only air in the step is one line gap. An inset taken out of that
 * leaves the cell longer than the gap it is centred in, the halved remainder truncates to zero,
 * and the last row of the cell lands on the next card's top edge: every heading with a
 * descender in it paints through the hairline, and only at the one scale nothing is rendered at.
 * Where there is no air the cards give up their inset rather than the heading its room.
 */
static int fb_list_card_pad(const struct mesh_ui_backend_fb_state *state) {
    const int edge = fb_edge(state);
    int pad = ((int)fb_metrics(state)->card_pad * state->scale) / 2;
    if (pad < edge) {
        pad = edge;
    }
    const int label = mesh_ui_theme_type_scale(state->theme, MESH_UI_TYPE_LABEL, state->scale);
    const int spare = fb_line_adv(state, state->scale) - (int)fb_font(state)->height * label - edge;
    if (pad > spare) {
        pad = spare;
    }
    return pad > 0 ? pad : 0;
}

enum mesh_ui_color fb_list_ground(const struct fb_list *list, uint32_t index) {
    return fb_list_on_card(list, index) ? MESH_UI_COLOR_SURFACE : MESH_UI_COLOR_BG;
}

uint32_t fb_list_row_height(const struct fb_list *list, uint32_t index) {
    return mesh_ui_list_item_height(&list->model, index);
}

/*
 * The card surfaces, for a list whose groups are drawn as cards.
 *
 * One rounded panel per *contiguous run* of items sharing a card ordinal, painted before any row
 * puts ink down - a fill has to go under text rather than over it, and there is no alpha on this
 * panel to recover from getting that the wrong way round.
 *
 * The run walk is over the window rather than over the list, which is what makes this cost the
 * same on a node with six rows and a node with a hundred and twenty. Heights come from the model
 * for the same reason every other measurement here does: the list is the authority once it has
 * been told, so a card can never be a row out from the rows standing on it.
 */
static void fb_list_cards(const struct mesh_ui_backend_fb_state *state, struct fb_list *list) {
    if (list == NULL || list->cards == NULL || list->model.visible == 0U) {
        return;
    }

    /*
     * The row fill's own rectangle with the hairline outside it.
     *
     * A card has to be at least as wide as the widest thing standing in it, and the widest thing
     * in a list is the cursor's highlight: drawn to the same rectangle, the highlight lands on
     * the hairline and paints it out for the length of one row, so the card appears to lose its
     * sides wherever the cursor is - and only there, which is the row the reader is looking at.
     * So the edge is spent outward, into the gutter fb_rail_gutter() keeps clear beside the
     * box. The highlight then fills the card's interior exactly, which is where Material puts a
     * state layer inside a container - and the card, being the box plus its hairline, is the
     * widest thing on the list and so the thing the rail measures its clearance from.
     */
    const int edge = fb_edge(state);
    const struct fb_row_box box = fb_row_box(state);
    const int x = box.x - edge;
    const int width = box.w + 2 * edge;
    /*
     * The row highlight's shape, not fb_draw_card()'s - and that is the same rule as the width,
     * one axis over. A card's fill and a row's highlight are the same rectangle here (the fill
     * is the card inset by its hairline, which is exactly the row gutter), so wherever the two
     * disagree about a corner the highlight wins: it reaches its full width while the card is
     * still curving, and the cursor's ends stand outside the card on the first and last rows of
     * every group. Drawn to one shape they nest exactly, and the hairline stays outside the
     * highlight all the way round.
     */
    const int radius = fb_radius(state, MESH_UI_SHAPE_SM);

    /*
     * The card's vertical inset, and it is spent at the bottom only.
     *
     * The same number fb_draw_card() insets by, so a card here and a card on the Status tab are
     * padded alike - but a list row is not a card row, and where that inset is *needed* differs.
     * A row's box is a line advance tall and a glyph's ink sits high in its cell, so the top of
     * the first row already carries most of a line's leading as air while the bottom of the last
     * carries none: its descenders run to the box's edge. Padding both ends equally would leave
     * the card top-heavy by exactly that leading. So the top of the box is the first row's own,
     * which is also what keeps the cursor's highlight inside the card on the row that opens it,
     * and the whole of the inset goes under the last row.
     */
    const int pad = fb_list_card_pad(state);

    const uint32_t first = list->model.first;
    const uint32_t last = first + list->model.visible; /* one past */
    /* Where the first visible row's fill starts, which is a glyph scale above its baseline -
       fb_draw_row_fill()'s own top edge. A card measured from the baseline would sit a few
       pixels low and clip the ascenders of the row it opens with. */
    int top = list->track_y - state->scale;
    uint32_t i = first;
    while (i < last && i < list->model.count) {
        const uint8_t card = fb_list_card_of(list, i);
        int height = (int)mesh_ui_list_item_height(&list->model, i) * list->line;
        uint32_t run = i + 1U;
        while (run < last && run < list->model.count && fb_list_card_of(list, run) == card) {
            height += (int)mesh_ui_list_item_height(&list->model, run) * list->line;
            run++;
        }
        if (!fb_list_on_card(list, i)) {
            top += height;
            i = run;
            continue;
        }

        /*
         * Whether this run is the whole card or a slice of one the window cut, asked of the
         * items on either side of it rather than of the window - which is the same question and
         * the one that stays right when a card happens to end exactly where the window does.
         *
         * A cut end keeps square corners and takes no padding, so the card runs to the edge of
         * the body and reads as continuing past it. Everything drawn here is inside the window
         * by construction: the first run starts at the body's own top edge and the last ends
         * where the visible steps do, so there is nothing to clip.
         */
        const bool cut_top = i > 0U && fb_list_card_of(list, i - 1U) == card;
        const bool cut_bottom = run < list->model.count && fb_list_card_of(list, run) == card;
        /*
         * The hairline is spent outward at the top, exactly as it is at the sides and for the
         * same reason: the first row of a card is a row the cursor can stand on, and a fill
         * drawn to the card's own rectangle lands on the edge and paints it out - so the card
         * reads as open at the top on precisely the row being pointed at. It never climbs past
         * the body, where the rows themselves start.
         */
        int box_top = top;
        if (!cut_top) {
            box_top -= edge;
            const int ceiling = list->track_y - state->scale;
            if (box_top < ceiling) {
                box_top = ceiling;
            }
        }
        int box_bottom = top + height;
        if (!cut_bottom) {
            /*
             * Into the step the group's next heading stands in, which is where the break between
             * two cards comes from - never past the body, or the bottom card of a list that
             * filled its window would put its edge through the action bar.
             *
             * A heading is the only step a card is ever padded into, and that is what makes the
             * inset safe to spend whole: a heading is drawn small and centres itself in whatever
             * room is left, where a full row is a line advance with a glyph cell in it and a
             * leading disc nearly as tall as the step, so a card padding into one would land its
             * edge on the disc's crown. Nothing hands this function a card followed straight by a
             * panel row any more - a settings group is one card whatever is in it, verbs
             * included - so the step below a card is a heading or it is the end of the list.
             */
            box_bottom += pad;
            const int floor_y = list->track_y + list->track_h;
            if (box_bottom > floor_y) {
                box_bottom = floor_y;
            }
        }
        const int box_h = box_bottom - box_top;

        /*
         * The edge first and the fill inside it, which is fb_draw_card()'s shape and for its
         * reason: fb_fill_round_rect() fills rather than strokes, so an outline is the larger
         * shape with the smaller one laid over it. The hairline is not decoration - on every
         * theme that ships the surface is one step off the ground, and the edge is most of what
         * says a card is there. It is OUTLINE rather than RULE for fb_draw_card()'s reason too:
         * a separator may fade politely into what it divides and an edge may not.
         *
         * A cut end loses its inset along with its corners. Insetting there would draw the
         * hairline *across* the cut, which is the card claiming to end again - in a straight
         * line this time.
         */
        /*
         * The card the cursor stands on, when it stands on a card: fb_draw_card()'s focus ring,
         * in the accent and twice the hairline, grown inward so nothing else on the list moves.
         * The rows of a focused card draw no highlight, so there is nothing for it to paint over.
         */
        const bool focused =
            list->focus_card && list->model.cursor >= i && list->model.cursor < run;
        const int ring = focused ? 2 * edge : edge;
        fb_fill_round_rect_ends(state, x, box_top, width, box_h, radius + edge,
                                focused ? fb_tone_color(state, MESH_UI_TONE_PRIMARY)
                                        : fb_color(state, MESH_UI_COLOR_OUTLINE),
                                !cut_top, !cut_bottom);
        const int inner_top = cut_top ? box_top : box_top + ring;
        const int inner_bottom = cut_bottom ? box_top + box_h : box_top + box_h - ring;
        const int inner_radius = radius + edge - ring > 0 ? radius + edge - ring : 0;
        fb_fill_round_rect_ends(state, x + ring, inner_top, width - 2 * ring,
                                inner_bottom - inner_top, inner_radius,
                                fb_color(state, MESH_UI_COLOR_SURFACE), !cut_top, !cut_bottom);

        top += height;
        i = run;
    }
}

/*
 * The scroll rail. Drawn once per list, by the first row that draws - see fb_widgets.h.
 *
 * It stands in fb_rail_gutter()'s strip, which every list has already been measured to leave
 * clear - so it is beside the content rather than over it, on a flat list and on a column of
 * cards alike. Where that strip *is* is asked of fb_row_box() rather than worked out from the
 * margin: the card spends its hairline outward from the box, so the free space starts one
 * hairline past the box's own edge and a rail measured from the margin lands on the card.
 */
static void fb_list_rail(const struct mesh_ui_backend_fb_state *state, struct fb_list *list) {
    if (list->track_h <= 0) {
        return;
    }

    /*
     * Sized from the gutter it lives in rather than from the glyph scale, unlike every other
     * control here. The gutter is half a margin wide and does not grow when a theme asks for
     * bigger text, so a rail measured in glyph steps ran out of clearance and ended up flush
     * against the panel edge at the larger scales. A quarter-margin leaves the same gap either
     * side at every scale, which is what makes it read as inset rather than as a screen edge.
     */
    int width = fb_gutter(state) / 2;
    if (width < 2) {
        width = 2;
    }

    /*
     * The free strip: from the outer edge of the widest thing the list draws - the box plus the
     * hairline a card spends outward - to the panel edge. The rail is centred in it, so the gap
     * to the content and the gap to the screen edge are the same number and neither is a
     * constant anybody has to keep in step with the card.
     *
     * Narrowed rather than moved if the strip cannot hold it with clearance either side: a rail
     * touching the card is what this exists to prevent, and a thinner one still reports the
     * scroll.
     */
    const struct fb_row_box box = fb_row_box(state);
    const int strip_x = box.x + box.w + fb_edge(state);
    const int strip_w = (int)state->var.xres - strip_x;
    if (strip_w < 3) {
        return;
    }
    if (width > strip_w - 2) {
        width = strip_w - 2;
    }

    /* The proportion is mesh_ui_list_scroll()'s - no pixels in it, and unit tested there. A
       length of 0 is a list that fits, which draws nothing at all rather than a full track. */
    const struct mesh_ui_scroll scroll =
        mesh_ui_list_scroll(&list->model, list->track_h, 4 * width);
    if (scroll.length <= 0) {
        return;
    }

    const int x = strip_x + (strip_w - width) / 2;
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

/*
 * Everything the list draws that is not a row: the card surfaces its groups stand on, then the
 * scroll rail.
 *
 * **No screen asks for it.** It is drawn by the first row that draws, because both halves are
 * derived entirely from the model - `count`, `first`, `visible`, the heights and the grouping -
 * so a screen has nothing to say about either and a screen that had to remember the call is a
 * screen that would forget on one list out of nine. Same reasoning the list item's clipping
 * follows: geometry belongs down here and the screen describes content.
 *
 * The order is the only thing this function decides, and it decides it once: a surface goes
 * under the ink standing on it, and the rail is outside both.
 */
void fb_list_chrome(const struct mesh_ui_backend_fb_state *state, struct fb_list *list) {
    if (list == NULL || list->chrome_drawn) {
        return;
    }
    list->chrome_drawn = true;
    fb_list_cards(state, list);
    fb_list_rail(state, list);
}

bool fb_list_next(struct fb_list *list, uint32_t *index) {
    return mesh_ui_list_next(&list->model, index);
}

void fb_list_row(const struct mesh_ui_backend_fb_state *state, struct fb_list *list, uint32_t index,
                 const char *text, enum mesh_ui_tone tone) {
    fb_list_chrome(state, list);
    /* Drawn out rather than through fb_draw_row(), which lays its fill on the panel's own
       ground: a row in a list may be standing on a card, and the ink its glyph edges blend into
       has to be the colour actually under it. */
    const bool selected = fb_list_is_cursor(list, index);
    const struct mesh_ui_rgb ground = fb_draw_row_fill_on(
        state, list->y, fb_list_row_height(list, index), selected, fb_list_ground(list, index));
    fb_draw_text(state, fb_row_box(state).text_x, list->y, text, state->scale,
                 selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL) : fb_tone_color(state, tone),
                 ground);
    /* By what the model says this row is, not by one row: a plain row in a list of mixed
       heights is still whatever height that list gave it, and advancing by a row would put
       every row under it in the wrong place. */
    list->y += (int)fb_list_row_height(list, index) * list->line;
}

/*
 * One tier of a row's text: the tone on the ground, and the cursor's own pair over the fill.
 *
 * A row under the cursor is drawn against a different fill rather than in a dimmer version of
 * the same colour, so "which ink" is two questions and not one - and a tier that is quiet on
 * the ground has to stay quiet on the fill or a label column flashes to full strength on
 * precisely the row being read. MESH_UI_TONE_DIM is what says a tier is the quiet one, which
 * is the same thing it says everywhere else the theme answers for ink.
 *
 * Shared by the headline, its label column and the supporting line, because the three were
 * three copies of this conditional and the supporting one had already grown a flag of its own.
 */
struct mesh_ui_rgb fb_item_ink(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_tone tone,
                               bool selected, bool quiet) {
    if (!selected) {
        return fb_tone_color(state, tone);
    }
    return fb_color(state, quiet ? MESH_UI_COLOR_TEXT_ON_SEL_DIM : MESH_UI_COLOR_TEXT_ON_SEL);
}

void fb_list_subheader(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                       uint32_t index, const char *text) {
    fb_list_subheader_icon(state, list, index, text, (struct fb_leading){.kind = FB_LEADING_NONE});
}

void fb_list_subheader_icon(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                            uint32_t index, const char *text, struct fb_leading leading) {
    fb_list_chrome(state, list);
    const int scale = mesh_ui_theme_type_scale(state->theme, MESH_UI_TYPE_LABEL, state->scale);
    const uint32_t rows = fb_list_row_height(list, index);
    const bool selected = fb_list_is_cursor(list, index);

    /* The fill is the whole step whatever size the words are, and it is the same rectangle a
       plain row lays down - a highlight that shrank to the label would be a cursor that changes
       shape as it walks down a list. */
    const struct mesh_ui_rgb ground =
        fb_draw_row_fill_on(state, list->y, rows, selected, fb_list_ground(list, index));

    /*
     * Sat on the bottom of the step, so the space the smaller glyphs free is air above the
     * heading rather than under it. That is the whole of what makes it read as a section break:
     * the gap belongs to the group beginning, not to the row that ended.
     *
     * On a column of cards it is centred instead, because there the heading is not a break
     * between two runs of rows - it is the label of the card under it, standing in the gap
     * between that card and the one that ended. The gap is the whole of this step bar the inset
     * the card above took out of its top (fb_list_cards()), and the *cell* is what is centred
     * in it rather than the line advance: a line carries its leading at the top, so centring
     * the advance would seat the words low and leave the heading hanging off the card above.
     */
    const int step_top = list->y - state->scale;
    const int step_h = (int)rows * list->line;
    int baseline = list->y + fb_line_adv(state, state->scale) - fb_line_adv(state, scale);
    if (fb_list_has_cards(list)) {
        const int gap_top = step_top + fb_list_card_pad(state);
        const int gap_bottom = step_top + step_h - fb_edge(state);
        const int cell = (int)fb_font(state)->height * scale;
        baseline = gap_top + (gap_bottom - gap_top - cell) / 2;
    }
    /*
     * Indented to where its own rows start, when the list declares a leading slot.
     *
     * A heading that stayed at the margin over rows whose words begin an icon-box further in is
     * a heading naming a column nothing is in, which is the two-column problem the leading slot
     * already refuses one row at a time. The slot is measured at the *body* scale, not the
     * label scale this draws at, because it is the rows' gutter being matched rather than one
     * of this row's own.
     *
     * Whether anything is *drawn* in it is the card distinction, and it is asked of the *list*
     * rather than of this row's ground - a heading on a column of cards stands between two of
     * them, so the ground under it is the panel's either way. On a flat list the slot stays
     * empty: a heading there is a break between runs of rows, and a symbol on it would be a
     * second thing saying what the words underneath say - the icons on such a list are what each
     * row is about, and a group has no single answer to that. On a column of cards the heading
     * names the card below it, and there the symbol is the cell the eye finds when it is looking
     * for Signal rather than Identity, which is exactly what struct fb_card's icon is for. The
     * list is asked which it is drawing, so a caller passes the icon either way and nothing
     * decides twice - and the indent is the same whether or not it was drawn.
     */
    /*
     * And the ink, from the same question the icon is: what this heading *is*.
     *
     * On a flat list it is a break between two runs of rows, so it stays quiet on the ground and
     * quiet on the fill alike - it is not one of the rows the cursor came here to read, and a
     * loud break would be the screen shouting its own furniture.
     *
     * On a column of cards it is the card's label, and there quiet is wrong twice over. It is
     * the cell the eye lands on when it is looking for Signal rather than Identity on a screen a
     * hundred and twenty rows long, which is the argument its icon is already drawn for - and
     * fb_draw_card() has been inking the heading beside *its* icon in the card's own tone since
     * the Status tab got cards, so a heading dimmed here was the two card kinds holding two
     * opinions about the same line. The primary is what a card with nothing wrong with it takes
     * there, and it is what a group of a node's facts is: the brand colour marking where the
     * reader is meant to look, which is the whole of what this palette keeps it for.
     *
     * Asked of the list rather than declared by the caller, exactly as the icon's own drawn/not
     * drawn is: the list knows which of the two it is drawing, so a screen passes a heading and
     * nothing decides twice.
     */
    const enum mesh_ui_tone tone =
        fb_list_has_cards(list) ? MESH_UI_TONE_PRIMARY : MESH_UI_TONE_DIM;
    const struct mesh_ui_rgb ink = fb_item_ink(state, tone, selected, tone == MESH_UI_TONE_DIM);

    int x = fb_row_box(state).text_x;
    if (leading.kind == FB_LEADING_TONAL || leading.kind == FB_LEADING_TONAL_SLOT) {
        /*
         * The card's symbol in a disc, which is what a heading over rows that carry discs has to
         * be: the slot is the rows' gutter being matched, so a heading that drew a bare icon
         * there would name a column a disc narrower than the one its rows start in.
         *
         * It is also the header every phone app gives a card - a circled mark beside the card's
         * name - and the two readings are the same one, which is why this takes the kind rather
         * than a flag. The disc wears the heading's own tone, exactly as a row's does.
         */
        const int gutter = list->line - state->scale;
        if (fb_list_has_cards(list)) {
            /*
             * Sized to the heading's own words, not to the break it stands in, and centred in
             * that break both ways.
             *
             * The gap is what a disc has to *fit inside*, and it is the wrong thing to measure
             * one against: the break between two cards is a body row less the inset the card
             * above took out of its top, so a disc grown to fill it comes out exactly as tall as
             * the gap and lands with its crown on the hairline above and its foot on the
             * hairline below. Nothing overlaps, so no edge check catches it - it just reads as a
             * bead jammed between two panels.
             *
             * Half again the cell the words are drawn in is the size a mark beside a heading
             * wants, and it scales with the type rather than with the furniture: a theme asking
             * for bigger text gets a bigger disc, and one asking for roomier cards gets more air
             * around the same one. The break is then only a clamp, and it keeps a clearance
             * step at each end - MESH_UI_SPACE_SM, which is what that step is named for - so the
             * disc is seen to be *in* the gap rather than wedged between the two cards the gap
             * separates. Half a step is not enough to read as clearance at any scale this
             * ships: it leaves two pixels, and two pixels of ground between a disc and a
             * hairline looks exactly like the disc touching it.
             */
            const int inset = fb_space(state, MESH_UI_SPACE_SM);
            const int gap_top = step_top + fb_list_card_pad(state);
            const int gap_h = step_top + step_h - fb_edge(state) - gap_top;
            const int cell = (int)fb_font(state)->height * scale;
            const int room = gap_h - 2 * inset;
            int size = cell + cell / 2;
            if (size > room) {
                size = room;
            }
            if (size > gutter) {
                size = gutter;
            }
            const enum mesh_ui_family family = mesh_ui_tone_family(tone);
            const struct mesh_ui_paint disc =
                fb_paint(state, family != MESH_UI_FAMILY_COUNT ? family : MESH_UI_FAMILY_PRIMARY,
                         MESH_UI_SLOT_CONTAINER, MESH_UI_STATE_REST);
            /* The empty slot takes the gutter and draws nothing in it, which is the whole of
               what it is for here: a group whose subject this client has no rune for - "Sent
               with a position" is a sentence about ten bits - still has to begin where its rows
               begin. Without it the heading stood at the panel's margin over rows indented past
               a disc, which is the heading "naming a column nothing is in" that the note above
               is about, seen from the one side that had no answer. */
            if (size > 0 && leading.kind == FB_LEADING_TONAL) {
                fb_draw_avatar(state, x + (gutter - size) / 2, gap_top + (gap_h - size) / 2, size,
                               NULL, leading.icon, disc);
            }
        }
        x += gutter + fb_char_adv(state, state->scale) / 2;
    } else if (leading.kind != FB_LEADING_NONE) {
        if (mesh_ui_icon_is_valid(leading.icon) && fb_list_has_cards(list)) {
            /* On the heading's own baseline rather than the body's: this row draws at the label
               scale, and a symbol standing where a body row's would floats a third of a row
               clear of the word it belongs to. */
            fb_draw_icon(state, x, baseline, leading.icon, scale, ink, ground);
        }
        x += fb_icon_box(state, state->scale) + fb_char_adv(state, state->scale) / 2;
    }
    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", text != NULL ? text : "");
    mesh_ui_line_fit(&line, fb_row_cols(state, scale));
    fb_draw_text(state, x, baseline, mesh_ui_line_text(&line), scale, ink, ground);
    list->y += (int)rows * list->line;
}

/* ---- the note row -------------------------------------------------------------------------- */

/* The width a note's body wraps to. The list's own columns: a paragraph indented past the rows
   around it would be a second left margin on a panel that has room for one. */
static size_t fb_note_cols(const struct mesh_ui_backend_fb_state *state) {
    return fb_row_cols(state, state->scale);
}

uint32_t fb_list_note_steps(const struct mesh_ui_backend_fb_state *state, const char *heading,
                            const char *body) {
    if (state == NULL) {
        return 1U;
    }
    /* The heading costs a step even though it is drawn small, because a step is a body row and
       the list model counts in those. The air the smaller glyphs free goes above it, exactly as
       it does on a subheader. */
    uint32_t steps = (heading != NULL && heading[0] != '\0') ? 1U : 0U;
    steps += mesh_ui_wrap_lines(body != NULL ? body : "", fb_note_cols(state));
    /* A note with nothing in it is still a row: a zero-height row would put every row under it
       at the wrong offset, which is the failure the whole heights mechanism exists to prevent. */
    return steps > 0U ? steps : 1U;
}

void fb_list_note(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                  uint32_t index, const char *heading, const char *body) {
    fb_list_chrome(state, list);
    const uint32_t rows = fb_list_row_height(list, index);
    const bool selected = fb_list_is_cursor(list, index);
    /* One fill for the whole note, the height the *model* gave it - not the height its words
       want. The two agree when the screen measured with fb_list_note_steps(), and when they do
       not it is the model that is right, because it is what every row below was placed against. */
    const struct mesh_ui_rgb ground =
        fb_draw_row_fill_on(state, list->y, rows, selected, fb_list_ground(list, index));

    const int margin = fb_row_box(state).text_x;
    const int body_line = fb_line_adv(state, state->scale);
    const bool titled = heading != NULL && heading[0] != '\0';
    int y = list->y;

    if (titled) {
        const int scale = mesh_ui_theme_type_scale(state->theme, MESH_UI_TYPE_LABEL, state->scale);
        /* Sat on the bottom of its step for the reason a subheader is: the gap the small glyphs
           leave belongs above the heading, separating it from the paragraph that ended. */
        struct mesh_ui_line line;
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%s", heading);
        mesh_ui_line_fit(&line, fb_row_cols(state, scale));
        /*
         * The ink is chosen with the fill rather than beside it. A heading painted in the
         * primary whether or not the row was selected is a pair no theme was measured against -
         * the accent over the selection fill is the one combination the contrast contract does
         * not cover, because on a light palette they are two shades of the same hue.
         */
        fb_draw_text(state, margin, y + body_line - fb_line_adv(state, scale),
                     mesh_ui_line_text(&line), scale,
                     selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)
                              : fb_tone_color(state, MESH_UI_TONE_PRIMARY),
                     ground);
        y += body_line;
    }

    /*
     * The paragraph, one wrapped line per step.
     *
     * Clipped to the row's own height rather than to the panel: a note the model was told is
     * three steps tall draws three lines and stops, so a measure that disagreed with the words
     * loses the tail of a sentence instead of painting it over the next note. Losing text is
     * visible; overlapping it is not, which is the trade this file makes everywhere.
     */
    uint32_t drawn = titled ? 1U : 0U;
    struct mesh_ui_wrap wrap;
    mesh_ui_wrap_begin(&wrap, body != NULL ? body : "", fb_note_cols(state));
    while (drawn < rows && mesh_ui_wrap_next(&wrap)) {
        fb_draw_text(state, margin, y, wrap.line, state->scale,
                     selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)
                              : fb_tone_color(state, MESH_UI_TONE_NORMAL),
                     ground);
        y += body_line;
        drawn++;
    }

    list->y += (int)rows * list->line;
}

void fb_list_row_line(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                      uint32_t index, struct mesh_ui_line *line, enum mesh_ui_tone tone) {
    /* The row's columns, not the panel's: fb_rail_gutter() is kept clear of the box every row
       is drawn in, so a line fitted to fb_cols() is a line fitted to a width no row has. */
    mesh_ui_line_fit(line, fb_row_cols(state, state->scale));
    fb_list_row(state, list, index, mesh_ui_line_text(line), tone);
}
