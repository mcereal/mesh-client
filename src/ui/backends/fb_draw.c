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
#include "mesh/utils/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Coverage is independent of the palette. A bounded, four-way cache holds the common
   glyph/scale pairs without retaining framebuffers or growing with incoming text. */
#define FB_GLYPH_CACHE_SETS 64U
#define FB_GLYPH_CACHE_WAYS 4U
#define FB_GLYPH_CACHE_PIXELS 2048U
struct fb_cached_glyph {
    const struct mesh_ui_font *font;
    uint32_t codepoint;
    int scale;
    uint64_t used;
    uint8_t steps[FB_GLYPH_CACHE_PIXELS];
};
struct fb_glyph_cache {
    uint64_t clock;
    struct fb_cached_glyph entries[FB_GLYPH_CACHE_SETS][FB_GLYPH_CACHE_WAYS];
};

void fb_glyph_cache_free(struct mesh_ui_backend_fb_state *state) {
    free(state->glyph_cache);
    state->glyph_cache = NULL;
}

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
    if (state->glyph_cache == NULL) {
        state->glyph_cache = calloc(1U, sizeof *state->glyph_cache);
    }
    state->theme = theme != NULL ? theme : mesh_ui_theme_default();
    state->scale = mesh_ui_theme_clamp_scale(state->theme, scale);
    /* Every position remembered in there is in pixels, measured against metrics this call has
       just replaced. Keeping them would slide a knob from where it sat under the old scale. */
    mesh_ui_anim_table_reset(&state->anim);
    /* And the frame's own transition, for the same reason and one more: a theme switch is not a
       move between screens, so a screen that slid in because the palette changed would be
       animating an event that did not happen. */
    memset(&state->slide, 0, sizeof state->slide);
    state->slide_dir = 0;
}

void fb_state_set_now(struct mesh_ui_backend_fb_state *state, uint64_t now_ms) {
    if (state != NULL && now_ms > state->now_ms) {
        state->now_ms = now_ms;
    }
}

bool fb_state_animating(const struct mesh_ui_backend_fb_state *state) {
    if (state == NULL) {
        return false;
    }
    /* The transition is asked about separately from the table because it is kept separately -
       see `slide` on the state. A frame owes another one while either has somewhere to be. */
    return mesh_ui_anim_active(&state->slide, state->now_ms) ||
           mesh_ui_anim_table_active(&state->anim, state->now_ms);
}

/*
 * How far a screen travels on its way in, as a fraction of the panel: a quarter of it.
 *
 * Not the whole width, and this is the one measurement in the transition that had to be looked
 * at rather than reasoned about. Only one screen is drawn (see fb_transition_offset()), so a
 * screen that started a full panel out left the body *empty* on the frame the press landed -
 * one blank frame, every time, before anything arrived. A blink is a worse artefact than no
 * animation at all.
 *
 * A quarter is also what Material's shared-axis transition does, and for the same reason
 * arrived at from the other end: there the displacement is small because the cross-fade is what
 * carries the change of identity, and the slide only says which way. Here there is no fade to
 * carry it - so the slide says which way *and* the content under the cursor is legible for the
 * whole of the move, which is what a blank frame was spending.
 */
#define FB_TRANSITION_TRAVEL_NUM 1
#define FB_TRANSITION_TRAVEL_DEN 4

/*
 * The move this frame is part of, as the distance the arriving screen still has to travel.
 *
 * Positive is a screen coming in from the right - which is what going a level deeper looks like
 * on every handheld - negative one coming in from the left, and 0 a frame that is not moving,
 * which is very nearly all of them.
 *
 * Where the "was" comes from is the whole design, and it is not in the snapshot: mesh/ui/route.h
 * reads the nav and says which *place* it is showing, this remembers the last one, and the
 * difference between two places is the direction. Nothing in the store or the nav records how it
 * got here, so no call site that opens a level has to remember to say so.
 *
 * Only one screen is ever rendered. There is no alpha on this panel and nothing can read back
 * what is already on it, so a cross-fade is out and so is carrying the outgoing screen along
 * beside the incoming one - which would want a second page of pixels held for the length of the
 * move. What is left is a displacement, and FB_TRANSITION_TRAVEL_NUM is how much of one.
 *
 * A theme that asks for no motion gets none: mesh_ui_anim_to() with a zero duration puts the
 * value on its target, which lands the screen in place on the frame it arrives.
 */
int fb_transition_offset(struct mesh_ui_backend_fb_state *state, const struct mesh_ui_nav *nav) {
    if (state == NULL || nav == NULL) {
        return 0;
    }

    struct mesh_ui_route route;
    mesh_ui_route_of(nav, &route);

    if (!state->route_valid) {
        /* First sight adopts, the rule the animation table follows for an id it has not seen: a
           screen that slid in on the frame the client came up would be announcing itself rather
           than reporting a move. */
        state->route = route;
        state->route_valid = true;
        return 0;
    }

    const enum mesh_ui_transition move = mesh_ui_route_move(&state->route, &route);
    state->route = route;
    if (move != MESH_UI_TRANSITION_NONE) {
        state->slide_dir = (move == MESH_UI_TRANSITION_FORWARD) ? 1 : -1;
        /* From the far end every time, including when a move interrupts one already running:
           a second press is a second screen arriving, not the first one changing its mind about
           where it was going. */
        mesh_ui_anim_set(&state->slide, 0);
        mesh_ui_anim_to(&state->slide, state->now_ms, MESH_UI_ANIM_ONE,
                        /* MEDIUM rather than the SHORT the audit named. SHORT is what a control
                           acknowledging a press takes, and it is also what anything *leaving*
                           takes - and this is the one animation here with nothing leaving in it.
                           The token whose stated meaning is "something arriving that was not
                           there" is the one a screen arriving should be spending. */
                        fb_motion(state, MESH_UI_MOTION_MEDIUM), MESH_UI_EASE_OUT);
    }

    if (state->slide_dir == 0) {
        return 0;
    }
    const int32_t remaining = MESH_UI_ANIM_ONE - mesh_ui_anim_value(&state->slide, state->now_ms);
    if (remaining <= 0) {
        state->slide_dir = 0;
        return 0;
    }
    const int travel = (int)state->var.xres * FB_TRANSITION_TRAVEL_NUM / FB_TRANSITION_TRAVEL_DEN;
    return state->slide_dir * (int)(((int64_t)remaining * travel) / MESH_UI_ANIM_ONE);
}

void fb_shift_begin(struct mesh_ui_backend_fb_state *state, int dx, int top, int bottom) {
    if (state == NULL || bottom <= top) {
        return;
    }
    state->shift_x = dx;
    state->shift_top = top;
    state->shift_bottom = bottom;
    state->shift_active = true;
}

void fb_shift_end(struct mesh_ui_backend_fb_state *state) {
    if (state != NULL) {
        state->shift_active = false;
        state->shift_x = 0;
    }
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

struct mesh_ui_paint fb_paint(const struct mesh_ui_backend_fb_state *state,
                              enum mesh_ui_family family, enum mesh_ui_slot slot,
                              enum mesh_ui_state ui_state) {
    return mesh_ui_theme_paint(state != NULL ? state->theme : NULL, family, slot, ui_state);
}

struct mesh_ui_rgb fb_state_layer(const struct mesh_ui_backend_fb_state *state,
                                  enum mesh_ui_color fill, enum mesh_ui_color ink,
                                  enum mesh_ui_state ui_state) {
    return mesh_ui_theme_state_layer(fb_color(state, fill), fb_color(state, ink), ui_state);
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

int fb_space(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_space space) {
    return mesh_ui_theme_space(state->theme, space, state->scale);
}

int fb_space_at(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_space space, int scale) {
    return mesh_ui_theme_space(state->theme, space, scale);
}

int fb_type_scale(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_type type) {
    return mesh_ui_theme_type_scale(state->theme, type, state->scale);
}

int fb_gutter(const struct mesh_ui_backend_fb_state *state) {
    const int margin = fb_margin(state);
    return margin > 1 ? margin / 2 : margin;
}

uint32_t fb_motion(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_motion motion) {
    return mesh_ui_theme_motion(state->theme, motion);
}

int fb_edge(const struct mesh_ui_backend_fb_state *state) {
    const int edge = fb_space(state, MESH_UI_SPACE_XS);
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
    /*
     * The frame's transform, before anything is measured against the panel: a screen arriving
     * from off the right-hand edge is drawn at coordinates that are not on the panel at all,
     * and the clamp below is what turns that into the part of it that has arrived. The band is
     * applied here rather than left to the caller for the same reason - a row whose glyphs
     * overhang the top of the body must be cut off at the body, not drawn over the navigation
     * bar it is sliding underneath. See fb_shift_begin().
     */
    if (state->shift_active) {
        x += state->shift_x;
        if (y < state->shift_top) {
            h -= state->shift_top - y;
            y = state->shift_top;
        }
        if (y + h > state->shift_bottom) {
            h = state->shift_bottom - y;
        }
        if (h <= 0) {
            return;
        }
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

    if (state->clip_active) {
        const int right = x + w < state->clip.right ? x + w : state->clip.right;
        const int bottom = y + h < state->clip.bottom ? y + h : state->clip.bottom;
        if (x < state->clip.x)
            x = state->clip.x;
        if (y < state->clip.y)
            y = state->clip.y;
        w = right - x;
        h = bottom - y;
        if (w <= 0 || h <= 0)
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

/*
 * The coverage ramp a tinted sprite - a glyph or an icon - is drawn in.
 *
 * 32 steps is below what the eye separates at this size, and packing a colour per pixel was
 * the thing fb_fill_packed() exists to avoid: quantising first makes the blend one multiply
 * per channel per *step* rather than per pixel, and lets equal steps coalesce into spans.
 * Text and icons share it because they are the same operation - coverage, tinted with the
 * ink, over a ground the caller has just filled.
 */
#define FB_BLEND_STEPS 32

static void fb_blend_table(const struct mesh_ui_backend_fb_state *state, struct mesh_ui_rgb ink,
                           struct mesh_ui_rgb ground, uint32_t out[FB_BLEND_STEPS]) {
    for (int step = 0; step < FB_BLEND_STEPS; ++step) {
        const int32_t a = (int32_t)step * 255 / (FB_BLEND_STEPS - 1);
        const uint8_t r = (uint8_t)(((int32_t)ink.r * a + (int32_t)ground.r * (255 - a)) / 255);
        const uint8_t g = (uint8_t)(((int32_t)ink.g * a + (int32_t)ground.g * (255 - a)) / 255);
        const uint8_t b = (uint8_t)(((int32_t)ink.b * a + (int32_t)ground.b * (255 - a)) / 255);
        out[step] = compose_color(state, r, g, b);
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

/* The widest cell fb_draw_glyph() will resample into: the largest cell a font may declare, at
   the largest scale the type scale can clamp to. */
#define FB_GLYPH_BOX_MAX (MESH_UI_GLYPH_MAX_WIDTH * MESH_UI_SCALE_MAX)

/*
 * One axis of the resample: the two master samples a destination pixel sits between, and how
 * far between them it is.
 *
 * A pixel-art font sets `lo == hi` and `frac == 0`, which collapses the interpolation below
 * into a plain lookup - so one formula serves both samplings with no per-pixel branch, and
 * 5x7 comes out of it as the same hard-edged blocks it has always drawn.
 */
struct fb_glyph_tap {
    int16_t lo;
    int16_t hi;
    int16_t frac; /* 0..255, the position between `lo` and `hi` */
};

static struct fb_glyph_tap fb_glyph_tap(int index, int box, int master,
                                        enum mesh_ui_font_sampling sampling) {
    struct fb_glyph_tap tap = {0, 0, 0};
    if (master <= 0 || box <= 0) {
        return tap;
    }
    /* Half-pixel offsets at both ends: sampling from the pixel's centre is what keeps a
       symmetric glyph symmetric after the scale. */
    int32_t pos = (((int32_t)index * 2 + 1) * master * 128) / box - 128;
    if (pos < 0) {
        pos = 0;
    }
    if (sampling == MESH_UI_FONT_PIXEL) {
        /* Nearest, which on the integer ratio a pixel font is drawn at is exactly the source
           block - rounding rather than truncating is what keeps the block boundaries where
           they were. */
        int32_t at = (pos + 128) >> 8;
        if (at > master - 1) {
            at = master - 1;
        }
        tap.lo = (int16_t)at;
        tap.hi = (int16_t)at;
        return tap;
    }
    int32_t lo = pos >> 8;
    if (lo > master - 1) {
        lo = master - 1;
    }
    tap.lo = (int16_t)lo;
    tap.hi = (int16_t)(lo + 1 < master ? lo + 1 : lo);
    tap.frac = (int16_t)(pos & 0xFF);
    return tap;
}

/* Coverage at one destination pixel, quantised into the blend table's index. The rounding
   lives here rather than at the call site for the reason fb_icon_step()'s does: measuring a
   span and drawing it must round identically or the span boundaries move. */
static int fb_glyph_step(const uint8_t *row_lo, const uint8_t *row_hi,
                         const struct fb_glyph_tap *tap, int32_t fy) {
    const int32_t fx = tap->frac;
    const int32_t upper = row_lo[tap->lo] * (256 - fx) + row_lo[tap->hi] * fx;
    const int32_t lower = row_hi[tap->lo] * (256 - fx) + row_hi[tap->hi] * fx;
    const int32_t alpha = upper * (256 - fy) + lower * fy; /* 0 .. MAX_ALPHA << 16 */
    return (int)((alpha * (FB_BLEND_STEPS - 1)) / (MESH_UI_GLYPH_MAX_ALPHA * 65536));
}

/*
 * Draw one glyph's coverage into its cell, in a ramp the caller has already built.
 *
 * The master is resampled into `width * scale` by `height * scale` and emitted as spans of
 * equal coverage, the way fb_draw_icon() emits a symbol. Taking the ramp rather than a colour
 * pair is what keeps a line of text to one table build instead of one per character.
 *
 * The master's overhang rows are drawn too, above `y` - they are the same resample continued
 * upward, not a second pass, which is why the whole glyph is one box placed by its baseline
 * rather than a cell plus an accent stuck on top of it.
 */
static void fb_draw_glyph_ramp(const struct mesh_ui_backend_fb_state *state, int x, int y,
                               uint32_t codepoint, int scale,
                               const uint32_t blend[FB_BLEND_STEPS]) {
    const struct mesh_ui_font *font = fb_font(state);
    const int cell_rows = (int)font->master_h - (int)font->master_top;
    const int box_w = (int)font->width * scale;
    const int box_h = (int)font->height * scale;
    if (scale <= 0 || box_w <= 0 || box_h <= 0 || box_w > FB_GLYPH_BOX_MAX ||
        font->master_w == 0U || cell_rows <= 0) {
        return;
    }
    /* The cell is `cell_rows` of the master, so the whole of it - overhang included - is that
       much taller, and starts that much higher. Both from the one division, so the overhang
       lands exactly on the row the cell starts at. */
    const int full_h = box_h * (int)font->master_h / cell_rows;
    const int top_off = box_h * (int)font->master_top / cell_rows;
    if (full_h <= 0) {
        return;
    }

    struct fb_cached_glyph *cached = NULL;
    bool hit = false;
    if (state->glyph_cache != NULL && (size_t)box_w * (size_t)full_h <= FB_GLYPH_CACHE_PIXELS) {
        struct fb_glyph_cache *cache = state->glyph_cache;
        const size_t set = (codepoint * 31U + (uint32_t)scale * 17U) % FB_GLYPH_CACHE_SETS;
        cached = &cache->entries[set][0];
        for (size_t i = 0; i < FB_GLYPH_CACHE_WAYS; ++i) {
            struct fb_cached_glyph *entry = &cache->entries[set][i];
            if (entry->font == font && entry->codepoint == codepoint && entry->scale == scale) {
                cached = entry;
                hit = true;
                break;
            }
            if (entry->used < cached->used) {
                cached = entry;
            }
        }
        cached->used = ++cache->clock;
    }

    struct mesh_ui_glyph glyph;
    struct fb_glyph_tap taps[FB_GLYPH_BOX_MAX];
    if (!hit) {
        (void)mesh_ui_font_glyph(font, codepoint, &glyph);
        for (int dx = 0; dx < box_w; ++dx) {
            taps[dx] = fb_glyph_tap(dx, box_w, (int)font->master_w, font->sampling);
        }
        if (cached != NULL) {
            cached->font = font;
            cached->codepoint = codepoint;
            cached->scale = scale;
        }
    }

    const int top = y - top_off;
    for (int dy = 0; dy < full_h; ++dy) {
        uint8_t scratch[FB_GLYPH_BOX_MAX];
        uint8_t *steps = cached != NULL ? &cached->steps[(size_t)dy * (size_t)box_w] : scratch;
        if (!hit) {
            const struct fb_glyph_tap row =
                fb_glyph_tap(dy, full_h, (int)font->master_h, font->sampling);
            const uint8_t *row_lo = &glyph.alpha[(size_t)row.lo * font->master_w];
            const uint8_t *row_hi = &glyph.alpha[(size_t)row.hi * font->master_w];
            for (int dx = 0; dx < box_w; ++dx) {
                steps[dx] = (uint8_t)fb_glyph_step(row_lo, row_hi, &taps[dx], row.frac);
            }
        }
        int dx = 0;
        while (dx < box_w) {
            const int step = steps[dx];
            int end = dx + 1;
            while (end < box_w && steps[end] == step) {
                ++end;
            }
            if (step > 0) {
                fb_fill_packed(state, x + dx, top + dy, end - dx, 1, blend[step]);
            }
            dx = end;
        }
    }
}

void fb_draw_glyph(const struct mesh_ui_backend_fb_state *state, int x, int y, uint32_t codepoint,
                   int scale, struct mesh_ui_rgb ink, struct mesh_ui_rgb ground) {
    uint32_t blend[FB_BLEND_STEPS];
    fb_blend_table(state, ink, ground, blend);
    fb_draw_glyph_ramp(state, x, y, codepoint, scale, blend);
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
 *
 * The height being matched is the *symbol's*, not the sprite's: a sprite is a window a little
 * wider than Material's grid (MESH_UI_ICON_WINDOW), and the shape lives in the central body of
 * it (MESH_UI_ICON_BODY) with air around the outside. Scaling by the ratio is what puts the
 * shape on the capitals' height rather than the air - drawing the window at the body's height
 * instead would sit every symbol a fifth short of the text it labels.
 */
int fb_icon_drawn(const struct mesh_ui_backend_fb_state *state, int scale) {
    const int body = mesh_ui_font_cap(fb_font(state), scale);
    return (body * MESH_UI_ICON_WINDOW + MESH_UI_ICON_BODY / 2) / MESH_UI_ICON_BODY;
}

/* The largest box fb_draw_icon() can be asked for: the empty state's symbol at the largest glyph
   scale, plus the air its window carries around it. Rounded the way fb_icon_drawn() rounds, not
   merely scaled the same way - a bound a pixel under what it is bounding fails the check below,
   and an icon that fails that check is not drawn at all. */
#define FB_ICON_DRAWN_MAX                                                                          \
    ((MESH_UI_GLYPH_MAX_HEIGHT * FB_ICON_SCALE_MAX * MESH_UI_ICON_WINDOW +                         \
      MESH_UI_ICON_BODY / 2) /                                                                     \
     MESH_UI_ICON_BODY)

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
    return (int)((alpha * (FB_BLEND_STEPS - 1)) / (MESH_UI_ICON_MAX_ALPHA * 65536));
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
 * pixel, because coverage is quantised into FB_BLEND_STEPS packed colours first.
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
    int32_t sx[FB_ICON_DRAWN_MAX];
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

    uint32_t blend[FB_BLEND_STEPS];
    fb_blend_table(state, ink, ground, blend);

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
                  int scale, struct mesh_ui_rgb ink, struct mesh_ui_rgb ground) {
    /* One ramp for the whole run: every character in it is the same ink over the same ground,
       and building the table per glyph would cost more than drawing one. */
    uint32_t blend[FB_BLEND_STEPS];
    fb_blend_table(state, ink, ground, blend);

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
            fb_draw_glyph_ramp(state, cursor, y, cell.codepoint, scale, blend);
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
 * The cursor's highlight, and the ground everything on the row is drawn against.
 *
 * It is a rounded, inset shape rather than a full-bleed bar. Both halves of that matter and for
 * the same reason: a bar running edge to edge reads as a *band across the screen*, while a
 * shape with ends reads as one row picked out of a column of them - which is what a cursor is.
 * The corners come from the theme's shape scale, so a theme that wants the old bar back asks
 * for MESH_UI_SHAPE_SM of zero rather than for a different renderer.
 *
 * Its own function because a list mixes row shapes: a plain row and a section heading in the
 * same list highlighting to two slightly different rectangles is a cursor that changes shape as
 * it walks, and two copies of `y - scale` is exactly how that happens.
 */
struct mesh_ui_rgb fb_draw_row_fill(const struct mesh_ui_backend_fb_state *state, int y,
                                    uint32_t rows, bool selected) {
    if (!selected) {
        return fb_color(state, MESH_UI_COLOR_BG);
    }
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_SURFACE_SEL);
    const int line = fb_line_adv(state, state->scale);
    fb_fill_round_rect(state, fb_gutter(state), y - state->scale,
                       (int)state->var.xres - fb_margin(state), (int)(rows > 0U ? rows : 1U) * line,
                       fb_radius(state, MESH_UI_SHAPE_SM), ground);
    return ground;
}

/* One list row of text, highlighted when it is the cursor: the fill above, then the words. */
void fb_draw_row(const struct mesh_ui_backend_fb_state *state, int y, const char *text,
                 struct mesh_ui_rgb color, bool selected) {
    const struct mesh_ui_rgb ground = fb_draw_row_fill(state, y, 1U, selected);
    if (selected) {
        color = fb_color(state, MESH_UI_COLOR_TEXT_ON_SEL);
    }
    fb_draw_text(state, fb_margin(state), y, text, state->scale, color, ground);
}

/* "3m", "2h", "5d" since a radio-reported epoch; "?" when either clock is unusable. */
void fb_format_age(uint32_t last_heard, char *out, size_t out_len) {
    if (last_heard == 0U) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_COMMON_UNKNOWN_SHORT));
        return;
    }
    const uint32_t now = mesh_time_wall_s();
    if (now == 0U || now < last_heard) {
        snprintf(out, out_len, "%s", mesh_str(MESH_STR_TIME_NOW));
        return;
    }
    const uint32_t delta = now - last_heard;
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

/* Wraps `text` into at most `max_lines` lines of `cols` columns, drawing each from `x`.

   The x is a parameter because a dialog's paragraph is inset from its panel's edge rather than
   from the body's - the body margin was the only answer while the only wrapped text on screen
   was a screen's own. */
int fb_draw_wrapped_at(const struct mesh_ui_backend_fb_state *state, int x, int y, const char *text,
                       size_t cols, int max_lines, struct mesh_ui_rgb color,
                       struct mesh_ui_rgb ground) {
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
        fb_draw_text(state, x, y, line, state->scale, color, ground);
        y += fb_line_adv(state, state->scale);
        lines++;
        cursor += take;
        while (*cursor == ' ') {
            ++cursor;
        }
    }
    return lines;
}

/* The same, from the body's left margin - which is where a screen's own wrapped text starts. */
int fb_draw_wrapped(const struct mesh_ui_backend_fb_state *state, int y, const char *text,
                    size_t cols, int max_lines, struct mesh_ui_rgb color,
                    struct mesh_ui_rgb ground) {
    return fb_draw_wrapped_at(state, fb_margin(state), y, text, cols, max_lines, color, ground);
}
