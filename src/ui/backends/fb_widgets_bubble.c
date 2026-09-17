#define _POSIX_C_SOURCE 200809L

/*
 * The chat bubble: the wrapped body, the quoted line over it, the reactions under it, and the
 * meta row carrying the time, the relay chip and the delivery mark.
 */

#include "fb_widgets_bubble.h"
#include "fb_widgets_chrome.h"

#include "mesh/ui/emoji.h"
#include "mesh/ui/layout.h"

#include <string.h>

/* ---- chat bubbles ------------------------------------------------------------------------- */

/* An icon stands in one cell, like a glyph, and one cell of air separates two parts of the
   trailing run. Both are counted by the measure and spent by the draw. */
#define FB_BUBBLE_ICON_CELLS 1U
#define FB_BUBBLE_META_GAP 1U

/* Reactions, relay chip, padlock, clock, delivery mark: the run is never longer than its five
   slots. */
#define FB_BUBBLE_META_PARTS 5U

/* One drawable part of a trailing run: a text or an icon, never both. */
struct fb_bubble_part {
    const char *text; /* NULL when this part is an icon */
    enum mesh_ui_icon icon;
    size_t cells;
};

/*
 * What a bubble works out to at this width, computed once and handed to both the measure and
 * the draw. Everything is in cells except `rows`, which is body rows.
 */
struct fb_bubble_metrics {
    size_t cols;        /* inner content width */
    uint32_t lines;     /* wrapped text lines */
    uint32_t notes;     /* wrapped lines of the failure reason under it */
    size_t last;        /* the width of the last line drawn, which the run tucks onto */
    bool meta_own_line; /* the run did not fit on the last one */
    struct fb_bubble_part parts[FB_BUBBLE_META_PARTS];
    size_t part_count;
    size_t meta_cols;  /* what the parts and their gaps come to; 0 when there is no run */
    size_t quote_cols; /* the quote line's text, elided to fit; 0 when there is no quote */
    uint32_t rows;
};

/* The bar down a quote's left edge and the gap after it, in cells. Both live here so the
   measure's width and the draw's text origin cannot disagree about the indent. */
#define FB_BUBBLE_QUOTE_INDENT 2U

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
 * The colour for a bubble's quieter lines - the sender on one of ours, the clock and the marks
 * beside it on any of them.
 *
 * Dim while the bubble sits at rest, and the bubble's own ink once the cursor is on it or the
 * fill has gone red. The selected fills are a step towards their ink by design, and dim over
 * one of those is the pairing a theme has least room for: on the dark palette it measured
 * 1.9:1, well under the 3:1 a secondary line is held to, and the bubble's own ink is a pair the
 * theme is validated on by construction.
 *
 * The delivery mark takes this ink too rather than a tone of its own. A failed message is
 * already drawn in the error family, so a red tick would be the fill said twice; and "gone out"
 * against "acknowledged" is a difference of one tick, which is the difference every messenger
 * has taught everybody to read. It also keeps the mark inside a pairing the theme is already
 * validated on, instead of asking every palette for a sixth one.
 */
static struct mesh_ui_rgb fb_bubble_quiet(const struct mesh_ui_backend_fb_state *state,
                                          const struct fb_bubble *bubble,
                                          struct mesh_ui_paint paint) {
    if (bubble->selected || bubble->failed) {
        return paint.ink;
    }
    return fb_tone_color(state, MESH_UI_TONE_DIM);
}

static void fb_bubble_part_text(struct fb_bubble_part *parts, size_t *count, const char *text) {
    if (!fb_bubble_has(text) || *count >= FB_BUBBLE_META_PARTS) {
        return;
    }
    parts[*count].text = text;
    parts[*count].icon = MESH_UI_ICON_NONE;
    parts[*count].cells = mesh_ui_text_cells(text);
    *count += 1U;
}

static void fb_bubble_part_icon(struct fb_bubble_part *parts, size_t *count,
                                enum mesh_ui_icon icon) {
    if (!mesh_ui_icon_is_valid(icon) || *count >= FB_BUBBLE_META_PARTS) {
        return;
    }
    parts[*count].text = NULL;
    parts[*count].icon = icon;
    parts[*count].cells = FB_BUBBLE_ICON_CELLS;
    *count += 1U;
}

/* The run's width: every part, plus a cell of air between each neighbouring pair. */
static size_t fb_bubble_run_cells(const struct fb_bubble_part *parts, size_t count) {
    if (count == 0U) {
        return 0U;
    }
    size_t cells = (count - 1U) * FB_BUBBLE_META_GAP;
    for (size_t i = 0; i < count; ++i) {
        cells += parts[i].cells;
    }
    return cells;
}

/*
 * Assemble the trailing run and cut it down to what the bubble can hold.
 *
 * One function because the measure and the draw both need it, and a run assembled one way and
 * painted another is how a transcript comes to overlap itself - or, as it did while the run was
 * a string the screen concatenated, to paint outside the bubble entirely.
 *
 * Parts go in the order they are drawn and come off the *front*, so what is lost first is the
 * reaction chip and what survives longest is the mark saying the message failed. Dropping
 * rather than truncating, because half a clock is not a shorter clock. It bottoms out at
 * nothing, which is the honest answer for a bubble too narrow to say anything in the corner.
 */
static size_t fb_bubble_run(const struct fb_bubble_meta *meta, size_t budget,
                            struct fb_bubble_part *parts, size_t *count) {
    *count = 0U;
    fb_bubble_part_text(parts, count, meta->reactions);
    fb_bubble_part_text(parts, count, meta->relay);
    fb_bubble_part_icon(parts, count, meta->lock);
    fb_bubble_part_text(parts, count, meta->clock);
    fb_bubble_part_icon(parts, count, meta->state);

    size_t cells = fb_bubble_run_cells(parts, *count);
    while (*count > 0U && cells > budget) {
        for (size_t i = 1U; i < *count; ++i) {
            parts[i - 1U] = parts[i];
        }
        *count -= 1U;
        cells = fb_bubble_run_cells(parts, *count);
    }
    return cells;
}

/* The widest and the last of the lines `text` wraps to at `max`, and how many there are. */
static uint32_t fb_bubble_wrap(const char *text, size_t max, size_t *widest, size_t *last) {
    uint32_t lines = 0U;
    struct mesh_ui_wrap wrap;
    mesh_ui_wrap_begin(&wrap, text, max);
    while (mesh_ui_wrap_next(&wrap)) {
        *last = mesh_ui_text_cells(wrap.line);
        if (*last > *widest) {
            *widest = *last;
        }
        lines += 1U;
    }
    return lines;
}

static struct fb_bubble_metrics fb_bubble_measure(const struct mesh_ui_backend_fb_state *state,
                                                  const struct fb_layout *layout,
                                                  const struct fb_bubble *bubble) {
    const size_t max = fb_bubble_max_cols(state, layout);
    struct fb_bubble_metrics metrics;
    memset(&metrics, 0, sizeof metrics);

    /* One wrap walk per block, and the draw makes the same ones. Anything that measured the
       text a second way - a strlen, a second wrapper - is how a bubble comes to paint over the
       one below it. */
    size_t widest = 0U;
    metrics.lines = fb_bubble_wrap(bubble->text, max, &widest, &metrics.last);
    if (metrics.lines == 0U) {
        metrics.lines = 1U; /* an empty message is still a bubble, just an empty one */
    }
    /* The reason a failed message failed, under it - so the run tucks onto *its* last line,
       which is the last line the bubble draws. */
    metrics.notes = fb_bubble_wrap(bubble->note, max, &widest, &metrics.last);

    metrics.cols = widest;
    if (fb_bubble_has(bubble->name)) {
        const size_t name_cols = mesh_ui_text_cells(bubble->name);
        if (name_cols > metrics.cols) {
            metrics.cols = name_cols;
        }
    }
    /* The quote is one line whatever it says, so it is elided here rather than wrapped - and
       the width it asks for is what is left of it, never what it started as. */
    if (fb_bubble_has(bubble->quote) && max > FB_BUBBLE_QUOTE_INDENT) {
        const size_t room = max - FB_BUBBLE_QUOTE_INDENT;
        metrics.quote_cols = mesh_ui_text_cells(bubble->quote);
        if (metrics.quote_cols > room) {
            metrics.quote_cols = room;
        }
        if (metrics.quote_cols + FB_BUBBLE_QUOTE_INDENT > metrics.cols) {
            metrics.cols = metrics.quote_cols + FB_BUBBLE_QUOTE_INDENT;
        }
    }

    /*
     * The run rides the last line when there is room for it there, which is what keeps a
     * three-word message three words tall instead of doubling it.
     *
     * `max` is the budget either way, so the run can never be wider than the bubble - and
     * because the bubble is then widened to hold it, the draw's right-aligned run cannot reach
     * past the left padding. That is the invariant the whole component turns on.
     */
    metrics.meta_cols = fb_bubble_run(&bubble->meta, max, metrics.parts, &metrics.part_count);
    if (metrics.meta_cols > 0U) {
        const size_t tucked = metrics.last + FB_BUBBLE_META_GAP + metrics.meta_cols;
        if (tucked <= max) {
            if (tucked > metrics.cols) {
                metrics.cols = tucked;
            }
        } else {
            metrics.meta_own_line = true;
            if (metrics.meta_cols > metrics.cols) {
                metrics.cols = metrics.meta_cols;
            }
        }
    }

    if (metrics.cols > max) {
        metrics.cols = max;
    }
    if (metrics.cols == 0U) {
        metrics.cols = 1U;
    }

    metrics.rows = metrics.lines + metrics.notes + (fb_bubble_has(bubble->name) ? 1U : 0U) +
                   (metrics.quote_cols > 0U ? 1U : 0U) + (metrics.meta_own_line ? 1U : 0U) +
                   (fb_bubble_has(bubble->separator) ? 1U : 0U);
    return metrics;
}

uint32_t fb_bubble_rows(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout, const struct fb_bubble *bubble) {
    return fb_bubble_measure(state, layout, bubble).rows;
}

void fb_draw_separator(const struct mesh_ui_backend_fb_state *state, int y, const char *label,
                       enum mesh_ui_tone tone) {
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
    fb_draw_text(state, x, y, label, state->scale, fb_tone_color(state, tone),
                 fb_color(state, MESH_UI_COLOR_BG));
}

/* Paints one block of wrapped text from `y` down, and reports where the next row starts. The
   walk the measure made, so the rows painted are the rows reserved. */
static int fb_bubble_draw_wrapped(const struct mesh_ui_backend_fb_state *state, int x, int y,
                                  const char *text, size_t max, int line_h, struct mesh_ui_rgb ink,
                                  struct mesh_ui_rgb fill) {
    struct mesh_ui_wrap wrap;
    mesh_ui_wrap_begin(&wrap, text, max);
    while (mesh_ui_wrap_next(&wrap)) {
        fb_draw_text(state, x, y, wrap.line, state->scale, ink, fill);
        y += line_h;
    }
    return y;
}

void fb_draw_bubble(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                    int y, const struct fb_bubble *bubble) {
    const struct fb_bubble_metrics metrics = fb_bubble_measure(state, layout, bubble);
    const int adv = fb_char_adv(state, state->scale);
    const int scale = state->scale;
    const size_t max = fb_bubble_max_cols(state, layout);

    if (fb_bubble_has(bubble->separator)) {
        fb_draw_separator(state, y, bubble->separator, bubble->separator_tone);
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
    const struct mesh_ui_rgb quiet = fb_bubble_quiet(state, bubble, paint);

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
        struct mesh_ui_rgb name_color = quiet;
        if (!bubble->failed) {
            if (bubble->alert) {
                name_color = fb_tone_color(state, MESH_UI_TONE_ERROR);
            } else if (!bubble->outbound) {
                name_color = fb_tone_color(state, MESH_UI_TONE_PRIMARY);
            }
        }
        fb_draw_text(state, text_x, y, mesh_ui_line_text(&line), scale, name_color, fill);
        y += layout->line;
    }

    /*
     * The quote, marked the way every messenger marks one: a bar down its left edge and the
     * words beside it in the quiet ink. The bar rather than a glyph because there is no arrow
     * in either face here worth the cell, and because a rule is what the eye already reads as
     * "this is being cited" - the same job the accent edge does on a list row.
     */
    if (metrics.quote_cols > 0U) {
        const int bar_w = scale;
        fb_fill_rect(state, text_x, y, bar_w, layout->line - scale,
                     fb_tone_color(state, MESH_UI_TONE_DIM));
        struct mesh_ui_line quote;
        mesh_ui_line_reset(&quote);
        mesh_ui_line_printf(&quote, "%s", bubble->quote);
        mesh_ui_line_fit(&quote, metrics.quote_cols);
        fb_draw_text(state, text_x + (int)FB_BUBBLE_QUOTE_INDENT * adv, y,
                     mesh_ui_line_text(&quote), scale, quiet, fill);
        y += layout->line;
    }

    int last_y = y;
    y = fb_bubble_draw_wrapped(state, text_x, y, bubble->text, max, layout->line, body, fill);
    if (y == last_y) {
        y += layout->line; /* the measure reserves a row for an empty message; spend it */
    }
    /* The reason under it, in the same ink: the bubble is already the error container, so a
       second red here would be the fill saying the same thing twice. */
    if (metrics.notes > 0U) {
        y = fb_bubble_draw_wrapped(state, text_x, y, bubble->note, max, layout->line, body, fill);
    }
    last_y = y - layout->line;

    if (metrics.part_count == 0U) {
        return;
    }
    /* Right-aligned against the padding, which the measure widened the bubble to leave room
       for - so this can never reach back past `text_x`. */
    int meta_x = box_x + box_w - pad - (int)metrics.meta_cols * adv;
    int meta_y = metrics.meta_own_line ? y : last_y;
    if (!metrics.meta_own_line) {
        /* Tucked against the right edge of the line it shares, which is where every messenger
           puts it - and which is why the measure widened the bubble to make room. */
        const int floor_x = text_x + (int)(metrics.last + FB_BUBBLE_META_GAP) * adv;
        if (meta_x < floor_x) {
            meta_x = floor_x;
        }
    }
    for (size_t i = 0; i < metrics.part_count; ++i) {
        const struct fb_bubble_part *part = &metrics.parts[i];
        if (part->text != NULL) {
            fb_draw_text(state, meta_x, meta_y, part->text, scale, quiet, fill);
        } else {
            fb_draw_icon(state, meta_x, meta_y, part->icon, scale, quiet, fill);
        }
        meta_x += (int)(part->cells + FB_BUBBLE_META_GAP) * adv;
    }
}
