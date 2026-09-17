#define _POSIX_C_SOURCE 200809L

/*
 * The card: its geometry, the rows a screen puts in it, and the one pass that draws them.
 */

#include "fb_widgets_card.h"
#include "fb_widgets_button.h"
#include "fb_widgets_meter.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/emoji.h"
#include "mesh/ui/layout.h"
#include "mesh/utils/text.h"

#include <stdarg.h>
#include <string.h>

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
    int x, width; /* the panel, edge included */
    int pad;      /* the inset from the side edges to the content */
    int pad_y;    /* the inset from the top and bottom edges */
    int edge;     /* the hairline's thickness, and the only edge the layout knows about */
    /* What is actually painted around the panel: the hairline, or the thicker focus ring. It is
       deliberately not `edge`, because `edge` is in the content inset and in the box height -
       so a ring that widened it would move the card's text and shift every card below it by a
       few pixels for no reason but the cursor arriving. The ring grows *inward*, into the
       padding, which is what keeps the outer geometry a fact about the card rather than about
       what is selected. */
    int ring;
    int radius; /* corner radius, clamped by fb_fill_round_rect() anyway */
    /* The header line: the heading, and the verbs against its far edge. 0 when there is
       neither. */
    int heading_h;
    int button_h;  /* one action button, 0 when the card has no verbs */
    int content_x; /* where a row's text starts */
    size_t cols;   /* content width in cells */
    size_t label_cols;
    int gap;                     /* to the next card */
    enum mesh_ui_color fill;     /* the variant's surface */
    struct mesh_ui_rgb edge_ink; /* the hairline, or the focus ring when a verb is selected */
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
    /*
     * The variant, as a fill and nothing else. Three tiers rather than three shadows: there is
     * no alpha on this panel to cast one into, so how far a card is off the ground is carried
     * by the surface it is drawn on - which is Material's tonal elevation, and is why the
     * three survive a light palette as well as a dark one.
     */
    switch (card->variant) {
    case FB_CARD_ELEVATED:
        m.fill = MESH_UI_COLOR_SURFACE_HIGH;
        break;
    case FB_CARD_OUTLINED:
        m.fill = MESH_UI_COLOR_BG;
        break;
    case FB_CARD_FILLED:
    default:
        m.fill = MESH_UI_COLOR_SURFACE;
        break;
    }
    /*
     * The focus ring. A card holding the selected verb is the one the next press acts on, and
     * that has to be findable before any of it is read - so it is the edge that changes rather
     * than the fill, drawn in the accent and at twice the thickness. See fb_draw_card().
     *
     * Only what is painted changes. `m.edge` stays the hairline, so the content inset, the
     * label column and the box height are the same whether the card is focused or not; the
     * extra thickness is taken out of the padding instead, capped so it can never reach the
     * text. Widening the layout edge here made a card grow when the cursor arrived and pushed
     * every card under it down the panel, which is a repaint of the whole screen to say one
     * thing about one card.
     */
    bool focused = false;
    for (uint32_t i = 0U; i < card->action_count && i < FB_CARD_ACTIONS_MAX; ++i) {
        focused = focused || card->actions[i].selected;
    }
    m.ring = m.edge;
    if (focused) {
        m.ring = m.edge * 2;
        m.edge_ink = fb_tone_color(state, MESH_UI_TONE_PRIMARY);
    } else {
        m.edge_ink = fb_color(state, MESH_UI_COLOR_OUTLINE);
    }
    m.radius = fb_radius(state, MESH_UI_SHAPE_MD);
    /* A heading is drawn at the chrome scale, the size the tab strip and the footer are: a
       section label is not something to read, it is something to find, and at the body scale it
       costs a whole row of content on a panel that has fifteen of them. */
    m.heading_h = card->heading[0] != '\0' ? fb_line_adv(state, layout->small) : 0;
    /*
     * The verbs sit on the heading's line, against the far edge, and they are drawn at the
     * chrome scale the heading is.
     *
     * A row of buttons under the content is where a card puts its actions on a phone, and it is
     * what this was first written as. It cost two rows off the bottom of the Status screen -
     * one per card carrying a verb - and that screen is the one that can outgrow the panel, so
     * the two rows it lost were a refused packet and a reboot count: the exact rows the card
     * exists to show. A heading is three or four cells of a line that is otherwise empty, and
     * putting the verbs in the rest of it costs nothing at all.
     *
     * The scale follows for the same reason it does on the action bar, which draws every other
     * verb on the frame: a keycap and a verb are chrome, read beside the content rather than as
     * part of it. It also keeps this line the height it already was - a body-scale button here
     * would have grown the header by the difference and given a third of a row back.
     */
    m.button_h = card->action_count > 0U ? fb_line_adv(state, layout->small) : 0;
    if (m.button_h > m.heading_h) {
        m.heading_h = m.button_h;
    }

    /* Never into the content: the ring lives in the padding, and a theme with a thick hairline
       and a tight inset must lose the ring rather than the row it would eat. Bounded by the
       *vertical* inset, which is the smaller of the two - the first row's baseline is what a
       ring grown too far would land on. */
    if (m.ring > m.edge + m.pad_y) {
        m.ring = m.edge + m.pad_y;
    }

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

/*
 * Body rows one row of content occupies. A note is the only one that is more than one, because
 * it is the only one with more words than fit on a line: every picture a card can carry is a
 * *length*, and a length is as thin as the theme's bar.
 *
 * There was a second, and losing it is worth a sentence because it is the rule arriving from the
 * other side. A trend row cost two, since a sparkline's reading is a shape rather than a length
 * and a shape in the height of a hairline is a hairline. It had one caller - the Status screen's
 * airtime block - and that screen now opens the same readings as a chart, which says everything
 * the two rows said and four things they could not. So the shape went to a screen and the card
 * kept the bar, and what is left here is the general form: a card draws levels, and a shape
 * wants a body.
 */
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

void fb_card_begin(struct fb_card *card, enum fb_card_variant variant, enum mesh_ui_icon icon,
                   enum mesh_str_id heading, enum mesh_ui_tone tone) {
    if (card == NULL) {
        return;
    }
    memset(card, 0, sizeof *card);
    card->variant = variant;
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
                   int32_t value, struct mesh_ui_scale scale, const struct mesh_ui_band *band,
                   uint32_t id) {
    struct fb_card_row *row = fb_card_next_row(card, FB_CARD_ROW_METER, tone);
    if (row == NULL) {
        return;
    }
    if (label != MESH_STR_NONE) {
        mesh_text_sanitise_str(mesh_str(label), row->label, sizeof row->label);
    }
    row->meter_value = value;
    row->meter_scale = scale;
    row->meter_banded = band != NULL;
    if (band != NULL) {
        row->meter_band = *band;
    }
    row->meter_id = id;
}

void fb_card_proportion(struct fb_card *card, enum mesh_ui_tone tone, enum mesh_str_id label,
                        const uint32_t *values, uint32_t count) {
    if (values == NULL || count < 2U || count > MESH_UI_PROPORTION_PARTS) {
        return;
    }
    /* The whole, tested here rather than in the drawing, so that a card asking for a composition
       and a card asking for a trend answer an absent reading the same way: with no row. */
    uint64_t total = 0U;
    for (uint32_t i = 0; i < count; ++i) {
        total += values[i];
    }
    if (total == 0U) {
        return;
    }
    struct fb_card_row *row = fb_card_next_row(card, FB_CARD_ROW_PROPORTION, tone);
    if (row == NULL) {
        return;
    }
    if (label != MESH_STR_NONE) {
        mesh_text_sanitise_str(mesh_str(label), row->label, sizeof row->label);
    }
    for (uint32_t i = 0; i < count; ++i) {
        row->parts[i] = values[i];
    }
    row->part_count = count;
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

void fb_card_action(struct fb_card *card, enum mesh_str_id label, bool selected) {
    if (card == NULL || card->action_count >= FB_CARD_ACTIONS_MAX || label == MESH_STR_NONE) {
        return;
    }
    struct fb_card_action *action = &card->actions[card->action_count++];
    memset(action, 0, sizeof *action);
    mesh_text_sanitise_str(mesh_str(label), action->label, sizeof action->label);
    action->selected = selected;
}

bool fb_card_is_empty(const struct fb_card *card) { return card == NULL || card->count == 0U; }

int fb_card_height(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   const struct fb_card *card) {
    if (fb_card_is_empty(card)) {
        return 0;
    }
    const struct fb_card_metrics m = fb_card_measure(state, layout, card);
    const struct fb_card_fit whole = {.rows = card->count, .tail_lines = 0U};
    return fb_card_box_height(&m, layout, card, whole);
}

/*
 * A card row's label column, drawn, and the x its answer starts at.
 *
 * The quiet tier of the two a stated fact has, and every row of every card here is one: what
 * the link is doing, what the mesh has heard, what the radio last said about itself. The label
 * is the question and repeats down a column the reader is scanning for the *answers*, so it
 * recedes and the row's own tone stays on the value - a link that is up is a green reading, and
 * "Sync" in green said nothing "Sync" did not. Composed into one string and drawn in one colour
 * the two were typographically identical, which is what made a card of them read as a block of
 * text with no way into it: the node detail's complaint, one component over, fixed its way.
 *
 * Stated here rather than asked of the caller because a card is not a list. fb_list_item() takes
 * `label_quiet` per row because a settings section mixes readings with controls; nothing a card
 * draws is a control - the verbs a card offers are buttons beside its heading, not rows - so a
 * flag here would be a question with one answer.
 *
 * One cell between the column and what follows it, which is the space the composed line carried,
 * so nothing moves sideways. A row with no label at all keeps the whole width and is the caller
 * saying the bar is the row; see fb_card_meter().
 */
static int fb_card_row_label(struct mesh_ui_backend_fb_state *state,
                             const struct fb_card_metrics *m, int y, const struct fb_card_row *row,
                             struct mesh_ui_rgb ground) {
    if (row->label[0] == '\0') {
        return m->content_x;
    }
    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    mesh_ui_line_column(&line, row->label, m->label_cols);
    mesh_ui_line_fit(&line, m->cols);
    fb_draw_text(state, m->content_x, y, mesh_ui_line_text(&line), state->scale,
                 fb_tone_color(state, MESH_UI_TONE_DIM), ground);
    return m->content_x + (int)(m->label_cols + 1U) * fb_char_adv(state, state->scale);
}

/* One row of content, drawn at `y` and returning the rows it used. `max_lines` of 0 means the
   row's own count; anything else is the budget a clipped note has been given. */
static uint32_t fb_draw_card_row(struct mesh_ui_backend_fb_state *state,
                                 const struct fb_card_metrics *m, const struct fb_layout *layout,
                                 int y, const struct fb_card_row *row, uint32_t max_lines) {
    const struct mesh_ui_rgb color = fb_tone_color(state, row->tone);
    /* Every row here is inside the panel fb_draw_card() filled, not on the ground it sits on -
       and which fill that is depends on the card's variant, so it comes from the metrics rather
       than from the surface role a card used to be. */
    const struct mesh_ui_rgb ground = fb_color(state, m->fill);
    if (row->kind == FB_CARD_ROW_NOTE) {
        /* fb_draw_wrapped() lays out from the left margin, and a card's content starts inside
           it, so the note is wrapped here against the card's own column. */
        const uint32_t budget = max_lines > 0U ? max_lines : FB_CARD_NOTE_LINES;
        struct mesh_ui_wrap wrap;
        mesh_ui_wrap_begin(&wrap, row->value, m->cols);
        uint32_t drawn = 0U;
        while (drawn < budget && mesh_ui_wrap_next(&wrap)) {
            fb_draw_text(state, m->content_x, y + (int)drawn * layout->line, wrap.line,
                         state->scale, color, ground);
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
        const int bar_x = fb_card_row_label(state, m, y, row, ground);
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
                .scale = row->meter_scale,
                .band = row->meter_banded ? &row->meter_band : NULL,
                .tone = row->tone,
            };
            fb_draw_meter(state, &meter);
        }
        return 1U;
    }

    if (row->kind == FB_CARD_ROW_PROPORTION) {
        /* The meter row's own layout, because a composition is a bar and a card that placed its
           two kinds of bar differently would be reporting a difference that is not there. */
        const int adv = fb_char_adv(state, state->scale);
        const int bar_x = fb_card_row_label(state, m, y, row, ground);
        const int bar_right = m->content_x + (int)m->cols * adv;
        const int height = fb_proportion_thickness(state, state->scale);
        if (bar_right - bar_x > 0) {
            struct fb_proportion bar = {
                .rect = {.x = bar_x,
                         .y = y + (layout->line - state->scale - height) / 2,
                         .w = bar_right - bar_x,
                         .h = height},
                .count = row->part_count,
                /* The card's own fill, which depends on its variant - the same colour every
                   other row on it draws its text over. */
                .ground = ground,
            };
            for (uint32_t i = 0; i < row->part_count && i < MESH_UI_PROPORTION_PARTS; ++i) {
                bar.values[i] = row->parts[i];
            }
            fb_draw_proportion(state, &bar);
        }
        return 1U;
    }

    const int value_x = fb_card_row_label(state, m, y, row, ground);
    /* A card whose label column has eaten the whole width has no room left to answer in, and
       says nothing rather than spilling past the panel. */
    const size_t taken = (size_t)((value_x - m->content_x) / fb_char_adv(state, state->scale));
    if (taken < m->cols) {
        struct mesh_ui_line line;
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%s", row->value);
        mesh_ui_line_fit(&line, m->cols - taken);
        fb_draw_text(state, value_x, y, mesh_ui_line_text(&line), state->scale, color, ground);
    }
    return 1U;
}

int fb_card_min_height(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                       const struct fb_card *card) {
    if (fb_card_is_empty(card)) {
        return 0;
    }
    const struct fb_card_metrics m = fb_card_measure(state, layout, card);
    /* One row, which is the same point fb_draw_card() refuses a card at: a heading with nothing
       under it is not a card. What the row costs is the row's own - a note that wraps to three
       lines is three - because the minimum has to be a card that can actually be drawn. */
    const struct fb_card_fit least = {.rows = 1U, .tail_lines = 0U};
    /* The box and no gap, exactly as fb_card_height() answers now. The gap *between* the two
       cards is real and still has to be paid for - it is just not this card's to state, because
       it belongs to whichever card is reserving the room and is added by
       fb_draw_card_reserving(). Counting it here as well would spend a row of content on air,
       and counting it in one of the pair and not the other is a reservation that is a row too
       generous depending on which of them a screen asked. */
    return fb_card_box_height(&m, layout, card, least);
}

bool fb_draw_card_reserving(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                            int *y, const struct fb_card *card, int reserve) {
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
    /*
     * And `reserve` off that, which is this card being told to leave room for what comes after
     * it.
     *
     * A column of cards is drawn in order and each one takes what it wants, so the last card is
     * the one that pays for everything above it - and paying, here, means not being drawn at
     * all. That is worse than losing a row: a card carries *verbs*, and which verbs a screen
     * offers is a table (src/ui/tables/status.c) that knows nothing about how tall anything came
     * out. So the cursor keeps walking onto a button that is not on the frame, which is the failure
     * "a card that can end up with no rows must not be given a verb" already names, arrived at
     * from the layout side instead of the row-count side.
     *
     * A reservation turns that around: the card that can afford to lose a row loses one, and the
     * card that would otherwise vanish survives. Which is also the right order editorially - the
     * rows that go are the last ones a screen declared, and a screen declares its least
     * important rows last.
     */
    /*
     * Plus this card's own gap, because that is what separates the two: `*y` advances past the
     * box *and* the gap, so the card being reserved for starts a gap lower than this one ends.
     * Reserving the bare content height leaves it exactly one gap short, which on this panel is
     * the difference between the card being drawn and not.
     */
    int limit = reserve > 0 ? bottom - reserve - m.gap : bottom;
    struct fb_card_fit fit = fb_card_clip(&m, layout, card, *y, limit);
    if (fit.rows == 0U && fit.tail_lines == 0U) {
        /*
         * The reservation yields rather than erasing this card.
         *
         * It is a promise about the card *below*, and a promise that cannot be kept without
         * deleting the card above it is not worth keeping - two cards missing is not an
         * improvement on one. So a reservation that cannot be afforded is dropped, and the
         * column degrades to what it did before: whoever is drawn first gets the room.
         */
        fit = fb_card_clip(&m, layout, card, *y, bottom);
    }
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
     * not. mesh_ui_theme_validate() holds OUTLINE against the ground and both surfaces, which
     * is every fill a variant can put behind it - and the outlined variant, whose fill *is* the
     * ground, is why it has to hold against all three rather than against the panel's own.
     *
     * On the card holding the selected verb both of those change: the ink is the accent and the
     * painted thickness is doubled, which is the focus ring. Only the *painted* thickness - the
     * panel is the same size and its content starts in the same place either way, so the ring
     * grows inward into the padding. See fb_card_measure().
     */
    const int top = *y;
    const int inner_radius = m.radius + m.edge - m.ring > 0 ? m.radius + m.edge - m.ring : 0;
    fb_fill_round_rect(state, m.x, top, m.width, height, m.radius + m.edge, m.edge_ink);
    fb_fill_round_rect(state, m.x + m.ring, top + m.ring, m.width - 2 * m.ring, height - 2 * m.ring,
                       inner_radius, fb_color(state, m.fill));

    int row_y = top + m.pad_y + m.edge;
    /*
     * The verbs, against the far edge of the heading's line.
     *
     * Laid out from the right so the first one declared ends up leftmost, which is the order
     * the screen cursor walks them in - a strip that packed from the left would have reversed
     * that on any card with two. They are text buttons: a word in the accent, and a fill only
     * under the cursor, which is what keeps three cards' worth of verbs from competing with the
     * numbers they are about and makes the selected one unmistakable with no second cue.
     */
    const int content_right = m.content_x + (int)m.cols * fb_char_adv(state, state->scale);
    const int gap = fb_space(state, MESH_UI_SPACE_XS);
    int actions_x = content_right;
    if (m.button_h > 0) {
        /*
         * How many of them there is room for, dropped from the *end* rather than from wherever
         * the layout ran out. Laying out from the right and stopping when the next one no
         * longer fits would drop the leftmost, which is the first the cursor reaches - so a
         * card too narrow for its verbs would lose the one A runs first.
         */
        uint32_t drawn = card->action_count;
        while (drawn > 0U) {
            int total = 0;
            for (uint32_t i = 0U; i < drawn; ++i) {
                total += fb_button_width(state, MESH_UI_ICON_NONE, card->actions[i].label,
                                         layout->small);
                if (i > 0U) {
                    total += gap;
                }
            }
            if (m.content_x + total <= content_right) {
                break;
            }
            drawn -= 1U;
        }
        for (uint32_t i = drawn; i-- > 0U;) {
            const int width =
                fb_button_width(state, MESH_UI_ICON_NONE, card->actions[i].label, layout->small);
            actions_x -= width;
            const struct fb_button button = {
                .rect = {.x = actions_x, .y = row_y, .w = width, .h = m.button_h},
                .icon = MESH_UI_ICON_NONE,
                .label = card->actions[i].label,
                .selected = card->actions[i].selected,
                .variant = FB_BUTTON_TEXT,
                .shape = MESH_UI_SHAPE_FULL,
                .idle_tone = MESH_UI_TONE_PRIMARY,
                .ground = m.fill,
                .scale = layout->small,
            };
            fb_draw_button(state, &button);
            actions_x -= gap;
        }
    }
    if (m.heading_h > 0 && card->heading[0] != '\0') {
        /* The heading takes the card's tone, which is how a card reports on what it holds
           without a second cue: the Link card goes bad when the radio has gone. Every tone a
           card can take is validated against the surface, so none of them can go quiet here. */
        struct mesh_ui_line line;
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%s", card->heading);
        const struct mesh_ui_rgb ink = fb_tone_color(state, card->tone);
        int heading_x = m.content_x;
        if (mesh_ui_icon_is_valid(card->icon)) {
            /* On the card's own surface, which is what it was just filled with - the icon is
               inside the panel, not on the ground the panel sits on. */
            fb_draw_icon(state, heading_x, row_y, card->icon, layout->small, ink,
                         fb_color(state, m.fill));
            /* The same half-cell a button leaves between its symbol and its word. */
            heading_x += fb_icon_box(state, layout->small) + fb_char_adv(state, layout->small) / 2;
        }
        /* Fitted to what the verbs left rather than to the card, so a long heading is cut on a
           cell boundary instead of running under the first button. */
        const int heading_adv = fb_char_adv(state, layout->small);
        const int heading_w = actions_x - heading_x;
        mesh_ui_line_fit(&line, heading_w >= heading_adv ? (size_t)(heading_w / heading_adv) : 1U);
        fb_draw_text(state, heading_x, row_y, mesh_ui_line_text(&line), layout->small, ink,
                     fb_color(state, m.fill));
    }
    if (m.heading_h > 0) {
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

bool fb_draw_card(struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout, int *y,
                  const struct fb_card *card) {
    return fb_draw_card_reserving(state, layout, y, card, 0);
}
