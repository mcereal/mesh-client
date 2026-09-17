#define _POSIX_C_SOURCE 200809L

/*
 * The slotted list row: where each slot lands, what ink it takes, and what gives way first when
 * the row runs out of width.
 */

#include "fb_widgets_item.h"
#include "fb_widgets_button.h"
#include "fb_widgets_chrome.h"
#include "fb_widgets_list_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/emoji.h"

#include <string.h>

/* ---- the conversation cell ----------------------------------------------------------------- */

/* ---- the list item ------------------------------------------------------------------------ */

/*
 * An item's geometry, all of it derived once so the fill, the text and the slots cannot
 * disagree about where the row is. Every screen used to re-derive some part of this, and the
 * parts that drifted were exactly the ones nothing else could see: how far a trailing control
 * ate into the text, and which box a control centred itself on.
 */
struct fb_item_geom {
    uint32_t rows;
    int fill_x, fill_w;   /* the row's own rectangle, fb_row_box()'s and nobody else's */
    int fill_top, fill_h; /* the box the cursor fill paints, and a control centres on */
    int head_y;           /* headline baseline */
    int supp_y;           /* supporting baseline; only meaningful on a two-row item */
    int slot_h;           /* height of a box-shaped trailing - a badge */
    int head_slot_top, supp_slot_top;
    int text_x, text_right;
    int content_x;    /* where the row's content starts, before any leading slot is reserved */
    int lead_size;    /* the leading slot's side: one width per list, never per row */
    int lead_disc;    /* what may actually be drawn in it, once the cards either side have theirs */
    int lead_y;       /* and where: its own answer, because a shrunken disc is centred not seated */
    int marker_x;     /* a plain row's marker cell; only meaningful when the row reserved one */
    size_t cols;      /* text columns between the leading slot and the trailing edge */
    int bar_y, bar_h; /* a stacked meter's track; bar_h of 0 is a row that has none */
};

static struct fb_item_geom fb_item_measure(const struct mesh_ui_backend_fb_state *state,
                                           const struct fb_list *list,
                                           const struct fb_list_item *item, uint32_t index,
                                           uint32_t rows) {
    const int scale = state->scale;
    const int adv = fb_char_adv(state, scale);
    const struct fb_row_box box = fb_row_box(state);
    struct fb_item_geom g;
    memset(&g, 0, sizeof g);

    /*
     * The height is the *list's* answer, not this item's.
     *
     * It used to be read off the item - two rows if it had a supporting line, one otherwise -
     * which was a second opinion about something the window had already decided, and the two
     * were only ever equal because every list happened to be uniform. Once a list can mix
     * heights the model is the one that has to be right: it is what placed the window, what
     * the cursor's highlight is measured against and what the scroll thumb reports. So a row
     * that draws taller than the list was told simply cannot happen from here.
     */
    g.rows = rows > 0U ? rows : 1U;
    g.head_y = list->y;
    g.fill_x = box.x;
    g.fill_w = box.w;
    g.text_right = box.text_right;
    g.slot_h = list->line - scale;

    if (g.rows >= 2U) {
        /* Two rows set closer together than two items are: the supporting line sits a scale
           above where a second row would put it, and the space that frees becomes the gap to
           the next item. */
        g.supp_y = list->y + list->line - scale;
        g.fill_top = g.head_y - fb_space(state, MESH_UI_SPACE_XS);
        g.fill_h = (int)g.rows * list->line - fb_space(state, MESH_UI_SPACE_MD);
        g.head_slot_top = g.fill_top;
        g.supp_slot_top = g.supp_y - fb_space(state, MESH_UI_SPACE_XS);
    } else {
        g.fill_top = g.head_y - scale;
        g.fill_h = list->line;
        g.head_slot_top = g.fill_top;
        g.supp_slot_top = g.fill_top;
    }

    /*
     * The leading slot's side, and the two answers are the point rather than an oversight.
     *
     * A disc is a *picture* and is sized to the row it leads, so a conversation cell's avatar
     * fills the two steps its preview line gave it. The empty slot is not a picture: its whole
     * job is to put the words of a row that carries nothing in the same column as the words of
     * the rows that do - and in every list that mixes the two, the row carrying a disc is one
     * step tall. A settings verb never takes a second step (a slider and a meter are settings,
     * and neither is a verb), and neither does a node detail's.
     *
     * So the slot is measured off `list->line`, the one step every row of the list is built
     * out of, where `g.fill_h` is how many of them this row happened to take. It used to be
     * `g.fill_h - scale` for all three, which is the same number on a one-step row and a
     * larger one on every row that took two - so a settings section that mixed a slider in
     * with its neighbours drew that row's label, its value and its track a cell to the right
     * of the rows above and below it, and the gutter promised to a row with nothing in it was
     * not the gutter the discs beside it actually stood in. Position and LoRa are where it
     * showed; the rule it breaks is the one stated on FB_LEADING_ICON directly below, and
     * `ui_capture_a_section_starts_every_row_in_one_column` is what holds it now.
     */
    g.lead_size =
        item->leading.kind == FB_LEADING_TONAL_SLOT ? list->line - scale : g.fill_h - scale;
    /*
     * A row on the panel gives back the hairline each card beside it spends into its step, and
     * what may actually be *drawn* in its leading slot follows from what is left.
     *
     * A card's edge is drawn outside its own rows' boxes so that no row's highlight can paint it
     * out (fb_list_cards()), and where the card's neighbour is a full row rather than a heading,
     * that "outside" is inside *this* row's box. Left there it is the same bug from either side
     * of one hairline: selecting this row paints over the card's edge and its corners, and
     * selecting the card's last row paints over the other one. So the box stops short of both,
     * which costs the row two pixels of fill it was not using and nothing else - the baseline
     * does not move and a glyph's cell sits inside what is left.
     *
     * The slot is the gutter and the gutter may not move: it is what puts every row's words in
     * one column, which is the whole of what the slot is for. `lead_size` is therefore measured
     * before any of this, and only the disc drawn in it gives way - centred in the slot, so the
     * room a card took comes off the disc and never off the column.
     *
     * A pixel of clearance at each end where a card is adjacent, because a disc laid against a
     * card's hairline reads as attached to that card rather than standing between two, which is
     * what the row is. It costs the disc two pixels at the Brick's own glyph scale, and those
     * two are the step at which fb_draw_avatar() drops its symbol a scale - which is a real cost
     * and the right way round: a smaller mark on a row that is a button under a form, against a
     * card that looks broken.
     */
    if (fb_list_has_cards(list) && !fb_list_on_card(list, index)) {
        const int edge = fb_edge(state);
        const bool card_above = index > 0U && fb_list_on_card(list, index - 1U);
        const bool card_below = fb_list_on_card(list, index + 1U);
        if (card_above) {
            g.fill_top += edge;
            g.fill_h -= edge;
        }
        if (card_below) {
            g.fill_h -= edge;
        }
        const int band_top = g.fill_top + (card_above ? 1 : 0);
        const int band = g.fill_top + g.fill_h - (card_below ? 1 : 0) - band_top;
        g.lead_disc = g.lead_size > band ? (band > 0 ? band : 0) : g.lead_size;
        /* Centred in what is left, so the room the cards took is ground at both ends of the disc
           rather than all of it at one. */
        g.lead_y = band_top + (band - g.lead_disc) / 2;
    } else {
        g.lead_disc = g.lead_size;
        /* A hairline inside the top of the row's box, which is where a glyph's own ink sits and
           so what keeps a disc reading as part of the line beside it. */
        g.lead_y = g.fill_top + fb_space(state, MESH_UI_SPACE_XS);
    }
    g.content_x = box.text_x;
    g.text_x = g.content_x;
    if (item->leading.kind == FB_LEADING_AVATAR || item->leading.kind == FB_LEADING_TONAL ||
        item->leading.kind == FB_LEADING_TONAL_SLOT) {
        /* One measurement for all three. A tonal container is an avatar that happens to be
           filled from a family rather than from a hash, and a gutter that differed between them
           would be a list unable to mix the two - which the node detail does, one card of verbs
           at a time. The empty slot measures with them for the same reason it exists: it is this
           gutter, promised to a row that has nothing to put in it. */
        g.text_x = g.content_x + g.lead_size + adv / 2;
    } else if (item->leading.kind == FB_LEADING_ICON) {
        /* Reserved whether or not this row filled it, so every row's words start in the same
           column - a list that indents only the rows with something to say is a list the eye
           cannot run down. */
        g.text_x = g.content_x + fb_icon_box(state, scale) + adv / 2;
    }
    /* The plain row's marker gutter, between whatever the leading slot put down and the words.
       Reserved for the whole list rather than for the rows that filled it, on the same terms as
       the leading slot above - and only where there is no label column, which measures its own. */
    g.marker_x = g.text_x;
    if (item->label_cols == 0U && item->marker_slot) {
        g.text_x += fb_icon_box(state, scale) + adv / 2;
    }
    g.cols = g.text_right > g.text_x ? (size_t)((g.text_right - g.text_x) / adv) : 1U;

    /*
     * A stacked meter's track, and the one thing on a two-step item that the fill has to be
     * grown for.
     *
     * The supporting line's box overhangs the fill by design - a glyph's ink sits high in its
     * cell, so text stays comfortably inside a fill that stops short of the box, and the space
     * that leaves is the gap between one item and the next. A bar has no such slack: its ink is
     * the whole of its box, so a fill measured for text left the cursor's highlight ending a few
     * pixels above the bar it was meant to be under. The fill therefore takes the bar in, plus
     * the same breathing room it has at the top.
     */
    if ((item->meter != NULL || item->slider != NULL) && g.rows >= 2U) {
        /* One answer for both of the things that can be in that step, because the step is one
           step: a screen that swapped a reading for a control on the same row must not find the
           row a few pixels shorter. The slider is the taller of the two - it reserves the room
           its handle stands up into under the cursor - so it is what the step is measured by
           wherever it is the one present. */
        g.bar_h = item->slider != NULL ? fb_slider_height(state, scale)
                                       : fb_meter_thickness(state, scale);
        /* On the supporting line's geometry: a second line set closer to its headline than two
           rows would be, which is what keeps the bar reading as part of the row above it rather
           than as something floating between two rows. */
        g.bar_y = g.supp_y + ((int)fb_font(state)->height * scale - g.bar_h) / 2;
        const int bottom = g.bar_y + g.bar_h + fb_space(state, MESH_UI_SPACE_XS);
        if (bottom - g.fill_top > g.fill_h) {
            g.fill_h = bottom - g.fill_top;
        }
    }
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
 *
 * `reserved` is what the row has already promised to something else - the label column and its
 * marker gutter, or a plain row's marker cell. A slot is fitted against what is *free*, not
 * against the whole line: the headline is clipped from its tail, so a slot measured against the
 * line ate the value first and then the label, and a label column is the one thing on a settings
 * row that may not move. The wide slots are why this arrived - a switch is four cells and never
 * reached it, a segmented button is most of a value column and reaches it at every scale.
 */
/*
 * What the segmented slot takes out of a line `cols` wide, and which of its two forms it takes.
 *
 * The one slot with a second form, so it is the one slot that needs a function of its own: every
 * other kind either fits or is dropped, and a set of choices always has the chosen one in words
 * to fall back on. `out_as_text` comes back true when the control could not have its room -
 * which is a value column too narrow for the segments, not a caller that forgot to fill one in.
 *
 * Measuring and drawing both ask *this*, for the reason they both ask fb_trailing_cols(): a slot
 * sized by one rule and painted by another is what turned a trailing age into a smear across an
 * avatar, and a control that decided its own form twice would do it again.
 */
/*
 * The glyph multiplier a row's segments are set at: the label role, not the row's own.
 *
 * Which is what Material asks for and, more to the point here, what makes the component reach
 * the settings it exists for. A segmented button spends its width `count` times over, so a
 * three-valued setting at body scale wants most of a panel - "Random PIN / Fixed PIN / No PIN"
 * does not fit a value column at any scale this ships with, and would have fallen back to the
 * word it was meant to replace on every theme. It is also the right answer on its own terms: a
 * control's label is chrome, which is the type role the action bar's verbs and the navigation
 * bar's tabs already take.
 */
static int fb_segmented_scale(const struct mesh_ui_backend_fb_state *state) {
    return fb_type_scale(state, MESH_UI_TYPE_LABEL);
}

/* The chosen word instead of the control, on the terms an ordinary trailing text takes. Both
   ways of ending up there - no room, and no drawable choice - go through this, so a slot that
   fell back for one reason is measured exactly like a slot that fell back for the other. */
static size_t fb_segmented_as_text(const struct mesh_ui_backend_fb_state *state, size_t cols,
                                   const struct fb_segmented *segmented, bool *out_as_text) {
    (void)state;
    if (out_as_text != NULL) {
        *out_as_text = true;
    }
    if (segmented == NULL) {
        return 0U;
    }
    const size_t cells = mesh_ui_text_cells(segmented->value);
    return (cells > 0U && cols > cells + 1U) ? cells + 1U : 0U;
}

static size_t fb_segmented_cols(const struct mesh_ui_backend_fb_state *state, size_t cols,
                                const struct fb_segmented *segmented, bool *out_as_text) {
    if (out_as_text != NULL) {
        *out_as_text = true;
    }
    if (segmented == NULL || segmented->count == 0U || segmented->count > FB_SEGMENTED_MAX ||
        segmented->active >= segmented->count) {
        /*
         * A choice outside the set is a choice this control cannot draw, and it is a state the
         * radio can genuinely be in: an enum value from a newer firmware, or a corrupt one. The
         * item still carries it and still formats it - "Unknown" - so the words are the honest
         * answer and they are already here. Highlighting the first segment instead would have
         * the panel state that pairing is set to Random PIN when nobody knows what it is set to,
         * which is the one thing a picture is not allowed to do quietly.
         *
         * Not "every segment unlit" either: a set with nothing chosen says *none of these*,
         * which is a different false claim.
         */
        return fb_segmented_as_text(state, cols, segmented, out_as_text);
    }
    const int adv = fb_char_adv(state, state->scale);
    const int width = fb_segmented_width(state, segmented, fb_segmented_scale(state));
    /*
     * Counted in the *row's* cells whatever the segments are set at, because what it is being
     * fitted into is a line of the row's own text.
     *
     * And with no cell of air added, unlike every other slot here. The others sit next to the
     * row's value and the extra cell is the gap to it; this one *is* the value - the words it
     * replaces are inside it - so the only thing left of the line is the label column, which the
     * caller has already reserved. The gap is then exactly the cell the strictly-greater test
     * below keeps back, and adding a second one cost the three-valued settings the control at
     * the shipping scale by a single column.
     */
    const size_t want = (size_t)((width + adv - 1) / adv);
    if (width > 0 && cols > want) {
        if (out_as_text != NULL) {
            *out_as_text = false;
        }
        return want;
    }
    return fb_segmented_as_text(state, cols, segmented, out_as_text);
}

static size_t fb_trailing_cols(const struct mesh_ui_backend_fb_state *state, size_t cols,
                               size_t reserved, const struct fb_trailing *trailing) {
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
    case FB_TRAILING_SIGNAL: {
        /* The staircase, its gap to whatever is left of it, and the figure it carries - which
           may be nothing, and then costs nothing. Stated the same way the meter's width is, and
           for the same reason: rungs have no natural width either. */
        const size_t cells = mesh_ui_text_cells(trailing->text);
        want = FB_SIGNAL_CELLS + 1U + (cells > 0U ? cells + 1U : 0U);
        break;
    }
    case FB_TRAILING_SPARK:
        /* The line and the cell of air between it and the words, stated the way the two above
           are. A trend with nothing in it costs nothing: the slot is not reserved against a
           second reading arriving, because a row whose value column was short by six cells
           until the radio repeated itself would reflow while being read. */
        want = (trailing->spark != NULL && trailing->spark->points != NULL &&
                trailing->spark->points->count >= 2U)
                   ? FB_SPARK_CELLS + 1U
                   : 0U;
        break;
    case FB_TRAILING_CHECKBOX:
    case FB_TRAILING_RADIO: {
        int width = 0;
        fb_selection_size(state, state->scale, &width, NULL);
        want = (size_t)((width + adv - 1) / adv) + 2U;
        break;
    }
    case FB_TRAILING_SEGMENTED:
        return fb_segmented_cols(state, cols > reserved ? cols - reserved : 0U, trailing->segmented,
                                 NULL);
    case FB_TRAILING_NONE:
    default:
        return 0U;
    }
    /* Strictly greater: the line keeps at least one cell of its own, which is the test the
       conversation cell made for its age before this was a shared slot. */
    return (want > 0U && cols > want + reserved) ? want : 0U;
}

/* One piece of a headline, clipped to the cells it was given - declared here for the segmented
   slot's fallback, which writes into the row's value column rather than the trailing edge and
   owes that column the same clipping every other piece of the line gets. */
static void fb_item_piece(struct mesh_ui_backend_fb_state *state, int x, int y, const char *text,
                          size_t cols, struct mesh_ui_rgb ink, struct mesh_ui_rgb ground);

/* `reserved` is the same figure fb_trailing_cols() was given - see there. Only the slot with
   two forms reads it, and it has to: a segmented button that measured itself against the free
   room and then drew itself against the whole line would be the one kind able to disagree with
   the measure that placed it. */
/*
 * `rest_role` is what the row is standing on when it is *not* the cursor's: the panel, or the
 * surface of the card its group was drawn on. A role rather than a mixed colour because the
 * controls in here take a role - fb_draw_segmented() needs to name what it is over, not to be
 * handed pixels.
 *
 * The two grounds below are deliberately different and it is not a shortcut that they come from
 * one parameter. Words and symbols are drawn *on* the cursor fill, so they blend against it. A
 * control that lays a patch of its own - the switch's ring, the meter's track bed - is laying
 * the row's **resting** ground under itself precisely to escape that fill: both are contracted
 * against what the row rests on, and on two of the four themes the cursor fill is the resting
 * track's own colour, so a patch in it would make the control vanish on exactly the row being
 * pointed at. `selected` is already here, so the ink's ground is derived rather than passed and
 * the two cannot be handed the wrong way round.
 */
/* `value_ink` is the ink the row's own value column is written in, and only the segmented
   slot's fallback uses it - see there. */
static void fb_draw_trailing(struct mesh_ui_backend_fb_state *state, const struct fb_item_geom *g,
                             size_t reserved, const struct fb_trailing *trailing, int baseline,
                             int slot_top, bool selected, enum mesh_ui_color rest_role,
                             struct mesh_ui_rgb value_ink) {
    const struct mesh_ui_rgb ground =
        fb_color(state, selected ? MESH_UI_COLOR_SURFACE_SEL : rest_role);
    const int scale = state->scale;
    const int adv = fb_char_adv(state, scale);
    const size_t cells =
        trailing->kind == FB_TRAILING_TEXT ? mesh_ui_text_cells(trailing->text) : 0U;

    switch (trailing->kind) {
    case FB_TRAILING_TEXT:
        if (cells == 0U) {
            return;
        }
        /* Always the quiet ink, on the ground and on the fill alike: a trailing figure is
           something the eye glances at on its way past, never the row's own words. */
        fb_draw_text(state, g->text_right - (int)cells * adv, baseline, trailing->text, scale,
                     selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                              : fb_tone_color(state, MESH_UI_TONE_DIM),
                     ground);
        return;
    case FB_TRAILING_BADGE: {
        /* The capsule is fb_draw_badge()'s, not this slot's: the top app bar's trailing slot
           draws the same thing, and two places filling their own round rect is two capsules
           that drift. What the row supplies is the box - a row knows where its own slot is. */
        const int width = fb_badge_width(state, trailing->text, scale);
        const struct fb_rect box = {
            .x = g->text_right - width, .y = slot_top, .w = width, .h = g->slot_h};
        fb_draw_badge(state, &box, baseline, trailing->text, trailing->family, scale);
        return;
    }
    case FB_TRAILING_ICON:
        /* The quiet ink a trailing figure takes, for the same reason: a chevron is something the
           eye passes on its way down the list, never one of the row's own words. */
        fb_draw_icon(state, g->text_right - fb_icon_box(state, scale), baseline, trailing->icon,
                     scale,
                     selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                              : fb_tone_color(state, MESH_UI_TONE_DIM),
                     ground);
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
        /* The row's resting ground, written here for the reason `selected` is: what a control
           is standing on is a fact about the row, and a screen asked to remember it is a screen
           that would forget on one list out of nine. Resting rather than current, because the
           ring exists to get the control *out* from under the cursor fill. */
        trailing->sw->ground = rest_role;
        fb_draw_switch(state, trailing->sw);
        return;
    }
    case FB_TRAILING_CHECKBOX:
    case FB_TRAILING_RADIO: {
        if (trailing->sel == NULL) {
            return;
        }
        int width = 0;
        int height = 0;
        fb_selection_size(state, scale, &width, &height);
        /* Centred on the row's *fill* rather than on the glyph body, for the reason the switch
           is: only the fill is what the control has to stay inside, and one that overhangs it
           notches the highlight on the one row the cursor is on. */
        trailing->sel->rect.w = width;
        trailing->sel->rect.h = height;
        trailing->sel->rect.x = g->text_right - width;
        trailing->sel->rect.y = g->fill_top + (g->fill_h - height) / 2;
        trailing->sel->selected = selected;
        /* Set from the kind, so a caller cannot name a radio and be handed a checkbox. There is
           one statement about which of the two this is and it is the slot's. */
        trailing->sel->shape =
            trailing->kind == FB_TRAILING_RADIO ? FB_SELECTION_RADIO : FB_SELECTION_CHECKBOX;
        fb_draw_selection(state, trailing->sel);
        return;
    }
    case FB_TRAILING_SEGMENTED: {
        bool as_text = true;
        const size_t want = fb_segmented_cols(state, g->cols > reserved ? g->cols - reserved : 0U,
                                              trailing->segmented, &as_text);
        if (want == 0U) {
            return;
        }
        if (as_text) {
            /*
             * The chosen word, in the row's own value column - because that is what it now is:
             * a setting whose value is a word, which is what every row around it is.
             *
             * It used to be drawn as a trailing text, quietly and against the right-hand edge,
             * and that was the control's fallback reasoning rather than the row's: the segments
             * were a trailing slot, so the word that replaced them took the trailing slot's
             * place. On the screen it read as a second value column - Display draws "Panel type
             * / Auto" in the column and drew "Layout / Default" against the edge, two rows
             * apart, for no reason a reader could see. A row that has a value column writes its
             * value there.
             *
             * Right-aligned only when there is no column to write in, which is a row that gave
             * its whole line to its words.
             */
            if (reserved > 0U && g->cols > reserved) {
                fb_item_piece(state, g->text_x + (int)reserved * adv, baseline,
                              trailing->segmented->value, g->cols - reserved, value_ink, ground);
                return;
            }
            fb_draw_text(state,
                         g->text_right - (int)mesh_ui_text_cells(trailing->segmented->value) * adv,
                         baseline, trailing->segmented->value, scale,
                         selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                                  : fb_tone_color(state, MESH_UI_TONE_DIM),
                         ground);
            return;
        }
        const int seg_scale = fb_segmented_scale(state);
        const int width = fb_segmented_width(state, trailing->segmented, seg_scale);
        /* The switch's height at the *row's* scale, not the segments': two controls in one
           column have to stand the same distance off their rows, and it is the labels that are
           chrome-sized, not the control. */
        const int height = fb_segmented_height(state, scale);
        const struct fb_rect box = {.x = g->text_right - width,
                                    .y = g->fill_top + (g->fill_h - height) / 2,
                                    .w = width,
                                    .h = height};
        fb_draw_segmented(state, &box, trailing->segmented, selected, rest_role, seg_scale);
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
        trailing->meter->ground = rest_role;
        fb_draw_meter(state, trailing->meter);
        return;
    }
    case FB_TRAILING_SIGNAL: {
        /*
         * The rungs against the trailing edge and the figure to their left, which is the order
         * a status bar puts the two in - the signal is the thing being scanned down the column,
         * so it is the thing that keeps the fixed edge.
         *
         * Both take the row's quiet pairing, exactly as a trailing age does, and the lit rungs
         * take the row's own ink. That is deliberately the same two colours the slot already
         * draws everything else in: a staircase is furniture the eye passes on its way down a
         * list, not one of the row's words, and giving quality a colour of its own would put a
         * third statement about the link on a row that has made two.
         */
        const struct mesh_ui_rgb quiet = selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                                                  : fb_tone_color(state, MESH_UI_TONE_DIM);
        const struct mesh_ui_rgb ink =
            fb_color(state, selected ? MESH_UI_COLOR_TEXT_ON_SEL : MESH_UI_COLOR_TEXT);
        /*
         * An unlit rung is the meter's track and not the dim text colour, which is what it was
         * first drawn as. The two are different jobs: dim text is held *above* the ground so it
         * stays readable, and a rung that is not lit has nothing to read - it is there to be
         * counted against the lit ones, so it wants the role the theme already validates as the
         * empty part of an indicator. As dim text the gap between two rungs and four was there
         * but had to be looked for.
         *
         * Except under the cursor, where the track is not a colour that can be relied on: two of
         * the four themes make it exactly the cursor fill, which is the reason fb_draw_meter()
         * lays a ground of its own. Rungs have gaps between them and no ground to lay, so they
         * take the pairing the cursor does validate.
         */
        const struct mesh_ui_rgb unlit =
            selected ? quiet : fb_color(state, MESH_UI_COLOR_METER_TRACK);
        const int width = (int)FB_SIGNAL_CELLS * adv;
        const int height = fb_icon_box(state, scale);
        const struct fb_rect box = {
            .x = g->text_right - width,
            /* Centred on the row's fill, as the switch and the meter are, so a list of them
               sits on one line however the glyph body and the fill differ. */
            .y = g->fill_top + (g->fill_h - height) / 2,
            .w = width,
            .h = height,
        };
        fb_draw_signal(state, &box, trailing->signal, ink, unlit);
        const size_t figure = mesh_ui_text_cells(trailing->text);
        if (figure > 0U) {
            fb_draw_text(state, box.x - adv - (int)figure * adv, baseline, trailing->text, scale,
                         quiet, ground);
        }
        return;
    }
    case FB_TRAILING_SPARK: {
        if (trailing->spark == NULL) {
            return;
        }
        /*
         * On the *line's* slot rather than centred on the row's fill, which is where the switch,
         * the meter and the staircase sit.
         *
         * The difference only shows on the row this was built for and it shows badly: the node
         * detail's battery row is two steps, a fact on the first and a banded bar across the
         * second, so a trend centred on the fill lands in the gap between them and draws its
         * floor through the bar. Every other slot in this column is on a row whose fill is one
         * step, which is why the fill and the line were the same box until now. The slot is the
         * box that means "beside these words", and beside the words is where this belongs.
         */
        const int height = g->slot_h > 0 ? g->slot_h : fb_sparkline_height(state, scale);
        trailing->spark->rect.w = (int)FB_SPARK_CELLS * adv;
        trailing->spark->rect.h = height;
        trailing->spark->rect.x = g->text_right - trailing->spark->rect.w;
        trailing->spark->rect.y = slot_top;
        trailing->spark->selected = selected;
        fb_draw_sparkline(state, trailing->spark);
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

/*
 * One piece of a headline: the words, clipped to the cells it was given, in the ink it was
 * given.
 *
 * The headline is drawn in pieces rather than composed into one string and drawn once, which is
 * the whole of what lets a label and its value take two inks - see fb_list_item.label_tone. The
 * clipping is per piece and the positions are absolute, so the value column lands in exactly
 * the cell the composed line used to put it in.
 */
static void fb_item_piece(struct mesh_ui_backend_fb_state *state, int x, int y, const char *text,
                          size_t cols, struct mesh_ui_rgb ink, struct mesh_ui_rgb ground) {
    if (cols == 0U) {
        return;
    }
    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", text != NULL ? text : "");
    mesh_ui_line_fit(&line, cols);
    fb_draw_text(state, x, y, mesh_ui_line_text(&line), state->scale, ink, ground);
}

void fb_list_item(struct mesh_ui_backend_fb_state *state, struct fb_list *list, uint32_t index,
                  const struct fb_list_item *item) {
    fb_list_chrome(state, list);
    const int scale = state->scale;
    const bool selected = fb_list_is_cursor(list, index);
    const uint32_t rows = fb_list_row_height(list, index);
    const struct fb_item_geom g = fb_item_measure(state, list, item, index, rows);

    if (selected) {
        const int radius = fb_radius(state, MESH_UI_SHAPE_SM);
        const int row_x = g.fill_x;
        const int row_w = g.fill_w;
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
    /* Where the row's tone is spent on its words. `label_plain` keeps them ordinary and leaves
       the tone to the disc and the accent edge, which is the whole of that flag; see
       fb_list_item.label_plain. Resolved once here so the plain row below and the label column
       further down cannot disagree about it. */
    const enum mesh_ui_tone text_tone = item->label_plain ? MESH_UI_TONE_NORMAL : item->tone;
    const struct mesh_ui_rgb head_ink = fb_item_ink(state, text_tone, selected, false);
    /* What every icon on this row is blended against: the fill if the cursor laid one down, and
       otherwise whatever the row is standing on - the panel, or the surface of the card its
       group was drawn on. Asked of the list rather than assumed, because a glyph carries
       coverage and not a mask: text told the wrong ground keeps its shape and gains a halo of a
       colour that is nowhere near it. */
    const enum mesh_ui_color rest_role = fb_list_ground(list, index);
    const struct mesh_ui_rgb ground =
        fb_color(state, selected ? MESH_UI_COLOR_SURFACE_SEL : rest_role);

    if (item->leading.kind == FB_LEADING_AVATAR || item->leading.kind == FB_LEADING_TONAL) {
        /* The slot the measure reserved, and not a second opinion about it: a disc drawn to any
           other size either leaves a gap its list's other rows do not have or runs under the
           words. Centred in the slot where the measure had to make it smaller than one, so the
           room a card took comes off the disc and never off the column. See fb_item_measure(). */
        const int size = g.lead_disc;
        const int lead_x = g.content_x + (g.lead_size - size) / 2;
        /*
         * Which pair the disc wears, and the tonal one reads it off the row's tone exactly as
         * the accent bar below does - the row says once what it means and the disc is one of
         * the renderings of that, never a second opinion.
         *
         * The container at rest, because a symbol has to sit on this and a column of them is
         * read rather than spotted. Under the cursor it commits to the family's full strength
         * instead, which is fb_button_paint()'s rule for a tonal control word for word, and it
         * is a correction rather than a flourish: a container is picked to be a quiet fill on
         * the body ground, the cursor's own fill is picked to be a quiet fill on the body
         * ground, and two quiet fills are necessarily near each other. Laid on the cursor it
         * came to 1.01:1 on the dark palette's success and 1.04:1 on the colour-blind error -
         * a disc that vanishes on precisely the row being pointed at. The state layer made it
         * worse rather than better, because a layer can only move a fill towards its own ink.
         *
         * The base is the one half of a family the theme already holds to being findable on
         * that fill: it is the pair the marker bar down a selected row is checked as. So the
         * focused row's disc brightens instead of disappearing, and no palette had to move.
         */
        struct mesh_ui_paint disc;
        if (item->leading.kind == FB_LEADING_TONAL) {
            const enum mesh_ui_family family = mesh_ui_tone_family(item->tone);
            disc =
                fb_paint(state, family != MESH_UI_FAMILY_COUNT ? family : MESH_UI_FAMILY_PRIMARY,
                         selected ? MESH_UI_SLOT_BASE : MESH_UI_SLOT_CONTAINER, MESH_UI_STATE_REST);
        } else {
            disc =
                (struct mesh_ui_paint){item->leading.role < MESH_UI_COLOR_COUNT
                                           ? fb_color(state, item->leading.role)
                                           : mesh_ui_theme_avatar(state->theme, item->leading.tint),
                                       fb_color(state, MESH_UI_COLOR_BG)};
        }
        fb_draw_avatar(state, lead_x, g.lead_y, size, item->leading.label, item->leading.icon,
                       disc);
    } else if (item->leading.kind == FB_LEADING_ICON) {
        fb_draw_icon(state, g.content_x, g.head_y, item->leading.icon, scale, head_ink, ground);
    }

    /*
     * What the headline has already spent *inside `g.cols`* before its trailing slot gets a say.
     *
     * Only the label column, and that is the whole of the subtlety. A plain row's marker gutter
     * is spent too, but it is spent by fb_item_measure() moving `g.text_x` past it before the
     * columns are counted - so it is already outside this number, and reserving it again took a
     * cell off every row with a marker slot. The label column is the other way round: it is
     * drawn inside `g.cols` rather than measured out of it, so nothing has counted it yet.
     */
    const size_t reserved = item->label_cols > 0U ? item->label_cols + FB_ITEM_MARKER_CELLS : 0U;
    const size_t head_take = fb_trailing_cols(state, g.cols, reserved, &item->trailing);
    const size_t head_cols = g.cols - head_take;
    if (item->label_cols > 0U) {
        /*
         * The label column, then the value in the cell the marker gutter leaves after it. Two
         * draws rather than one, so the question and the answer can be two tiers - which is the
         * whole of what fb_list_item.label_tone is for.
         *
         * Both are clipped against `head_cols` rather than against their own widths, which is
         * what keeps this identical to the composed line it replaced: a label column wider than
         * the row cut the label and left the value nowhere to start, and a value column that
         * the trailing slot has eaten into is cut at the same cell either way.
         */
        /* The row's own tone unless the row said the label is its quiet tier, which is what
           keeps a dim section and a strong unsaved field marked across both halves. */
        const enum mesh_ui_tone label_tone = item->label_quiet ? MESH_UI_TONE_DIM : text_tone;
        const size_t label_cols = item->label_cols < head_cols ? item->label_cols : head_cols;
        fb_item_piece(state, g.text_x, g.head_y, item->label, label_cols,
                      fb_item_ink(state, label_tone, selected, item->label_quiet), ground);
        const size_t gutter = item->label_cols + FB_ITEM_MARKER_CELLS;
        if (head_cols > gutter) {
            const int value_x = g.text_x + (int)gutter * fb_char_adv(state, scale);
            const size_t value_cols = head_cols - gutter;
            /* The capsule, where the row said its value is a state and the column is wide
               enough to hold one. Measured against the room the words would have had, so a
               chip never runs under a trailing slot - and drawn as words when it does not fit,
               which is the fallback the row has because the caller supplied the text either
               way. */
            const int chip_w = item->value_chip ? fb_badge_width(state, item->value, scale) : 0;
            if (chip_w > 0 && chip_w <= (int)value_cols * fb_char_adv(state, scale)) {
                const struct fb_rect box = {value_x, g.head_slot_top, chip_w, g.slot_h};
                fb_draw_state_chip(state, &box, g.head_y, item->value, item->tone,
                                   selected ? MESH_UI_COLOR_SURFACE_SEL : rest_role, head_ink,
                                   scale);
            } else {
                fb_item_piece(state, value_x, g.head_y, item->value, value_cols, head_ink, ground);
            }
        }
    } else {
        fb_item_piece(state, g.text_x, g.head_y, item->text, head_cols, head_ink, ground);
    }
    /* Into the blank cell the label column and the value leave between them, and only when the
       value column actually got that far - a label column wider than the row is clipped, and a
       marker drawn at a column the words no longer reach would sit on top of the label. */
    if (item->label_cols > 0U && mesh_ui_icon_is_valid(item->marker_icon) &&
        g.cols > item->label_cols + FB_ITEM_MARKER_CELLS) {
        fb_draw_icon(state, g.text_x + (int)(item->label_cols + 1U) * fb_char_adv(state, scale),
                     g.head_y, item->marker_icon, scale, head_ink, ground);
    } else if (item->label_cols == 0U && item->marker_slot &&
               mesh_ui_icon_is_valid(item->marker_icon)) {
        /* Into the cell the measure held back before the words. In the row's own ink, like every
           other icon in a row's slots: the star is as loud as the name it sits beside. */
        fb_draw_icon(state, g.marker_x, g.head_y, item->marker_icon, scale, head_ink, ground);
    }
    if (head_take > 0U) {
        fb_draw_trailing(state, &g, reserved, &item->trailing, g.head_y, g.head_slot_top, selected,
                         rest_role, head_ink);
    }

    if (g.rows >= 2U && item->supporting != NULL) {
        const struct mesh_ui_rgb supp_ink =
            fb_item_ink(state, item->supporting_tone, selected, item->supporting_quiet);
        /* Nothing reserved: a supporting line has no label column, and the icon that may take
           its first cell is tested against what the slot leaves rather than before it. */
        const size_t supp_take = fb_trailing_cols(state, g.cols, 0U, &item->supporting_trailing);
        /* An icon on the supporting line takes the first cell and the words move over, which is
           what "> " did when it was two characters of the preview. */
        const bool supp_icon =
            mesh_ui_icon_is_valid(item->supporting_icon) && g.cols > supp_take + 1U;
        const int supp_x = g.text_x + (supp_icon ? fb_char_adv(state, scale) : 0);
        if (supp_icon) {
            fb_draw_icon(state, g.text_x, g.supp_y, item->supporting_icon, scale, supp_ink, ground);
        }
        fb_item_piece(state, supp_x, g.supp_y, item->supporting,
                      g.cols - supp_take - (supp_icon ? 1U : 0U), supp_ink, ground);
        if (supp_take > 0U) {
            fb_draw_trailing(state, &g, 0U, &item->supporting_trailing, g.supp_y, g.supp_slot_top,
                             selected, rest_role, supp_ink);
        }
    }

    /*
     * The bar the row gave a step to, across the width the words had.
     *
     * Only when the list actually gave it that step. A screen that declared a meter row one
     * step tall has made a mistake, and the two ways of answering it are to draw the bar over
     * whatever is under this row or to leave it out; leaving it out is the one a screen notices
     * and the one that cannot corrupt the frame. It is the same rule the trailing slots follow
     * when the line is too narrow for them.
     */
    if (item->meter != NULL && g.bar_h > 0) {
        item->meter->rect.x = g.text_x;
        item->meter->rect.w = g.text_right - g.text_x;
        item->meter->rect.h = g.bar_h;
        item->meter->rect.y = g.bar_y;
        item->meter->selected = selected;
        item->meter->ground = rest_role;
        fb_draw_meter(state, item->meter);
    } else if (item->slider != NULL && g.bar_h > 0) {
        /* The same box the meter would have had, and the control centres its own track in it -
           so a reading and the control that sets it start and end in the same two columns, which
           is what lets a section mix the two without the eye finding two different lists. */
        item->slider->rect.x = g.text_x;
        item->slider->rect.w = g.text_right - g.text_x;
        item->slider->rect.h = g.bar_h;
        item->slider->rect.y = g.bar_y;
        item->slider->selected = selected;
        fb_draw_slider(state, item->slider);
    }

    /*
     * The divider goes in the gap the tightened leading opened up, inset to where the text
     * starts rather than run edge to edge: a leading disc already separates the items, and a
     * full-width rule under one reads as a box drawn around it.
     */
    const bool last = (index + 1U >= list->model.count) ||
                      (index + 1U >= list->model.first + list->model.visible);
    if (item->divider && !selected && !last) {
        fb_draw_rule(state, g.text_x, g.fill_top + g.fill_h + fb_space(state, MESH_UI_SPACE_XS),
                     g.text_right - g.text_x, scale, MESH_UI_COLOR_RULE);
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
    fb_list_chrome(state, list);
    /*
     * Unread, and allowed to say so.
     *
     * The two emphases a new message earns - the preview at full weight, and the row refusing
     * to stay quiet under the cursor - are exactly what a mute is asking this row to stop
     * doing, so they are decided together here rather than by the caller passing `unread`
     * false. The count itself is untouched: it goes on drawing, one family quieter.
     */
    const bool emphasis = conversation->unread && !conversation->muted;
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
        /* The bell with a stroke through it, in the gutter the pinned star uses. The slot is
           reserved on every row and not only on the muted ones, because a list that indents
           only some of its rows is a list whose names start in two columns. */
        .marker_icon = conversation->muted ? MESH_UI_ICON_MUTED : MESH_UI_ICON_NONE,
        .marker_slot = true,
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
        .supporting_tone = conversation->armed ? MESH_UI_TONE_ERROR
                           : emphasis          ? MESH_UI_TONE_STRONG
                                               : MESH_UI_TONE_DIM,
        .supporting_quiet = !conversation->armed && !emphasis,
        /* Secondary rather than the accent on a muted row - see struct fb_conversation. The
           capsule is still there and still counts; it just stops being the loudest thing on
           the screen, which is the whole of what the user asked for. */
        .supporting_trailing = {.kind = FB_TRAILING_BADGE,
                                .family = conversation->muted ? MESH_UI_FAMILY_SECONDARY
                                                              : MESH_UI_FAMILY_PRIMARY,
                                .text = conversation->badge},
        .accent_edge = true,
        .divider = true,
    };
    fb_list_item(state, list, index, &item);
}

size_t fb_field_label_cols(const struct mesh_ui_backend_fb_state *state,
                           const struct fb_layout *layout, size_t preferred) {
    const struct mesh_ui_metrics *metrics = fb_metrics(state);
    if (preferred == 0U) {
        preferred = metrics->field_label_cols;
    }
    return layout->cols < metrics->narrow_cols ? layout->cols / 2U : preferred;
}
