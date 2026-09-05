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

/*
 * Fill a clipped, axis-aligned span with an already-packed pixel value.
 *
 * This is the one place that touches the mapping. Everything above it packs its colour once
 * and then describes rectangles, because compose_color() is far too much arithmetic to run per
 * pixel: a full screen of text is ~200k scaled sub-pixels, and packing each one separately was
 * about a third of the frame.
 */
static void fb_fill_packed(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int h,
                           uint32_t packed) {
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

    const size_t bpp = state->bytes_per_pixel;
    const size_t stride = state->fix.line_length;
    uint8_t *row = state->fb_ptr + (size_t)y * stride + (size_t)x * bpp;

    /* The pixel format is fixed for the life of the mapping, so the switch belongs out here
       rather than inside the column loop it used to sit in. */
    for (int r = 0; r < h; ++r, row += stride) {
        if ((size_t)(row - state->fb_ptr) + (size_t)w * bpp > state->fb_size) {
            return;
        }
        /* The stores go through memcpy rather than a cast to uint32_t*: the compiler emits the same
           single instruction, but a row pointer is only as aligned as fix.line_length makes it, and
           casting one to a wider type is undefined where the hardware is strict about it. */
        switch (bpp) {
        case 4: {
            uint8_t *px = row;
            for (int col = 0; col < w; ++col, px += 4) {
                memcpy(px, &packed, 4U);
            }
            break;
        }
        case 3: {
            uint8_t *px = row;
            for (int col = 0; col < w; ++col) {
                px[0] = (uint8_t)(packed & 0xFFU);
                px[1] = (uint8_t)((packed >> 8) & 0xFFU);
                px[2] = (uint8_t)((packed >> 16) & 0xFFU);
                px += 3;
            }
            break;
        }
        case 2: {
            const uint16_t narrow = (uint16_t)packed;
            uint8_t *px = row;
            for (int col = 0; col < w; ++col, px += 2) {
                memcpy(px, &narrow, 2U);
            }
            break;
        }
        default:
            memset(row, (int)(packed & 0xFFU), (size_t)w);
            break;
        }
    }
}

/* Glyph metrics for a given multiplier: one pixel column of gap per scale step, two rows. */
int fb_char_adv(int scale) { return MESH_FONT_WIDTH * scale + scale; }
int fb_line_adv(int scale) { return MESH_FONT_HEIGHT * scale + 2 * scale; }

void fb_draw_glyph(const struct mesh_ui_backend_fb_state *state, int x, int y, uint32_t codepoint,
                   int scale, struct fb_rgb color) {
    struct mesh_font_glyph glyph;
    (void)mesh_font5x7_glyph(codepoint, &glyph);

    const uint32_t packed = compose_color(state, color.r, color.g, color.b);

    /*
     * The glyph is stored column-major and the framebuffer is row-major, so walk rows and emit
     * each horizontal run of lit columns as one span. A typical glyph row is one or two runs,
     * where the old per-pixel loop was up to MESH_FONT_WIDTH * scale separate clipped writes.
     */
    for (int row = 0; row < MESH_FONT_HEIGHT; ++row) {
        const uint8_t bit = (uint8_t)(1U << row);
        int col = 0;
        while (col < MESH_FONT_WIDTH) {
            if ((glyph.columns[col] & bit) == 0U) {
                ++col;
                continue;
            }
            int end = col;
            while (end < MESH_FONT_WIDTH && (glyph.columns[end] & bit) != 0U) {
                ++end;
            }
            fb_fill_packed(state, x + col * scale, y + row * scale, (end - col) * scale, scale,
                           packed);
            col = end;
        }
    }

    /* An accent that would not fit in the cell hangs in the gap above the line. */
    int col = 0;
    while (col < MESH_FONT_WIDTH) {
        if ((glyph.above[col] & 0x01U) == 0U) {
            ++col;
            continue;
        }
        int end = col;
        while (end < MESH_FONT_WIDTH && (glyph.above[end] & 0x01U) != 0U) {
            ++end;
        }
        fb_fill_packed(state, x + col * scale, y - scale, (end - col) * scale, scale, packed);
        col = end;
    }
}

/*
 * The emoji palette, packed into framebuffer pixels once.
 *
 * compose_color() depends only on the mapping's pixel format, which never changes while fb0 is
 * open, so the 255 palette entries are packed on first use and reused. The signature guards the
 * case of a second open with a different format - the tests do exactly that.
 */
static uint64_t fb_format_signature(const struct mesh_ui_backend_fb_state *state) {
    const struct fb_var_screeninfo *var = &state->var;
    uint64_t sig = var->bits_per_pixel;
    const struct fb_bitfield *fields[4] = {&var->red, &var->green, &var->blue, &var->transp};
    for (size_t i = 0; i < 4U; ++i) {
        sig = sig * 131U + fields[i]->offset;
        sig = sig * 131U + fields[i]->length;
    }
    return sig;
}

#define FB_EMOJI_PALETTE_SLOTS 256

static const uint32_t *fb_emoji_palette(const struct mesh_ui_backend_fb_state *state,
                                        const bool **opaque_out) {
    static uint32_t packed[FB_EMOJI_PALETTE_SLOTS];
    static bool opaque[FB_EMOJI_PALETTE_SLOTS];
    static uint64_t signature;
    static bool valid;

    const uint64_t sig = fb_format_signature(state);
    if (!valid || sig != signature) {
        for (size_t i = 0; i < FB_EMOJI_PALETTE_SLOTS; ++i) {
            uint8_t rgb[3];
            opaque[i] = mesh_emoji_color((uint8_t)i, rgb);
            packed[i] = opaque[i] ? compose_color(state, rgb[0], rgb[1], rgb[2]) : 0U;
        }
        signature = sig;
        valid = true;
    }
    *opaque_out = opaque;
    return packed;
}

/* Decoding a sprite is a run-length expansion into 256 bytes. A row of identical reactions or a
   repeated node emoji redraws the same sprite many times per frame, so keep the last one. */
static const uint8_t *fb_emoji_pixels(uint16_t sprite) {
    static uint8_t pixels[MESH_EMOJI_SIZE * MESH_EMOJI_SIZE];
    static uint16_t cached_sprite;
    static bool valid;

    if (!valid || cached_sprite != sprite) {
        mesh_emoji_decode(sprite, pixels);
        cached_sprite = sprite;
        valid = true;
    }
    return pixels;
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
    /* The box is the full character advance rather than the glyph's five columns: at the
       advance an emoji stands as tall as the capitals beside it, and the sprites carry their
       own transparent margin, so neighbours still separate. */
    const int box = fb_char_adv(scale);
    const int top = y + (MESH_FONT_HEIGHT * scale - box) / 2;

    /* Nearest-neighbour source column per destination column. Identical for every row, so the
       division runs once per column instead of once per pixel. */
    int sx_map[MESH_FONT_WIDTH * FB_MAX_SCALE + FB_MAX_SCALE];
    if (box <= 0 || box > (int)(sizeof sx_map / sizeof sx_map[0])) {
        return;
    }
    for (int dx = 0; dx < box; ++dx) {
        sx_map[dx] = dx * MESH_EMOJI_SIZE / box;
    }

    const uint8_t *pixels = fb_emoji_pixels(sprite);
    const bool *opaque = NULL;
    const uint32_t *palette = fb_emoji_palette(state, &opaque);

    /* Emoji are mostly flat fills, so coalescing equal-index neighbours into one span turns
       most rows into a handful of writes. */
    for (int dy = 0; dy < box; ++dy) {
        const uint8_t *src_row = &pixels[(dy * MESH_EMOJI_SIZE / box) * MESH_EMOJI_SIZE];
        int dx = 0;
        while (dx < box) {
            const uint8_t index = src_row[sx_map[dx]];
            int end = dx + 1;
            while (end < box && src_row[sx_map[end]] == index) {
                ++end;
            }
            if (opaque[index]) {
                fb_fill_packed(state, x + dx, top + dy, end - dx, 1, palette[index]);
            }
            dx = end;
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
    fb_fill_packed(state, x, y, w, h, compose_color(state, color.r, color.g, color.b));
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
