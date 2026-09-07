#define _POSIX_C_SOURCE 200809L

/*
 * Pixels, glyphs and the page geometry.
 *
 * Everything above this file measures in cells rather than bytes: a name is four *characters*
 * wide whether it is "Andy" or one emoji, so fb_cols()/fb_fit()/fb_width() are the only
 * legitimate way to ask how much fits. A strlen() or a "%-12s" up in fb_screens.c is a bug.
 */

#include "fb_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/emoji.h"
#include "mesh/ui/icon.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- the theme on the state --------------------------------------------------------------- */

/*
 * Every colour, margin and glyph size a frame uses comes through these four.
 *
 * They are trivial on purpose: the point is that there is exactly one path from "what does
 * this mean" to "which pixels", so a theme switch cannot leave a corner of the UI behind.
 */
void fb_state_set_theme(struct mesh_ui_backend_fb_state *state, const struct mesh_ui_theme *theme,
                        int scale) {
    if (state == NULL) {
        return;
    }
    state->theme = theme != NULL ? theme : mesh_ui_theme_default();
    state->scale = mesh_ui_theme_clamp_scale(state->theme, scale);
    /* Every position remembered in there is in pixels, measured against metrics this call has
       just replaced. Keeping them would slide a knob from where it sat under the old scale. */
    mesh_ui_anim_table_reset(&state->anim);
}

void fb_state_set_now(struct mesh_ui_backend_fb_state *state, uint64_t now_ms) {
    if (state != NULL && now_ms > state->now_ms) {
        state->now_ms = now_ms;
    }
}

bool fb_state_animating(const struct mesh_ui_backend_fb_state *state) {
    return state != NULL && mesh_ui_anim_table_active(&state->anim, state->now_ms);
}

bool fb_state_follow_snapshot(struct mesh_ui_backend_fb_state *state,
                              const struct mesh_ui_snapshot *snapshot) {
    if (state == NULL || snapshot == NULL) {
        return false;
    }
    const char *const id = snapshot->settings.client.theme;
    if (id[0] == '\0') {
        /* Nothing published one - the capture harness has no app behind it - so keep drawing
           with whatever this state was opened with. */
        return false;
    }
    const struct mesh_ui_theme *theme = mesh_ui_theme_by_id(id);
    if (theme == NULL || theme == state->theme) {
        return false;
    }
    /* The new theme brings its own glyph scale unless the environment pinned one. */
    fb_state_set_theme(state, theme, state->scale_pinned ? state->scale : 0);
    return true;
}

struct mesh_ui_rgb fb_color(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_color role) {
    return mesh_ui_theme_color(state != NULL ? state->theme : NULL, role);
}

struct mesh_ui_rgb fb_tone_color(const struct mesh_ui_backend_fb_state *state,
                                 enum mesh_ui_tone tone) {
    return mesh_ui_theme_tone(state != NULL ? state->theme : NULL, tone);
}

const struct mesh_ui_metrics *fb_metrics(const struct mesh_ui_backend_fb_state *state) {
    return mesh_ui_theme_metrics(state != NULL ? state->theme : NULL);
}

const struct mesh_ui_font *fb_font(const struct mesh_ui_backend_fb_state *state) {
    return mesh_ui_theme_font(state != NULL ? state->theme : NULL);
}

int fb_radius(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_shape shape) {
    return mesh_ui_theme_radius(state->theme, shape, state->scale);
}

int fb_edge(const struct mesh_ui_backend_fb_state *state) {
    const int edge = state->scale / 2;
    return edge > 0 ? edge : 1;
}

int fb_margin(const struct mesh_ui_backend_fb_state *state) {
    return (int)fb_metrics(state)->margin;
}

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

/* Glyph metrics for a given multiplier. The gaps are the font's, not this file's: a taller
   font with a different line gap changes every measurement above without touching one. */
int fb_char_adv(const struct mesh_ui_backend_fb_state *state, int scale) {
    return mesh_ui_font_advance(fb_font(state), scale);
}
int fb_line_adv(const struct mesh_ui_backend_fb_state *state, int scale) {
    return mesh_ui_font_line(fb_font(state), scale);
}

void fb_draw_glyph(const struct mesh_ui_backend_fb_state *state, int x, int y, uint32_t codepoint,
                   int scale, struct mesh_ui_rgb color) {
    const struct mesh_ui_font *font = fb_font(state);
    struct mesh_ui_glyph glyph;
    (void)mesh_ui_font_glyph(font, codepoint, &glyph);

    const uint32_t packed = compose_color(state, color.r, color.g, color.b);

    /*
     * The glyph is stored column-major and the framebuffer is row-major, so walk rows and emit
     * each horizontal run of lit columns as one span. A typical glyph row is one or two runs,
     * where the old per-pixel loop was up to one clipped write per lit pixel.
     */
    for (int row = 0; row < (int)font->height; ++row) {
        const uint16_t bit = (uint16_t)(1U << row);
        int col = 0;
        while (col < (int)font->width) {
            if ((glyph.columns[col] & bit) == 0U) {
                ++col;
                continue;
            }
            int end = col;
            while (end < (int)font->width && (glyph.columns[end] & bit) != 0U) {
                ++end;
            }
            fb_fill_packed(state, x + col * scale, y + row * scale, (end - col) * scale, scale,
                           packed);
            col = end;
        }
    }

    /* An accent that would not fit in the cell hangs in the gap above the line. */
    int col = 0;
    while (col < (int)font->width) {
        if ((glyph.above[col] & 0x01U) == 0U) {
            ++col;
            continue;
        }
        int end = col;
        while (end < (int)font->width && (glyph.above[end] & 0x01U) != 0U) {
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
    const int box = fb_char_adv(state, scale);
    const int top = y + ((int)fb_font(state)->height * scale - box) / 2;

    /* Nearest-neighbour source column per destination column. Identical for every row, so the
       division runs once per column instead of once per pixel. */
    int sx_map[(MESH_UI_GLYPH_MAX_WIDTH + 1) * MESH_UI_SCALE_MAX];
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

/* An icon occupies exactly one text cell, which is what lets a screen put one in a line's
   leading slot and keep counting the rest of the row in columns. */
int fb_icon_box(const struct mesh_ui_backend_fb_state *state, int scale) {
    return fb_char_adv(state, scale);
}

/*
 * What it is actually drawn at, which is a little wider than the cell it occupies.
 *
 * A symbol has to stand as tall as the capitals beside it to read as their equal, and the cell
 * advance is narrower than the glyph body is tall - so an icon drawn at the advance comes out
 * visibly smaller than the text it is labelling, which is the one thing a Material icon is
 * never allowed to be. It is drawn at the body's height instead and centred on its cell, so the
 * overhang is a couple of pixels into the gaps either side and the column arithmetic above is
 * untouched.
 */
static int fb_icon_drawn(const struct mesh_ui_backend_fb_state *state, int scale) {
    return (int)fb_font(state)->height * scale;
}

/* Coverage steps a blended icon is drawn in. The sprites carry 16 levels and the sampling
   between them is continuous, so this is about how many colours one icon costs to pack:
   32 is below what the eye separates on a 24 px symbol, and packing a colour per pixel was
   the thing fb_fill_packed() exists to avoid. */
#define FB_ICON_BLEND_STEPS 32

/*
 * Coverage at one destination pixel, as a blend step.
 *
 * Bilinear between the four sprite pixels around it: `sample_x` is an 8.8 position along the
 * sprite's row, `fy` the fraction between the two rows the caller has already picked out. The
 * result is the index into the packed-colour table below, which is why the quantisation lives
 * here rather than at the call site - measuring a span and drawing it must round identically or
 * the span boundaries move.
 */
static int fb_icon_step(const uint8_t *row0, const uint8_t *row1, int32_t sample_x, int32_t fy) {
    const int32_t x0 = sample_x >> 8;
    const int32_t x1 = (x0 + 1 < MESH_UI_ICON_SIZE) ? x0 + 1 : x0;
    const int32_t fx = sample_x & 0xFF;
    const int32_t upper = row0[x0] * (256 - fx) + row0[x1] * fx;
    const int32_t lower = row1[x0] * (256 - fx) + row1[x1] * fx;
    const int32_t alpha = upper * (256 - fy) + lower * fy; /* 0 .. MESH_UI_ICON_MAX_ALPHA << 16 */
    return (int)((alpha * (FB_ICON_BLEND_STEPS - 1)) / (MESH_UI_ICON_MAX_ALPHA * 65536));
}

/*
 * Draw one icon sprite into the cell, in `ink` over `ground`.
 *
 * The two colours are the whole difference from fb_draw_emoji(). An emoji carries its own
 * palette; an icon carries coverage only and is *tinted*, so a chevron on a selected row is the
 * selected row's ink and a warning is the bad tone - it is themed like the text it stands
 * beside, because it is doing that text's job.
 *
 * `ground` is what it is blended against, and it has to be passed in for the same reason
 * fb_draw_emoji() does not blend at all: what is already on the panel is not readable from
 * here - an icon sits on the ground on one row and on the cursor fill on the next - and the
 * display engine composites fb0 against its own layer rather than against what we have drawn,
 * so there is no alpha to leave the job to. A caller that has just filled a row knows the
 * colour it filled it with; nothing else does.
 *
 * Sampling is bilinear, unlike the emoji path's nearest neighbour, and that is not a
 * preference: the sprite is 32 px and the cell it lands in is 28 at the body scale and 21 in
 * the chrome, so nearest neighbour would drop every fourth source row - and on the empty
 * screen's symbol, several times that size, would duplicate them instead. On a flat-filled
 * emoji either is invisible; on a 2 px chevron stroke it is the difference between a smooth
 * diagonal and a staircase. The blend is one multiply per channel per step rather than per
 * pixel, because coverage is quantised into FB_ICON_BLEND_STEPS packed colours first.
 */
void fb_draw_icon(const struct mesh_ui_backend_fb_state *state, int x, int y,
                  enum mesh_ui_icon icon, int scale, struct mesh_ui_rgb ink,
                  struct mesh_ui_rgb ground) {
    if (!mesh_ui_icon_is_valid(icon) || scale <= 0 || scale > FB_ICON_SCALE_MAX) {
        return;
    }
    const int box = fb_icon_drawn(state, scale);
    /* Centred on the cell in both directions, so it sits on the same optical line as the
       capitals beside it and in the same column the layout above counted. */
    const int left = x - (box - fb_icon_box(state, scale)) / 2;
    const int top = y + ((int)fb_font(state)->height * scale - box) / 2;

    /* Source column per destination column, as a 8.8 fixed-point position: identical for every
       row, so the division runs once per column instead of once per pixel. Sized for the
       largest icon anything asks for, which is the empty state's - the rest are one text cell. */
    int32_t sx[MESH_UI_GLYPH_MAX_HEIGHT * FB_ICON_SCALE_MAX];
    if (box <= 0 || box > (int)(sizeof sx / sizeof sx[0])) {
        return;
    }
    for (int dx = 0; dx < box; ++dx) {
        /* Half-pixel offsets at both ends: sampling from the pixel's centre is what keeps a
           symmetric symbol symmetric after the scale. */
        const int32_t pos = (((int32_t)dx * 2 + 1) * MESH_UI_ICON_SIZE * 128) / box - 128;
        sx[dx] = pos < 0 ? 0 : pos;
    }

    uint8_t pixels[MESH_UI_ICON_SIZE * MESH_UI_ICON_SIZE];
    mesh_ui_icon_alpha(icon, pixels);

    uint32_t blend[FB_ICON_BLEND_STEPS];
    for (int step = 0; step < FB_ICON_BLEND_STEPS; ++step) {
        const int32_t a = (int32_t)step * 255 / (FB_ICON_BLEND_STEPS - 1);
        const uint8_t r = (uint8_t)(((int32_t)ink.r * a + (int32_t)ground.r * (255 - a)) / 255);
        const uint8_t g = (uint8_t)(((int32_t)ink.g * a + (int32_t)ground.g * (255 - a)) / 255);
        const uint8_t b = (uint8_t)(((int32_t)ink.b * a + (int32_t)ground.b * (255 - a)) / 255);
        blend[step] = compose_color(state, r, g, b);
    }

    for (int dy = 0; dy < box; ++dy) {
        const int32_t pos_y = (((int32_t)dy * 2 + 1) * MESH_UI_ICON_SIZE * 128) / box - 128;
        const int32_t py = pos_y < 0 ? 0 : pos_y;
        const int32_t y0 = py >> 8;
        const int32_t y1 = (y0 + 1 < MESH_UI_ICON_SIZE) ? y0 + 1 : y0;
        const int32_t fy = py & 0xFF;
        const uint8_t *row0 = &pixels[y0 * MESH_UI_ICON_SIZE];
        const uint8_t *row1 = &pixels[y1 * MESH_UI_ICON_SIZE];

        int dx = 0;
        while (dx < box) {
            const int step = fb_icon_step(row0, row1, sx[dx], fy);
            /* Runs of equal coverage - which most of a filled symbol is - become one span, the
               way the emoji path coalesces equal palette indices. */
            int end = dx + 1;
            while (end < box && fb_icon_step(row0, row1, sx[end], fy) == step) {
                ++end;
            }
            if (step > 0) {
                fb_fill_packed(state, left + dx, top + dy, end - dx, 1, blend[step]);
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
                  int scale, struct mesh_ui_rgb color) {
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
            y += fb_line_adv(state, scale);
            cursor = x;
            continue;
        } else {
            fb_draw_glyph(state, cursor, y, cell.codepoint, scale, color);
        }
        cursor += fb_char_adv(state, scale);
    }
}

void fb_fill_rect(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int h,
                  struct mesh_ui_rgb color) {
    fb_fill_packed(state, x, y, w, h, compose_color(state, color.r, color.g, color.b));
}

void fb_fill_round_rect(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int h,
                        int radius, struct mesh_ui_rgb color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    const int limit = (w < h ? w : h) / 2;
    if (radius > limit) {
        radius = limit;
    }
    if (radius <= 0) {
        fb_fill_rect(state, x, y, w, h, color);
        return;
    }

    const uint32_t packed = compose_color(state, color.r, color.g, color.b);
    /* The straight middle, then a span per row of each corner band. */
    fb_fill_packed(state, x, y + radius, w, h - 2 * radius, packed);

    /*
     * How far in the fill starts on each of the rounded rows.
     *
     * Everything is doubled so the test lands on pixel *centres* without leaving integers:
     * the row's centre is half a pixel below its top edge, and a circle drawn from pixel
     * corners is visibly lopsided at this size. `2*d - 2*r + 1` is twice the offset of the
     * pixel centre from the arc's centre, and the comparison is that against twice the radius.
     */
    const int diameter_sq = 4 * radius * radius;
    for (int dy = 0; dy < radius; ++dy) {
        const int oy = 2 * dy - 2 * radius + 1;
        int dx = 0;
        while (dx < radius) {
            const int ox = 2 * dx - 2 * radius + 1;
            if (ox * ox + oy * oy <= diameter_sq) {
                break;
            }
            ++dx;
        }
        const int span = w - 2 * dx;
        fb_fill_packed(state, x + dx, y + dy, span, 1, packed);
        fb_fill_packed(state, x + dx, y + h - 1 - dy, span, 1, packed);
    }
}

void fb_clear(const struct mesh_ui_backend_fb_state *state, struct mesh_ui_rgb color) {
    fb_fill_rect(state, 0, 0, (int)state->var.xres, (int)state->var.yres, color);
}

/* Columns of text that fit between the margins at this scale. */
size_t fb_cols(const struct mesh_ui_backend_fb_state *state, int scale) {
    const int usable = (int)state->var.xres - 2 * fb_margin(state);
    if (usable <= 0) {
        return 1U;
    }
    return (size_t)(usable / fb_char_adv(state, scale));
}

/* Clip a line to `cols` columns. Counted in drawn cells, not bytes, so a character is never
   cut in half - half a sequence would draw as the replacement box and, on the paths that also
   log or serialise the line, would be malformed UTF-8 - and a flag or a ZWJ sequence is never
   split into the pieces it is spelled with. */
void fb_fit(char *line, size_t cols) { mesh_ui_text_cell_truncate(line, cols); }

/* Columns a line occupies once drawn. */
size_t fb_width(const char *line) { return mesh_ui_text_cells(line); }

/*
 * Draw one list row, highlighting it when it is the cursor. `x` is the text origin; the
 * highlight spans the full width so the eye finds it without reading.
 *
 * The highlight is a rounded, inset shape rather than a full-bleed bar. Both halves of that
 * matter and for the same reason: a bar running edge to edge reads as a *band across the
 * screen*, while a shape with ends reads as one row picked out of a column of them - which is
 * what a cursor is. The corners come from the theme's shape scale, so a theme that wants the
 * old bar back asks for MESH_UI_SHAPE_SM of zero rather than for a different renderer.
 */
void fb_draw_row(const struct mesh_ui_backend_fb_state *state, int y, const char *text,
                 struct mesh_ui_rgb color, bool selected) {
    const int margin = fb_margin(state);
    const int line = fb_line_adv(state, state->scale);
    if (selected) {
        fb_fill_round_rect(state, margin / 2, y - state->scale, (int)state->var.xres - margin, line,
                           fb_radius(state, MESH_UI_SHAPE_SM),
                           fb_color(state, MESH_UI_COLOR_SURFACE_SEL));
        color = fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL);
    }
    fb_draw_text(state, margin, y, text, state->scale, color);
}

/* "3m", "2h", "5d" since a radio-reported epoch; "?" when either clock is unusable. */
void fb_format_age(uint32_t last_heard, char *out, size_t out_len) {
    if (last_heard == 0U) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT));
        return;
    }
    const time_t now = time(NULL);
    if (now <= 0 || (uint32_t)now < last_heard) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_TIME_NOW));
        return;
    }
    const uint32_t delta = (uint32_t)now - last_heard;
    if (delta < 60U) {
        mesh_str_format(out, out_len, MESH_STR_TIME_SECONDS_SHORT, delta);
    } else if (delta < 3600U) {
        mesh_str_format(out, out_len, MESH_STR_TIME_MINUTES_SHORT, delta / 60U);
    } else if (delta < 86400U) {
        mesh_str_format(out, out_len, MESH_STR_TIME_HOURS_SHORT, delta / 3600U);
    } else {
        mesh_str_format(out, out_len, MESH_STR_TIME_DAYS_SHORT, delta / 86400U);
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
                    size_t cols, int max_lines, struct mesh_ui_rgb color) {
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
        fb_draw_text(state, fb_margin(state), y, line, state->scale, color);
        y += fb_line_adv(state, state->scale);
        lines++;
        cursor += take;
        while (*cursor == ' ') {
            ++cursor;
        }
    }
    return lines;
}
