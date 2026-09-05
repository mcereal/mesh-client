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
    fb_draw_text(state, FB_MARGIN, layout->body_y, text, state->scale, k_fb_dim);
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

void fb_list_sub_row(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                     const char *text, enum fb_tone tone) {
    fb_draw_text(state, FB_MARGIN, list->y, text, state->scale, fb_tone_color(tone));
    list->y += list->line;
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
