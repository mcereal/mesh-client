#define _POSIX_C_SOURCE 200809L

/*
 * The components. Everything here is built from fb_draw.c's primitives and knows nothing about
 * the snapshot - a widget takes what it draws, not where it came from, which is what lets the
 * same list component carry devices, nodes, messages and settings rows.
 */

#include "fb_widgets.h"

#include "mesh/ui/emoji.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void fb_draw_button(const struct mesh_ui_backend_fb_state *state, const struct fb_button *button) {
    if (button->selected) {
        fb_fill_rect(state, button->rect.x, button->rect.y, button->rect.w, button->rect.h,
                     fb_color(state, MESH_UI_COLOR_SURFACE_ACTIVE));
    } else if (button->filled) {
        fb_fill_rect(state, button->rect.x, button->rect.y, button->rect.w, button->rect.h,
                     fb_color(state, MESH_UI_COLOR_SURFACE_SEL));
    }

    if (button->label == NULL || button->label[0] == '\0') {
        return;
    }

    /* Centre by cells, so a label holding an emoji sits where it looks centred rather than
       where its byte count says it does. Vertically it is the glyph body that is centred, not
       the line advance: the advance carries the gap that accents hang in, and counting it
       would push every label low in its box. */
    const int text_w = (int)mesh_ui_text_cells(button->label) * fb_char_adv(state, button->scale);
    const int text_h = (int)fb_font(state)->height * button->scale;
    const int x = button->rect.x + (button->rect.w - text_w) / 2;
    const int y = button->rect.y + (button->rect.h - text_h) / 2;
    /* Any fill at all - the cursor's or a resting one - means the label is drawn in the colour
       the theme validates against that fill. Only a button with no fill is free to take its
       idle tone: a resting fill and the text over it are exactly the pair a theme is held to,
       and reading the tone there is how the keyboard's action row came out white on white. */
    const bool on_fill = button->selected || button->filled;
    fb_draw_text(state, x, y, button->label, button->scale,
                 on_fill ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)
                         : fb_tone_color(state, button->idle_tone));
}

int fb_draw_chip(const struct mesh_ui_backend_fb_state *state, int x, int y, const char *label,
                 bool active, int scale) {
    const int adv = fb_char_adv(state, scale);
    const int width = (int)mesh_ui_text_cells(label) * adv + 2 * scale;
    const struct fb_button button = {
        .rect = {.x = x, .y = y - scale, .w = width, .h = fb_line_adv(state, scale)},
        .label = label,
        .selected = active,
        .filled = false,
        .idle_tone = MESH_UI_TONE_DIM,
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
    fb_draw_text(state, fb_margin(state), layout->body_y, mesh_ui_line_text(&line), state->scale,
                 fb_tone_color(state, MESH_UI_TONE_ACCENT));
    layout->body_y += layout->line + state->scale;
    if (layout->rows > 1U) {
        layout->rows -= 1U;
    }
}

void fb_draw_empty(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   const char *text) {
    /* Wrapped rather than drawn flat: these strings say which button to press next, and at a
       large glyph scale a flat one ran off the right edge with the verb on it. */
    (void)fb_draw_wrapped(state, layout->body_y, text, layout->cols, (int)layout->rows,
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

void fb_list_row_line_badge(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                            uint32_t index, struct mesh_ui_line *line, enum mesh_ui_tone tone,
                            const char *badge) {
    const size_t badge_cols = (badge != NULL) ? mesh_ui_text_cells(badge) : 0U;
    if (badge_cols == 0U) {
        fb_list_row_line(state, list, index, line, tone);
        return;
    }

    /* The row is clipped to leave the badge its own space rather than drawn under it, and the
       badge is measured in cells, so a count is a count whatever the row beside it holds. */
    const int adv = fb_char_adv(state, state->scale);
    const size_t room = list->cols > badge_cols + 2U ? list->cols - badge_cols - 2U : 1U;
    mesh_ui_line_fit(line, room);

    const int y = list->y;
    fb_list_row(state, list, index, mesh_ui_line_text(line), tone);

    const int width = (int)badge_cols * adv + adv;
    const int x = (int)state->var.xres - fb_margin(state) - width;
    fb_fill_rect(state, x, y - state->scale, width, fb_line_adv(state, state->scale) - state->scale,
                 fb_color(state, MESH_UI_COLOR_ACCENT));
    /* Whatever the theme says reads on its own accent fill - on the dark theme that is the
       ground colour, because white on that yellow is unreadable at this glyph size. */
    fb_draw_text(state, x + adv / 2, y, badge, state->scale,
                 fb_color(state, MESH_UI_COLOR_ON_ACCENT));
}

void fb_list_sub_row(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                     const char *text, enum mesh_ui_tone tone) {
    fb_draw_text(state, fb_margin(state), list->y, text, state->scale, fb_tone_color(state, tone));
    list->y += list->line;
}

/* ---- the conversation cell ----------------------------------------------------------------- */

/* The disc and its initials. `size` is both its width and its height, so the radius is half
   of it and the shape is a circle. */
static void fb_draw_avatar(const struct mesh_ui_backend_fb_state *state, int x, int y, int size,
                           const char *label, struct mesh_ui_rgb tint) {
    fb_fill_round_rect(state, x, y, size, size, size / 2, tint);
    if (label == NULL || label[0] == '\0') {
        return;
    }
    /* Centred in cells, and vertically on the glyph body rather than the line advance - the
       advance carries the gap accents hang in, and counting it sits the initials low in the
       disc. The same reasoning as fb_draw_button's label. */
    const int text_w = (int)mesh_ui_text_cells(label) * fb_char_adv(state, state->scale);
    const int text_h = (int)fb_font(state)->height * state->scale;
    fb_draw_text(state, x + (size - text_w) / 2, y + (size - text_h) / 2, label, state->scale,
                 fb_color(state, MESH_UI_COLOR_BG));
}

/* Text clipped to `cols` and drawn at `x`. The conversation cell indents past its avatar, so
   it cannot use fb_draw_row's margin-anchored placement. */
static void fb_draw_clipped(const struct mesh_ui_backend_fb_state *state, int x, int y,
                            const char *text, size_t cols, struct mesh_ui_rgb color) {
    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", text);
    mesh_ui_line_fit(&line, cols);
    fb_draw_text(state, x, y, mesh_ui_line_text(&line), state->scale, color);
}

void fb_draw_conversation(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                          uint32_t index, const struct fb_conversation *conversation) {
    const int scale = state->scale;
    const int adv = fb_char_adv(state, scale);
    const int glyph = (int)fb_font(state)->height * scale;
    const int margin = fb_margin(state);
    const bool selected = mesh_ui_list_is_cursor(&list->model, index);

    /*
     * The two rows are one item, so they are set closer together than two items are - the
     * preview sits a scale above where a second list row would put it, and the space that
     * frees becomes the gap between cells. Without that the cell fills every pixel of its two
     * rows, and a list of them reads as one block of text with no way in.
     */
    const int name_y = list->y;
    const int preview_y = list->y + list->line - scale;
    const int top = name_y - scale / 2;
    const int height = 2 * list->line - 2 * scale;

    if (selected) {
        fb_fill_round_rect(state, margin / 2, top, (int)state->var.xres - margin, height, scale,
                           fb_color(state, MESH_UI_COLOR_SURFACE_SEL));
        /* And a bar down the outer edge, for the same reason the selected bubble gets one: a
           fill one step off the ground is not by itself findable on a small panel in sunlight,
           and gives a colour-blind eye nothing at all. */
        fb_fill_rect(state, margin / 2, top, scale, height, fb_color(state, MESH_UI_COLOR_ACCENT));
    }

    /* The disc, and the text column that starts after it. */
    const int avatar_size = height - scale;
    const int text_x = margin + avatar_size + adv / 2;
    const int text_right = (int)state->var.xres - margin;
    const size_t cols = text_right > text_x ? (size_t)((text_right - text_x) / adv) : 1U;
    fb_draw_avatar(state, margin, top + scale / 2, avatar_size, conversation->avatar,
                   conversation->armed    ? fb_color(state, MESH_UI_COLOR_BAD)
                   : conversation->accent ? fb_color(state, MESH_UI_COLOR_ACCENT)
                                          : mesh_ui_theme_avatar(state->theme, conversation->tint));

    /* Under the cursor everything is drawn against that fill instead of against the ground,
       which is a different pair of colours and not a dimmer version of the same one. */
    const struct mesh_ui_rgb quiet = selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL_DIM)
                                              : fb_tone_color(state, MESH_UI_TONE_DIM);
    const struct mesh_ui_rgb name_ink = selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)
                                                 : fb_tone_color(state, conversation->name_tone);
    /* An unread preview is the row's own words at full weight - never the name's tone, which
       says what kind of conversation this is rather than how much of it is new. */
    const struct mesh_ui_rgb loud = selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)
                                             : fb_tone_color(state, MESH_UI_TONE_STRONG);

    /* The name row: the age sits against the right edge, quietly, the way a messenger dates a
       conversation - it is a fact you glance at, not one you read. */
    const size_t age_cells = mesh_ui_text_cells(conversation->age);
    size_t name_cols = cols;
    if (age_cells > 0U && cols > age_cells + 1U) {
        name_cols = cols - age_cells - 1U;
        fb_draw_text(state, text_right - (int)age_cells * adv, name_y, conversation->age, scale,
                     quiet);
    }
    fb_draw_clipped(state, text_x, name_y, conversation->name, name_cols, name_ink);

    /* The preview row, and the unread count as a pill after it. */
    const size_t badge_cells = mesh_ui_text_cells(conversation->badge);
    size_t preview_cols = cols;
    if (badge_cells > 0U) {
        const int pill_w = (int)(badge_cells + 1U) * adv;
        const int pill_h = glyph + scale;
        fb_fill_round_rect(state, text_right - pill_w, preview_y - scale / 2, pill_w, pill_h,
                           pill_h / 2, fb_color(state, MESH_UI_COLOR_ACCENT));
        fb_draw_text(state, text_right - pill_w + adv / 2, preview_y, conversation->badge, scale,
                     fb_color(state, MESH_UI_COLOR_ON_ACCENT));
        preview_cols = cols > badge_cells + 2U ? cols - badge_cells - 2U : 1U;
    }

    if (conversation->armed) {
        /* The armed row says what the next press does, in place of the preview it would take
           away. Nothing else on screen changes, so the warning is on the row it is about. */
        fb_draw_clipped(state, text_x, preview_y, "X again to delete", preview_cols,
                        selected ? fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL)
                                 : fb_tone_color(state, MESH_UI_TONE_BAD));
    } else {
        struct mesh_ui_line line;
        mesh_ui_line_reset(&line);
        if (conversation->preview[0] != '\0') {
            /* "> " says the last word was ours, which is what tells you whether a quiet thread
               is waiting on you or on them. */
            mesh_ui_line_printf(&line, "%s%s", conversation->preview_outbound ? "> " : "",
                                conversation->preview);
        } else {
            mesh_ui_line_printf(&line, "%s", "no messages yet");
        }
        /* Full weight when there is something unread and quiet when there is not, which is
           the other half of what the badge says - and the half that still reads once the
           badge has been marked away. */
        mesh_ui_line_fit(&line, preview_cols);
        fb_draw_text(state, text_x, preview_y, mesh_ui_line_text(&line), scale,
                     conversation->unread ? loud : quiet);
    }

    /*
     * A hairline in the gap the tightened leading opened up, inset to where the text starts
     * rather than run edge to edge: the avatar column already separates the cells, and a
     * full-width rule under a disc reads as a box drawn around it.
     *
     * Not under the cursor, whose fill is doing that job, and not under the last cell on
     * screen - a rule separates two things, and below the last one there is nothing to
     * separate it from.
     */
    const bool last = (index + 1U >= list->model.count) ||
                      (index + 1U >= list->model.first + list->model.visible);
    if (!selected && !last) {
        fb_draw_rule(state, text_x, top + height + scale / 2, text_right - text_x, scale,
                     MESH_UI_COLOR_RULE);
    }

    list->y += 2 * list->line;
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
    fb_fill_rect(state, box_x, y - scale, box_w, box_h, fill);

    /* The cursor also gets a bar down its outer edge: on a small panel a fill one step lighter
       is not by itself enough to find, and a colour-blind eye gets nothing from it at all. */
    if (bubble->selected) {
        const int bar_x = bubble->outbound ? box_x + box_w - scale : box_x;
        fb_fill_rect(state, bar_x, y - scale, scale, box_h, fb_color(state, MESH_UI_COLOR_ACCENT));
    }

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

void fb_list_field_row(const struct mesh_ui_backend_fb_state *state, struct fb_list *list,
                       uint32_t index, const char *label, size_t label_cols, const char *marker,
                       const char *value, enum mesh_ui_tone tone) {
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
                        const struct fb_layout *layout, int *y, enum mesh_ui_tone tone,
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
    fb_draw_text(state, fb_margin(state), *y, mesh_ui_line_text(&line), state->scale,
                 fb_tone_color(state, tone));
    *y += layout->line;
}
