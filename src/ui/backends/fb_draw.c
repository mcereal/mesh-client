#define _POSIX_C_SOURCE 200809L

/*
 * Pixels, glyphs and the page geometry.
 *
 * Everything above this file measures in cells rather than bytes: a name is four *characters*
 * wide whether it is "Andy" or one emoji, so fb_cols()/fb_fit()/fb_width() are the only
 * legitimate way to ask how much fits. A strlen() or a "%-12s" up in fb_screens.c is a bug.
 */

#include "fb_internal.h"

#include "mesh/ui/emoji.h"
#include "mesh/ui/font5x7.h"
#include "mesh/ui/input.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Palette. Dark ground, cool greys for chrome, one warm colour for things that need the eye. */
const struct fb_rgb k_fb_bg = {0x0A, 0x14, 0x1E};
const struct fb_rgb k_fb_text = {220, 230, 240};
const struct fb_rgb k_fb_dim = {140, 150, 165};
const struct fb_rgb k_fb_tab_active_bg = {60, 110, 170};
const struct fb_rgb k_fb_cursor_bg = {40, 80, 120};
const struct fb_rgb k_fb_white = {255, 255, 255};
const struct fb_rgb k_fb_inbound = {235, 245, 255};
const struct fb_rgb k_fb_outbound = {170, 190, 210};
const struct fb_rgb k_fb_accent = {255, 220, 120};
const struct fb_rgb k_fb_good = {120, 220, 150};
const struct fb_rgb k_fb_bad = {240, 120, 120};

/* Scale an 8-bit channel into a framebuffer bitfield and shift it into place. */
static inline uint32_t fb_pack_channel(uint8_t value, const struct fb_bitfield *field) {
    if (field->length == 0U) {
        return 0U;
    }
    uint32_t scaled = field->length >= 8U ? (uint32_t)value << (field->length - 8U)
                                          : (uint32_t)value >> (8U - field->length);
    return scaled << field->offset;
}

/*
 * The Brick's display engine composites fb0 with per-pixel alpha (the layer dump in
 * /sys/class/disp/disp/attr/sys says `a[pixel 255]`), so a 32-bit pixel with a zero top byte is
 * fully transparent and shows the black background no matter what RGB it carries. That was the
 * black screen. Alpha is therefore always written as opaque, whether or not the driver reports a
 * transp bitfield: for 32 bpp every bit outside the colour channels is set.
 */
static inline uint32_t compose_color(const struct mesh_ui_backend_fb_state *state, uint8_t r,
                                     uint8_t g, uint8_t b) {
    const struct fb_var_screeninfo *var = &state->var;
    const bool has_fields =
        var->red.length != 0U || var->green.length != 0U || var->blue.length != 0U;

    uint32_t color;
    uint32_t color_mask;
    if (has_fields) {
        color = fb_pack_channel(r, &var->red) | fb_pack_channel(g, &var->green) |
                fb_pack_channel(b, &var->blue);
        color_mask = fb_pack_channel(0xFFU, &var->red) | fb_pack_channel(0xFFU, &var->green) |
                     fb_pack_channel(0xFFU, &var->blue);
    } else {
        switch (var->bits_per_pixel) {
        case 32:
        case 24:
            color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            color_mask = 0x00FFFFFFU;
            break;
        case 16:
            color = (uint32_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            color_mask = 0xFFFFU;
            break;
        default:
            return 0U;
        }
    }

    if (var->transp.length != 0U) {
        color |= fb_pack_channel(0xFFU, &var->transp);
    } else if (var->bits_per_pixel == 32U) {
        color |= ~color_mask; /* opaque in whatever byte the colour channels leave free */
    }
    return color;
}

static void fb_draw_pixel(const struct mesh_ui_backend_fb_state *state, int x, int y, uint8_t r,
                          uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= (int)state->var.xres || y >= (int)state->var.yres) {
        return;
    }

    uint32_t color = compose_color(state, r, g, b);
    size_t offset = (size_t)y * state->fix.line_length + (size_t)x * state->bytes_per_pixel;
    if (offset + state->bytes_per_pixel > state->fb_size) {
        return;
    }
    uint8_t *row = state->fb_ptr + offset;

    switch (state->bytes_per_pixel) {
    case 4:
        *(uint32_t *)row = color;
        break;
    case 3:
        row[0] = color & 0xFF;
        row[1] = (color >> 8) & 0xFF;
        row[2] = (color >> 16) & 0xFF;
        break;
    case 2:
        *(uint16_t *)row = (uint16_t)color;
        break;
    default:
        row[0] = color & 0xFF;
        break;
    }
}

/* Glyph metrics for a given multiplier: one pixel column of gap per scale step, two rows. */
int fb_char_adv(int scale) { return MESH_FONT_WIDTH * scale + scale; }
int fb_line_adv(int scale) { return MESH_FONT_HEIGHT * scale + 2 * scale; }

/* One scaled pixel of a glyph. */
static void fb_draw_block(const struct mesh_ui_backend_fb_state *state, int x, int y, int scale,
                          struct fb_rgb color) {
    for (int sx = 0; sx < scale; ++sx) {
        for (int sy = 0; sy < scale; ++sy) {
            fb_draw_pixel(state, x + sx, y + sy, color.r, color.g, color.b);
        }
    }
}

void fb_draw_glyph(const struct mesh_ui_backend_fb_state *state, int x, int y, uint32_t codepoint,
                   int scale, struct fb_rgb color) {
    struct mesh_font_glyph glyph;
    (void)mesh_font5x7_glyph(codepoint, &glyph);

    for (int col = 0; col < MESH_FONT_WIDTH; ++col) {
        for (int row = 0; row < MESH_FONT_HEIGHT; ++row) {
            if (glyph.columns[col] & (1U << row)) {
                fb_draw_block(state, x + col * scale, y + row * scale, scale, color);
            }
        }
        /* An accent that would not fit in the cell hangs in the gap above the line. */
        if (glyph.above[col] & 0x01U) {
            fb_draw_block(state, x + col * scale, y - scale, scale, color);
        }
    }
}

/*
 * Draw one emoji sprite into the cell.
 *
 * The sprite is square and the cell is five by seven, so it is drawn at the cell's width and
 * centred vertically - one font row of padding above and below, which puts it on the same
 * optical line as the capitals beside it. Sampling is nearest-neighbour from the stored 16x16:
 * the cell is 15 px at the tab scale and 20 px at the body scale, so this is a small upscale
 * of pixel art, and anything smoother would need to blend against a background this function
 * cannot see (rows under the cursor are filled a different colour).
 *
 * Emoji ignore `color`. They carry their own, which is the point of having them: the red of a
 * flag and the yellow of a lightning bolt are most of what makes one recognisable at 20 px.
 */
static void fb_draw_emoji(const struct mesh_ui_backend_fb_state *state, int x, int y,
                          uint16_t sprite, int scale) {
    uint8_t pixels[MESH_EMOJI_SIZE * MESH_EMOJI_SIZE];
    mesh_emoji_decode(sprite, pixels);

    /* The box is the full character advance rather than the glyph's five columns: at the
       advance an emoji stands as tall as the capitals beside it, and the sprites carry their
       own transparent margin, so neighbours still separate. */
    const int box = fb_char_adv(scale);
    const int top = y + (MESH_FONT_HEIGHT * scale - box) / 2;

    for (int dy = 0; dy < box; ++dy) {
        const int sy = dy * MESH_EMOJI_SIZE / box;
        for (int dx = 0; dx < box; ++dx) {
            const int sx = dx * MESH_EMOJI_SIZE / box;
            uint8_t rgb[3];
            if (mesh_emoji_color(pixels[sy * MESH_EMOJI_SIZE + sx], rgb)) {
                fb_draw_pixel(state, x + dx, top + dy, rgb[0], rgb[1], rgb[2]);
            }
        }
    }
}

/*
 * Draw `text` as UTF-8, one cell per character - or per emoji, which may be several
 * codepoints.
 *
 * Walking cells rather than bytes is the whole point: a node named with a single emoji used to
 * draw as four question marks, because every byte of the sequence fell through the font's
 * ASCII range separately. Everything below measures with the same walker, so a line is always
 * as wide as it draws.
 */
void fb_draw_text(const struct mesh_ui_backend_fb_state *state, int x, int y, const char *text,
                  int scale, struct fb_rgb color) {
    int cursor = x;
    size_t offset = 0;
    for (;;) {
        const struct mesh_ui_text_cell cell = mesh_ui_text_cell_next(&text[offset]);
        if (cell.bytes == 0U) {
            break;
        }
        offset += cell.bytes;

        if (cell.is_emoji) {
            fb_draw_emoji(state, cursor, y, cell.sprite, scale);
        } else if (cell.codepoint == (uint32_t)'\n') {
            y += fb_line_adv(scale);
            cursor = x;
            continue;
        } else {
            fb_draw_glyph(state, cursor, y, cell.codepoint, scale, color);
        }
        cursor += fb_char_adv(scale);
    }
}

void fb_fill_rect(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int h,
                  struct fb_rgb color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > (int)state->var.xres) {
        w = (int)state->var.xres - x;
    }
    if (y + h > (int)state->var.yres) {
        h = (int)state->var.yres - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    const uint32_t packed = compose_color(state, color.r, color.g, color.b);
    for (int row = y; row < y + h; ++row) {
        uint8_t *px = state->fb_ptr + (size_t)row * state->fix.line_length +
                      (size_t)x * state->bytes_per_pixel;
        if ((size_t)(px - state->fb_ptr) + (size_t)w * state->bytes_per_pixel > state->fb_size) {
            return;
        }
        for (int col = 0; col < w; ++col) {
            switch (state->bytes_per_pixel) {
            case 4:
                *(uint32_t *)px = packed;
                px += 4;
                continue;
            case 3:
                px[0] = packed & 0xFF;
                px[1] = (packed >> 8) & 0xFF;
                px[2] = (packed >> 16) & 0xFF;
                px += 3;
                continue;
            case 2:
                *(uint16_t *)px = (uint16_t)packed;
                px += 2;
                continue;
            default:
                *px++ = packed & 0xFF;
                continue;
            }
        }
    }
}

void fb_clear(const struct mesh_ui_backend_fb_state *state, struct fb_rgb color) {
    fb_fill_rect(state, 0, 0, (int)state->var.xres, (int)state->var.yres, color);
}

/* Columns of text that fit between the margins at this scale. */
size_t fb_cols(const struct mesh_ui_backend_fb_state *state, int scale) {
    const int usable = (int)state->var.xres - 2 * FB_MARGIN;
    if (usable <= 0) {
        return 1U;
    }
    return (size_t)(usable / fb_char_adv(scale));
}

/* Clip a line to `cols` columns. Counted in drawn cells, not bytes, so a character is never
   cut in half - half a sequence would draw as the replacement box and, on the paths that also
   log or serialise the line, would be malformed UTF-8 - and a flag or a ZWJ sequence is never
   split into the pieces it is spelled with. */
void fb_fit(char *line, size_t cols) { mesh_ui_text_cell_truncate(line, cols); }

/* Columns a line occupies once drawn. */
size_t fb_width(const char *line) { return mesh_ui_text_cells(line); }

/* Draw one list row, highlighting it when it is the cursor. `x` is the text origin; the
   highlight spans the full width so the eye finds it without reading. */
void fb_draw_row(const struct mesh_ui_backend_fb_state *state, int y, const char *text,
                 struct fb_rgb color, bool selected) {
    const int line = fb_line_adv(state->scale);
    if (selected) {
        fb_fill_rect(state, FB_MARGIN / 2, y - state->scale, (int)state->var.xres - FB_MARGIN, line,
                     k_fb_cursor_bg);
        color = k_fb_white;
    }
    fb_draw_text(state, FB_MARGIN, y, text, state->scale, color);
}

/* "3m", "2h", "5d" since a radio-reported epoch; "?" when either clock is unusable. */
void fb_format_age(uint32_t last_heard, char *out, size_t out_len) {
    if (last_heard == 0U) {
        snprintf(out, out_len, "%s", "?");
        return;
    }
    const time_t now = time(NULL);
    if (now <= 0 || (uint32_t)now < last_heard) {
        snprintf(out, out_len, "%s", "now");
        return;
    }
    const uint32_t delta = (uint32_t)now - last_heard;
    if (delta < 60U) {
        snprintf(out, out_len, "%us", delta);
    } else if (delta < 3600U) {
        snprintf(out, out_len, "%um", delta / 60U);
    } else if (delta < 86400U) {
        snprintf(out, out_len, "%uh", delta / 3600U);
    } else {
        snprintf(out, out_len, "%ud", delta / 86400U);
    }
}

void fb_format_clock(uint32_t rx_time, char *out, size_t out_len) {
    if (rx_time == 0U) {
        out[0] = '\0';
        return;
    }
    time_t t = (time_t)rx_time;
    struct tm tm_buf;
    if (localtime_r(&t, &tm_buf) == NULL) {
        out[0] = '\0';
        return;
    }
    strftime(out, out_len, "%H:%M", &tm_buf);
}

/* Where the scrolling window starts so the cursor row is on screen. Stateless on purpose:
   the backend keeps nothing between frames, so this is derived from the snapshot alone. */
uint32_t fb_first_visible(uint32_t cursor, uint32_t count, uint32_t visible) {
    if (visible == 0U || count <= visible) {
        return 0U;
    }
    if (cursor + 1U > visible) {
        uint32_t first = cursor + 1U - visible;
        if (first + visible > count) {
            first = count - visible;
        }
        return first;
    }
    return 0U;
}

void fb_draw_tabs(const struct mesh_ui_backend_fb_state *state,
                  const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const int small = layout->small;
    const int adv = fb_char_adv(small);
    const int line = fb_line_adv(small);
    const int y = FB_MARGIN / 2 + small;
    int x = FB_MARGIN / 2;

    for (int i = 0; i < MESH_UI_SCREEN_COUNT; ++i) {
        const enum mesh_ui_screen screen = (enum mesh_ui_screen)i;
        const char *name = mesh_ui_screen_name(screen);
        const int width = (int)fb_width(name) * adv + 2 * small;
        const bool active = (snapshot->nav.screen == screen);
        if (active) {
            fb_fill_rect(state, x, y - small, width, line, k_fb_tab_active_bg);
        }
        fb_draw_text(state, x + small, y, name, small, active ? k_fb_white : k_fb_dim);
        x += width + adv;
    }

    /* Rule under the tab strip. */
    fb_fill_rect(state, 0, y + line, (int)state->var.xres, small / 2 > 0 ? small / 2 : 1,
                 k_fb_tab_active_bg);
    layout->body_y = y + line + 2 * small + FB_MARGIN / 2;
}

void fb_draw_footer(const struct mesh_ui_backend_fb_state *state,
                    const struct mesh_ui_snapshot *snapshot, const struct fb_layout *layout,
                    const char *hint) {
    const int small = layout->small;
    const int line = fb_line_adv(small);
    const size_t cols = fb_cols(state, small);
    char text[160];

    /* Line 1: what the buttons do here. */
    snprintf(text, sizeof text, "%s", hint);
    fb_fit(text, cols);
    fb_draw_text(state, FB_MARGIN, layout->footer_y, text, small, k_fb_dim);

    /* Line 2: a toast when there is one, else the link summary. */
    const struct mesh_ui_nav *nav = &snapshot->nav;
    struct fb_rgb color = k_fb_dim;
    if (nav->toast[0] != '\0') {
        snprintf(text, sizeof text, "%s", nav->toast);
        color = k_fb_accent;
    } else {
        const char *status =
            snapshot->transport_status[0] != '\0' ? snapshot->transport_status : "starting";
        const char *connected = NULL;
        for (size_t i = 0; i < snapshot->device_count; ++i) {
            if (snapshot->devices[i].connected) {
                connected = snapshot->devices[i].name[0] != '\0' ? snapshot->devices[i].name
                                                                 : snapshot->devices[i].identifier;
                break;
            }
        }
        if (connected != NULL) {
            snprintf(text, sizeof text, "%s: %s", status, connected);
            color = k_fb_good;
        } else {
            snprintf(text, sizeof text, "%s | %s", status, mesh_ui_input_quit_hint());
        }
    }
    fb_fit(text, cols);
    fb_draw_text(state, FB_MARGIN, layout->footer_y + line, text, small, color);
}

void fb_draw_title(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                   const char *title) {
    char text[128];
    snprintf(text, sizeof text, "%s", title);
    fb_fit(text, layout->cols);
    fb_draw_text(state, FB_MARGIN, layout->body_y, text, state->scale, k_fb_accent);
    layout->body_y += layout->line + state->scale;
    if (layout->rows > 1U) {
        layout->rows -= 1U;
    }
}

void fb_draw_empty(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout,
                   const char *text) {
    fb_draw_text(state, FB_MARGIN, layout->body_y, text, state->scale, k_fb_dim);
}

/* Wraps `text` into at most `max_lines` lines of `cols` columns, drawing each. */
int fb_draw_wrapped(const struct mesh_ui_backend_fb_state *state, int y, const char *text,
                    size_t cols, int max_lines, struct fb_rgb color) {
    int lines = 0;
    const char *cursor = text;
    char line[160];
    if (cols >= sizeof line) {
        cols = sizeof line - 1U;
    }
    while (*cursor != '\0' && lines < max_lines) {
        /* Byte length of the next `cols` cells, which is what memcpy below wants. */
        size_t take = mesh_ui_text_cell_offset(cursor, cols);
        if (cursor[take] != '\0') {
            /* Break at the last space inside the window when there is one. A space byte can
               never appear inside a multi-byte sequence, so scanning bytes is safe here. */
            for (size_t i = take; i > take / 2; --i) {
                if (cursor[i] == ' ') {
                    take = i;
                    break;
                }
            }
        }
        memcpy(line, cursor, take);
        line[take] = '\0';
        fb_draw_text(state, FB_MARGIN, y, line, state->scale, color);
        y += fb_line_adv(state->scale);
        lines++;
        cursor += take;
        while (*cursor == ' ') {
            ++cursor;
        }
    }
    return lines;
}

/* The conversation and picker helpers take a store; a snapshot carries the same data, so hand
   them a view of it rather than duplicating the walk. */
