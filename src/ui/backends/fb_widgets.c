#define _POSIX_C_SOURCE 200809L

/*
 * The components. Everything here is built from fb_draw.c's primitives and knows nothing about
 * the snapshot - a widget takes what it draws, not where it came from, which is what lets the
 * same list component carry devices, nodes, messages and settings rows.
 */

#include "fb_widgets.h"

#include "mesh/ui/emoji.h"
#include "mesh/ui/font5x7.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct fb_rgb fb_tone_color(enum fb_tone tone) {
    switch (tone) {
    case FB_TONE_DIM:
        return k_fb_dim;
    case FB_TONE_STRONG:
        return k_fb_white;
    case FB_TONE_ACCENT:
        return k_fb_accent;
    case FB_TONE_GOOD:
        return k_fb_good;
    case FB_TONE_BAD:
        return k_fb_bad;
    case FB_TONE_INBOUND:
        return k_fb_inbound;
    case FB_TONE_OUTBOUND:
        return k_fb_outbound;
    case FB_TONE_NORMAL:
    default:
        return k_fb_text;
    }
}

void fb_draw_button(const struct mesh_ui_backend_fb_state *state, const struct fb_button *button) {
    if (button->selected) {
        fb_fill_rect(state, button->rect.x, button->rect.y, button->rect.w, button->rect.h,
                     k_fb_tab_active_bg);
    } else if (button->filled) {
        fb_fill_rect(state, button->rect.x, button->rect.y, button->rect.w, button->rect.h,
                     k_fb_cursor_bg);
    }

    if (button->label == NULL || button->label[0] == '\0') {
        return;
    }

    /* Centre by cells, so a label holding an emoji sits where it looks centred rather than
       where its byte count says it does. Vertically it is the glyph body that is centred, not
       the line advance: the advance carries the gap that accents hang in, and counting it
       would push every label low in its box. */
    const int text_w = (int)mesh_ui_text_cells(button->label) * fb_char_adv(button->scale);
    const int text_h = MESH_FONT_HEIGHT * button->scale;
    const int x = button->rect.x + (button->rect.w - text_w) / 2;
    const int y = button->rect.y + (button->rect.h - text_h) / 2;
    fb_draw_text(state, x, y, button->label, button->scale,
                 button->selected ? k_fb_white : fb_tone_color(button->idle_tone));
}

int fb_draw_chip(const struct mesh_ui_backend_fb_state *state, int x, int y, const char *label,
                 bool active, int scale) {
    const int adv = fb_char_adv(scale);
    const int width = (int)mesh_ui_text_cells(label) * adv + 2 * scale;
    const struct fb_button button = {
        .rect = {.x = x, .y = y - scale, .w = width, .h = fb_line_adv(scale)},
        .label = label,
        .selected = active,
        .filled = false,
        .idle_tone = FB_TONE_DIM,
        .scale = scale,
    };
    fb_draw_button(state, &button);
    return x + width + adv;
}

void fb_draw_title(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                   const char *title) {
    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", title);
    mesh_ui_line_fit(&line, layout->cols);
    fb_draw_text(state, FB_MARGIN, layout->body_y, mesh_ui_line_text(&line), state->scale,
                 k_fb_accent);
    layout->body_y += layout->line + state->scale;
    if (layout->rows > 1U) {
        layout->rows -= 1U;
    }
}

void fb_draw_empty(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   const char *text) {
    /* Wrapped rather than drawn flat: these strings say which button to press next, and at a
       large glyph scale a flat one ran off the right edge with the verb on it. */
    (void)fb_draw_wrapped(state, layout->body_y, text, layout->cols, (int)layout->rows, k_fb_dim);
}

void fb_draw_rule(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int scale,
                  struct fb_rgb color) {
    fb_fill_rect(state, x, y, w, scale / 2 > 0 ? scale / 2 : 1, color);
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
                 const char *text, enum fb_tone tone) {
    fb_draw_row(state, list->y, text, fb_tone_color(tone),
                mesh_ui_list_is_cursor(&list->model, index));
    list->y += list->line;
}

void fb_list_row_line(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                      uint32_t index, struct mesh_ui_line *line, enum fb_tone tone) {
    mesh_ui_line_fit(line, list->cols);
    fb_list_row(state, list, index, mesh_ui_line_text(line), tone);
}

void fb_list_row_line_badge(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                            uint32_t index, struct mesh_ui_line *line, enum fb_tone tone,
                            const char *badge) {
    const size_t badge_cols = (badge != NULL) ? mesh_ui_text_cells(badge) : 0U;
    if (badge_cols == 0U) {
        fb_list_row_line(state, list, index, line, tone);
        return;
    }

    /* The row is clipped to leave the badge its own space rather than drawn under it, and the
       badge is measured in cells, so a count is a count whatever the row beside it holds. */
    const int adv = fb_char_adv(state->scale);
    const size_t room = list->cols > badge_cols + 2U ? list->cols - badge_cols - 2U : 1U;
    mesh_ui_line_fit(line, room);

    const int y = list->y;
    fb_list_row(state, list, index, mesh_ui_line_text(line), tone);

    const int width = (int)badge_cols * adv + adv;
    const int x = (int)state->var.xres - FB_MARGIN - width;
    fb_fill_rect(state, x, y - state->scale, width, fb_line_adv(state->scale) - state->scale,
                 k_fb_accent);
    /* Dark text on the accent fill: white on that yellow is unreadable at this glyph size. */
    fb_draw_text(state, x + adv / 2, y, badge, state->scale, k_fb_bg);
}

void fb_list_sub_row(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                     const char *text, enum fb_tone tone) {
    fb_draw_text(state, FB_MARGIN, list->y, text, state->scale, fb_tone_color(tone));
    list->y += list->line;
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
static size_t fb_bubble_max_cols(const struct fb_layout *layout) {
    const size_t max = layout->cols > 4U ? layout->cols * 3U / 4U : layout->cols;
    return max > 0U ? max : 1U;
}

static bool fb_bubble_has(const char *text) { return text != NULL && text[0] != '\0'; }

static struct fb_bubble_metrics fb_bubble_measure(const struct fb_layout *layout,
                                                  const struct fb_bubble *bubble) {
    const size_t max = fb_bubble_max_cols(layout);
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

uint32_t fb_bubble_rows(const struct fb_layout *layout, const struct fb_bubble *bubble) {
    return fb_bubble_measure(layout, bubble).rows;
}

void fb_draw_separator(const struct mesh_ui_backend_fb_state *state, int y, const char *label) {
    const int adv = fb_char_adv(state->scale);
    const int rule_y = y + (MESH_FONT_HEIGHT * state->scale) / 2;
    const int left = FB_MARGIN;
    const int right = (int)state->var.xres - FB_MARGIN;

    if (!fb_bubble_has(label)) {
        fb_draw_rule(state, left, rule_y, right - left, state->scale, k_fb_cursor_bg);
        return;
    }

    /* Centred in cells, so a label with an emoji in it sits where it looks centred. */
    const int width = (int)mesh_ui_text_cells(label) * adv;
    const int x = left + (right - left - width) / 2;
    fb_draw_rule(state, left, rule_y, x - left - adv, state->scale, k_fb_cursor_bg);
    fb_draw_rule(state, x + width + adv, rule_y, right - (x + width + adv), state->scale,
                 k_fb_cursor_bg);
    fb_draw_text(state, x, y, label, state->scale, k_fb_dim);
}

void fb_draw_bubble(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                    int y, const struct fb_bubble *bubble) {
    const struct fb_bubble_metrics metrics = fb_bubble_measure(layout, bubble);
    const int adv = fb_char_adv(state->scale);
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
    const int box_x = bubble->outbound ? (int)state->var.xres - FB_MARGIN - box_w : FB_MARGIN;
    const uint32_t box_rows = metrics.rows - (fb_bubble_has(bubble->separator) ? 1U : 0U);
    const int box_h = (int)box_rows * layout->line - scale;

    struct fb_rgb fill;
    if (bubble->failed) {
        fill = k_fb_bubble_bad;
    } else if (bubble->outbound) {
        fill = bubble->selected ? k_fb_bubble_out_sel : k_fb_bubble_out;
    } else {
        fill = bubble->selected ? k_fb_bubble_in_sel : k_fb_bubble_in;
    }
    fb_fill_rect(state, box_x, y - scale, box_w, box_h, fill);

    /* The cursor also gets a bar down its outer edge: on a small panel a fill one step lighter
       is not by itself enough to find, and a colour-blind eye gets nothing from it at all. */
    if (bubble->selected) {
        const int bar_x = bubble->outbound ? box_x + box_w - scale : box_x;
        fb_fill_rect(state, bar_x, y - scale, scale, box_h, k_fb_accent);
    }

    const int text_x = box_x + pad;
    const struct fb_rgb body = fb_tone_color(bubble->outbound ? FB_TONE_OUTBOUND : FB_TONE_INBOUND);

    if (fb_bubble_has(bubble->name)) {
        struct mesh_ui_line line;
        mesh_ui_line_reset(&line);
        mesh_ui_line_printf(&line, "%s", bubble->name);
        mesh_ui_line_fit(&line, metrics.cols);
        fb_draw_text(state, text_x, y, mesh_ui_line_text(&line), scale,
                     bubble->outbound ? k_fb_dim : k_fb_accent);
        y += layout->line;
    }

    /* The same walk the measure made, so the rows painted are the rows reserved. */
    struct mesh_ui_wrap wrap;
    mesh_ui_wrap_begin(&wrap, bubble->text, fb_bubble_max_cols(layout));
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
    const struct fb_rgb meta_color = bubble->failed ? k_fb_bad : k_fb_dim;
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

size_t fb_field_label_cols(const struct fb_layout *layout, size_t preferred) {
    return layout->cols < 40U ? layout->cols / 2U : preferred;
}

void fb_list_field_row(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                       uint32_t index, const char *label, size_t label_cols, const char *marker,
                       const char *value, enum fb_tone tone) {
    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    /* The label occupies its column exactly - clipped when long, padded when short - measured
       in cells so a value column still lines up under a label that is not all ASCII. */
    mesh_ui_line_column(&line, label, label_cols);
    mesh_ui_line_printf(&line, " %s%s", marker != NULL ? marker : "", value != NULL ? value : "");
    fb_list_row_line(state, list, index, &line, tone);
}

void fb_title_count(char *out, size_t out_len, const char *name, uint32_t count, uint32_t dropped) {
    if (dropped > 0U) {
        snprintf(out, out_len, "%s (%u, +%u older)", name, count, dropped);
    } else {
        snprintf(out, out_len, "%s (%u)", name, count);
    }
}

void fb_draw_status_row(const struct mesh_ui_backend_fb_state *state,
                        const struct fb_layout *layout, int *y, enum fb_tone tone,
                        const char *label, const char *fmt, ...) {
    if (*y + layout->line > layout->footer_y) {
        return;
    }

    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    mesh_ui_line_column(&line, label, 11U);
    mesh_ui_line_printf(&line, " ");

    va_list args;
    va_start(args, fmt);
    mesh_ui_line_vprintf(&line, fmt, args);
    va_end(args);

    mesh_ui_line_fit(&line, layout->cols);
    fb_draw_text(state, FB_MARGIN, *y, mesh_ui_line_text(&line), state->scale, fb_tone_color(tone));
    *y += layout->line;
}
