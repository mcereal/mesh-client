#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/backends/fb.h"

#include "mesh/event_loop.h"
#include "mesh/log.h"
#include "mesh/mesh_message.h"
#include "mesh/ui/input.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct mesh_ui_backend_fb_state {
    int fb_fd;
    uint8_t *fb_ptr;
    size_t fb_size;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    uint32_t line_bytes;
    uint32_t bytes_per_pixel;
    bool pan_failed_logged;
    /* Glyph multiplier for body text; the tab bar and footer use one step smaller. The Brick's
       3.2" panel is 1024 px wide, so 4 gives ~41 columns of legible text. */
    int scale;
};

struct fb_rgb {
    uint8_t r, g, b;
};

/* Palette. Dark ground, cool greys for chrome, one warm colour for things that need the eye. */
static const struct fb_rgb k_bg = {0x0A, 0x14, 0x1E};
static const struct fb_rgb k_text = {220, 230, 240};
static const struct fb_rgb k_dim = {140, 150, 165};
static const struct fb_rgb k_tab_active_bg = {60, 110, 170};
static const struct fb_rgb k_cursor_bg = {40, 80, 120};
static const struct fb_rgb k_white = {255, 255, 255};
static const struct fb_rgb k_inbound = {235, 245, 255};
static const struct fb_rgb k_outbound = {170, 190, 210};
static const struct fb_rgb k_accent = {255, 220, 120};
static const struct fb_rgb k_good = {120, 220, 150};
static const struct fb_rgb k_bad = {240, 120, 120};

#define FONT_WIDTH 5
#define FONT_HEIGHT 7
#define FB_MARGIN 16
#define FB_DEFAULT_SCALE 4
#define FB_MIN_SCALE 2
#define FB_MAX_SCALE 6

static const uint8_t k_font5x7[96][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x3E, 0x41, 0x5D, 0x55, 0x1E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x7F, 0x20, 0x18, 0x20, 0x7F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20}, {0x38, 0x44, 0x44, 0x48, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0x7C, 0x14, 0x14, 0x14, 0x08},
    {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x08, 0x3E, 0x41, 0x41, 0x00}, {0x00, 0x00, 0x7F, 0x00, 0x00},
    {0x00, 0x41, 0x41, 0x3E, 0x08}, {0x02, 0x01, 0x02, 0x04, 0x02},
};

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
static inline int fb_char_adv(int scale) { return FONT_WIDTH * scale + scale; }
static inline int fb_line_adv(int scale) { return FONT_HEIGHT * scale + 2 * scale; }

static void fb_draw_char(const struct mesh_ui_backend_fb_state *state, int x, int y,
                         unsigned char ch, int scale, struct fb_rgb color) {
    if (ch < 32 || ch > 126) {
        ch = '?';
    }
    const uint8_t *glyph = k_font5x7[ch - 32];
    for (int col = 0; col < FONT_WIDTH; ++col) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT_HEIGHT; ++row) {
            if (bits & (1U << row)) {
                for (int sx = 0; sx < scale; ++sx) {
                    for (int sy = 0; sy < scale; ++sy) {
                        fb_draw_pixel(state, x + col * scale + sx, y + row * scale + sy, color.r,
                                      color.g, color.b);
                    }
                }
            }
        }
    }
}

static void fb_draw_text(const struct mesh_ui_backend_fb_state *state, int x, int y,
                         const char *text, int scale, struct fb_rgb color) {
    int cursor = x;
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; ++c) {
        if (*c == '\n') {
            y += fb_line_adv(scale);
            cursor = x;
            continue;
        }
        fb_draw_char(state, cursor, y, *c, scale, color);
        cursor += fb_char_adv(scale);
    }
}

static void fb_fill_rect(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int h,
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

static void fb_clear(const struct mesh_ui_backend_fb_state *state, struct fb_rgb color) {
    fb_fill_rect(state, 0, 0, (int)state->var.xres, (int)state->var.yres, color);
}

/* Columns of text that fit between the margins at this scale. */
static size_t fb_cols(const struct mesh_ui_backend_fb_state *state, int scale) {
    const int usable = (int)state->var.xres - 2 * FB_MARGIN;
    if (usable <= 0) {
        return 1U;
    }
    return (size_t)(usable / fb_char_adv(scale));
}

static void fb_fit(char *line, size_t cols) {
    if (strlen(line) > cols) {
        line[cols] = '\0';
    }
}

/* Draw one list row, highlighting it when it is the cursor. `x` is the text origin; the
   highlight spans the full width so the eye finds it without reading. */
static void fb_draw_row(const struct mesh_ui_backend_fb_state *state, int y, const char *text,
                        struct fb_rgb color, bool selected) {
    const int line = fb_line_adv(state->scale);
    if (selected) {
        fb_fill_rect(state, FB_MARGIN / 2, y - state->scale, (int)state->var.xres - FB_MARGIN, line,
                     k_cursor_bg);
        color = k_white;
    }
    fb_draw_text(state, FB_MARGIN, y, text, state->scale, color);
}

/* "3m", "2h", "5d" since a radio-reported epoch; "?" when either clock is unusable. */
static void fb_format_age(uint32_t last_heard, char *out, size_t out_len) {
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

static void fb_format_clock(uint32_t rx_time, char *out, size_t out_len) {
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
static uint32_t fb_first_visible(uint32_t cursor, uint32_t count, uint32_t visible) {
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

struct fb_layout {
    int body_y;    /* first body row */
    int footer_y;  /* top of the two footer lines */
    int line;      /* body line advance */
    uint32_t rows; /* body rows available */
    size_t cols;   /* body columns */
    int small;     /* scale for chrome text */
};

static void fb_draw_tabs(const struct mesh_ui_backend_fb_state *state,
                         const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const int small = layout->small;
    const int adv = fb_char_adv(small);
    const int line = fb_line_adv(small);
    const int y = FB_MARGIN / 2 + small;
    int x = FB_MARGIN / 2;

    for (int i = 0; i < MESH_UI_SCREEN_COUNT; ++i) {
        const enum mesh_ui_screen screen = (enum mesh_ui_screen)i;
        const char *name = mesh_ui_screen_name(screen);
        const int width = (int)strlen(name) * adv + 2 * small;
        const bool active = (snapshot->nav.screen == screen);
        if (active) {
            fb_fill_rect(state, x, y - small, width, line, k_tab_active_bg);
        }
        fb_draw_text(state, x + small, y, name, small, active ? k_white : k_dim);
        x += width + adv;
    }

    /* Rule under the tab strip. */
    fb_fill_rect(state, 0, y + line, (int)state->var.xres, small / 2 > 0 ? small / 2 : 1,
                 k_tab_active_bg);
    layout->body_y = y + line + 2 * small + FB_MARGIN / 2;
}

static void fb_draw_footer(const struct mesh_ui_backend_fb_state *state,
                           const struct mesh_ui_snapshot *snapshot, const struct fb_layout *layout,
                           const char *hint) {
    const int small = layout->small;
    const int line = fb_line_adv(small);
    const size_t cols = fb_cols(state, small);
    char text[160];

    /* Line 1: what the buttons do here. */
    snprintf(text, sizeof text, "%s", hint);
    fb_fit(text, cols);
    fb_draw_text(state, FB_MARGIN, layout->footer_y, text, small, k_dim);

    /* Line 2: a toast when there is one, else the link summary. */
    const struct mesh_ui_nav *nav = &snapshot->nav;
    struct fb_rgb color = k_dim;
    if (nav->toast[0] != '\0') {
        snprintf(text, sizeof text, "%s", nav->toast);
        color = k_accent;
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
            color = k_good;
        } else {
            snprintf(text, sizeof text, "%s | %s", status, mesh_ui_input_quit_hint());
        }
    }
    fb_fit(text, cols);
    fb_draw_text(state, FB_MARGIN, layout->footer_y + line, text, small, color);
}

static void fb_draw_title(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                          const char *title) {
    char text[128];
    snprintf(text, sizeof text, "%s", title);
    fb_fit(text, layout->cols);
    fb_draw_text(state, FB_MARGIN, layout->body_y, text, state->scale, k_accent);
    layout->body_y += layout->line + state->scale;
    if (layout->rows > 1U) {
        layout->rows -= 1U;
    }
}

static void fb_draw_empty(const struct mesh_ui_backend_fb_state *state,
                          const struct fb_layout *layout, const char *text) {
    fb_draw_text(state, FB_MARGIN, layout->body_y, text, state->scale, k_dim);
}

/* Wraps `text` into at most `max_lines` lines of `cols` columns, drawing each. */
static int fb_draw_wrapped(const struct mesh_ui_backend_fb_state *state, int y, const char *text,
                           size_t cols, int max_lines, struct fb_rgb color) {
    int lines = 0;
    const char *cursor = text;
    char line[160];
    if (cols >= sizeof line) {
        cols = sizeof line - 1U;
    }
    while (*cursor != '\0' && lines < max_lines) {
        size_t take = strlen(cursor);
        if (take > cols) {
            take = cols;
            /* Break at the last space inside the window when there is one. */
            for (size_t i = take; i > cols / 2; --i) {
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

static void fb_render_messages(const struct mesh_ui_backend_fb_state *state,
                               const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_message_list *messages = &snapshot->messages;
    const struct mesh_ui_nav *nav = &snapshot->nav;
    char title[96];
    if (messages->dropped > 0U) {
        snprintf(title, sizeof title, "Messages (%u, +%u older)", (unsigned)messages->count,
                 (unsigned)messages->dropped);
    } else {
        snprintf(title, sizeof title, "Messages (%u)", (unsigned)messages->count);
    }
    fb_draw_title(state, layout, title);

    if (messages->count == 0U) {
        fb_draw_empty(state, layout, "No messages yet. Compose sends a quick reply.");
        return;
    }

    /* The bottom of the body is a detail pane for the selected message: full text, sender,
       channel and time, since the list rows are clipped to one line. */
    const int detail_lines = 3;
    uint32_t list_rows = layout->rows > (uint32_t)detail_lines + 1U
                             ? layout->rows - (uint32_t)detail_lines - 1U
                             : 1U;
    const uint32_t count =
        messages->count > MESH_UI_MAX_MESSAGES ? MESH_UI_MAX_MESSAGES : messages->count;
    const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_MESSAGES] < count
                                ? nav->cursor[MESH_UI_SCREEN_MESSAGES]
                                : count - 1U;
    const uint32_t first = fb_first_visible(cursor, count, list_rows);

    int y = layout->body_y;
    char line[300];
    for (uint32_t i = first; i < count && i < first + list_rows; ++i) {
        const struct mesh_ui_message *message = &messages->entries[i];
        const bool outbound = (message->direction == MESH_MESSAGE_OUTBOUND);
        const char *peer = message->peer_name[0] != '\0' ? message->peer_name : "?";
        char tag[8] = "";
        if (outbound && message->ack != MESH_MESSAGE_ACK_NONE) {
            snprintf(tag, sizeof tag, " %s",
                     message->ack == MESH_MESSAGE_ACK_DELIVERED ? "ok"
                     : message->ack == MESH_MESSAGE_ACK_FAILED  ? "!!"
                                                                : "..");
        }
        snprintf(line, sizeof line, "%s%s%s: %s", outbound ? ">" : "<", peer, tag, message->text);
        fb_fit(line, layout->cols);
        fb_draw_row(state, y, line, outbound ? k_outbound : k_inbound, i == cursor);
        y += layout->line;
    }

    /* Detail pane. */
    const struct mesh_ui_message *selected = &messages->entries[cursor];
    y = layout->body_y + (int)list_rows * layout->line + layout->line / 2;
    fb_fill_rect(state, FB_MARGIN, y - state->scale, (int)state->var.xres - 2 * FB_MARGIN,
                 state->scale / 2 > 0 ? state->scale / 2 : 1, k_cursor_bg);
    y += state->scale * 2;

    char clock[8];
    fb_format_clock(selected->rx_time, clock, sizeof clock);
    snprintf(line, sizeof line, "%s %s%s ch%u%s%s",
             selected->direction == MESH_MESSAGE_OUTBOUND ? "To" : "From",
             selected->peer_name[0] != '\0' ? selected->peer_name : "?",
             selected->broadcast ? " (all)" : "", (unsigned)selected->channel,
             clock[0] != '\0' ? " " : "", clock);
    fb_fit(line, layout->cols);
    fb_draw_text(state, FB_MARGIN, y, line, state->scale, k_dim);
    y += layout->line;
    fb_draw_wrapped(state, y, selected->text, layout->cols, detail_lines - 1, k_text);
}

static void fb_render_nodes(const struct mesh_ui_backend_fb_state *state,
                            const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    if (!snapshot->handshake_valid || snapshot->handshake.node_count == 0U) {
        fb_draw_title(state, layout, "Nodes");
        fb_draw_empty(state, layout,
                      snapshot->handshake_valid ? "Waiting for the node list..."
                                                : "Connect to a node to see the mesh.");
        return;
    }

    const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
    const uint32_t count =
        hs->node_count > MESH_UI_MAX_HANDSHAKE_NODES ? MESH_UI_MAX_HANDSHAKE_NODES : hs->node_count;
    char title[96];
    if (hs->has_my_info && hs->my_info.nodedb_entries > count) {
        snprintf(title, sizeof title, "Nodes (%u of %u)", count, hs->my_info.nodedb_entries);
    } else {
        snprintf(title, sizeof title, "Nodes (%u)", count);
    }
    fb_draw_title(state, layout, title);

    const uint32_t cursor =
        nav->cursor[MESH_UI_SCREEN_NODES] < count ? nav->cursor[MESH_UI_SCREEN_NODES] : count - 1U;
    const uint32_t first = fb_first_visible(cursor, count, layout->rows);
    const uint32_t me = hs->has_my_info ? hs->my_info.node_num : 0U;

    int y = layout->body_y;
    char line[160];
    char right[32];
    char age[8];
    for (uint32_t i = first; i < count && i < first + layout->rows; ++i) {
        const struct mesh_ui_node_summary *node = &hs->nodes[i];
        const char *short_name = node->short_name[0] != '\0' ? node->short_name : "----";
        const char *long_name = node->long_name[0] != '\0' ? node->long_name : "";
        fb_format_age(node->last_heard, age, sizeof age);

        if (node->has_hops_away && node->hops_away > 0U) {
            snprintf(right, sizeof right, "%uhop %s", (unsigned)node->hops_away, age);
        } else if (node->via_mqtt) {
            snprintf(right, sizeof right, "mqtt %s", age);
        } else {
            snprintf(right, sizeof right, "%.1fdB %s", (double)node->snr, age);
        }

        /* Left part is clipped so the right-aligned metrics always fit. */
        const size_t right_len = strlen(right);
        size_t left_cols = layout->cols > right_len + 1U ? layout->cols - right_len - 1U : 8U;
        snprintf(line, sizeof line, "%c%-4s %s", (me != 0U && node->node_id == me) ? '*' : ' ',
                 short_name, long_name);
        fb_fit(line, left_cols);
        const size_t pad =
            layout->cols > strlen(line) + right_len ? layout->cols - strlen(line) - right_len : 1U;
        snprintf(line + strlen(line), sizeof line - strlen(line), "%*s%s", (int)pad, "", right);

        fb_draw_row(state, y, line, (node->node_id == nav->target_node) ? k_accent : k_text,
                    i == cursor);
        y += layout->line;
    }
}

static void fb_render_compose(const struct mesh_ui_backend_fb_state *state,
                              const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    fb_draw_title(state, layout, "Compose");

    const size_t canned = mesh_ui_canned_count();
    const uint32_t count = 1U + (uint32_t)canned;
    const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_COMPOSE] < count
                                ? nav->cursor[MESH_UI_SCREEN_COMPOSE]
                                : count - 1U;
    const uint32_t first = fb_first_visible(cursor, count, layout->rows);

    int y = layout->body_y;
    char line[160];
    for (uint32_t i = first; i < count && i < first + layout->rows; ++i) {
        if (i == 0U) {
            snprintf(line, sizeof line, "To: %s%s", nav->target_name,
                     nav->target_node == MESH_MESSAGE_BROADCAST_ADDR ? " (primary channel)"
                                                                     : " (direct)");
            fb_fit(line, layout->cols);
            fb_draw_row(state, y, line, k_accent, i == cursor);
        } else {
            snprintf(line, sizeof line, "  %s", mesh_ui_canned_text(i - 1U));
            fb_fit(line, layout->cols);
            fb_draw_row(state, y, line, k_text, i == cursor);
        }
        y += layout->line;
    }
}

static void fb_render_devices(const struct mesh_ui_backend_fb_state *state,
                              const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    char title[96];
    snprintf(title, sizeof title, "Devices (%zu)", snapshot->device_count);
    fb_draw_title(state, layout, title);

    if (snapshot->device_count == 0U) {
        fb_draw_empty(state, layout, "Scanning for Meshtastic nodes...");
        return;
    }

    const uint32_t count = (uint32_t)snapshot->device_count;
    const uint32_t cursor = nav->cursor[MESH_UI_SCREEN_DEVICES] < count
                                ? nav->cursor[MESH_UI_SCREEN_DEVICES]
                                : count - 1U;
    const uint32_t first = fb_first_visible(cursor, count, layout->rows);

    int y = layout->body_y;
    char line[160];
    for (uint32_t i = first; i < count && i < first + layout->rows; ++i) {
        const struct mesh_ui_device *device = &snapshot->devices[i];
        const char *name = device->name[0] != '\0' ? device->name : device->identifier;
        if (name[0] == '\0') {
            name = "<unknown>";
        }
        snprintf(line, sizeof line, "%c %s  %ddBm%s", device->connected ? '*' : ' ', name,
                 (int)device->rssi, device->connected ? "  connected" : "");
        fb_fit(line, layout->cols);
        fb_draw_row(state, y, line, device->connected ? k_good : k_text, i == cursor);
        y += layout->line;
    }
}

static void fb_render_status(const struct mesh_ui_backend_fb_state *state,
                             const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    fb_draw_title(state, layout, "Status");

    int y = layout->body_y;
    char line[160];
    const int adv = layout->line;

    snprintf(line, sizeof line, "Transport   %s",
             snapshot->transport_status[0] != '\0' ? snapshot->transport_status : "starting");
    fb_fit(line, layout->cols);
    fb_draw_text(state, FB_MARGIN, y, line, state->scale, k_text);
    y += adv;

    const struct mesh_ui_device *connected = NULL;
    for (size_t i = 0; i < snapshot->device_count; ++i) {
        if (snapshot->devices[i].connected) {
            connected = &snapshot->devices[i];
            break;
        }
    }
    snprintf(line, sizeof line, "Radio       %s",
             connected != NULL
                 ? (connected->name[0] != '\0' ? connected->name : connected->identifier)
                 : "not connected");
    fb_fit(line, layout->cols);
    fb_draw_text(state, FB_MARGIN, y, line, state->scale, connected != NULL ? k_good : k_bad);
    y += adv;

    if (snapshot->handshake_valid) {
        const struct mesh_ui_handshake_state *hs = &snapshot->handshake;
        snprintf(line, sizeof line, "Sync        %s%s",
                 hs->config_complete ? "complete"
                                     : (hs->request_in_flight ? "in progress" : "idle"),
                 hs->cached ? " (cached)" : "");
        fb_fit(line, layout->cols);
        fb_draw_text(state, FB_MARGIN, y, line, state->scale, k_text);
        y += adv;

        if (hs->has_my_info) {
            snprintf(line, sizeof line, "My node     %s !%08x", hs->my_short_name,
                     hs->my_info.node_num);
            fb_fit(line, layout->cols);
            fb_draw_text(state, FB_MARGIN, y, line, state->scale, k_text);
            y += adv;
            snprintf(line, sizeof line, "NodeDB      %u nodes, %u reboots",
                     hs->my_info.nodedb_entries, hs->my_info.reboot_count);
            fb_fit(line, layout->cols);
            fb_draw_text(state, FB_MARGIN, y, line, state->scale, k_text);
            y += adv;
        }
        if (hs->primary_channel[0] != '\0') {
            snprintf(line, sizeof line, "Channel     %s", hs->primary_channel);
            fb_fit(line, layout->cols);
            fb_draw_text(state, FB_MARGIN, y, line, state->scale, k_text);
            y += adv;
        }
    } else {
        fb_draw_text(state, FB_MARGIN, y, "Sync        waiting for a radio", state->scale, k_dim);
        y += adv;
    }

    snprintf(line, sizeof line, "Messages    %u kept, %u dropped",
             (unsigned)snapshot->messages.count, (unsigned)snapshot->messages.dropped);
    fb_fit(line, layout->cols);
    fb_draw_text(state, FB_MARGIN, y, line, state->scale, k_text);
    y += adv;

    snprintf(line, sizeof line, "Devices     %zu in range", snapshot->device_count);
    fb_fit(line, layout->cols);
    fb_draw_text(state, FB_MARGIN, y, line, state->scale, k_text);
    y += adv + adv / 2;

    fb_draw_text(state, FB_MARGIN, y, "Left/Right or L1/R1 switch tabs.", state->scale, k_dim);
    y += adv;
    fb_draw_text(state, FB_MARGIN, y, "Up/Down move, A selects, B backs out.", state->scale, k_dim);
    y += adv;
    fb_draw_text(state, FB_MARGIN, y, mesh_ui_input_quit_hint(), state->scale, k_dim);
}

static void fb_render_snapshot(struct mesh_ui_backend_fb_state *state,
                               const struct mesh_ui_snapshot *snapshot) {
    fb_clear(state, k_bg);

    struct fb_layout layout;
    memset(&layout, 0, sizeof layout);
    layout.small = state->scale > FB_MIN_SCALE ? state->scale - 1 : FB_MIN_SCALE;
    layout.line = fb_line_adv(state->scale);
    layout.cols = fb_cols(state, state->scale);

    fb_draw_tabs(state, snapshot, &layout);

    const int footer_height = 2 * fb_line_adv(layout.small) + FB_MARGIN;
    layout.footer_y = (int)state->var.yres - footer_height;
    const int body_height = layout.footer_y - layout.body_y - FB_MARGIN / 2;
    layout.rows = body_height > 0 ? (uint32_t)(body_height / layout.line) : 0U;

    const char *hint = "A select  B back  Left/Right tabs";
    switch (snapshot->nav.screen) {
    case MESH_UI_SCREEN_MESSAGES:
        hint = "A reply  Up/Down scroll  Left/Right tabs";
        fb_render_messages(state, snapshot, &layout);
        break;
    case MESH_UI_SCREEN_NODES:
        hint = "A message node  Up/Down scroll  Left/Right tabs";
        fb_render_nodes(state, snapshot, &layout);
        break;
    case MESH_UI_SCREEN_COMPOSE:
        hint = "A send / change To  B back  Left/Right tabs";
        fb_render_compose(state, snapshot, &layout);
        break;
    case MESH_UI_SCREEN_DEVICES:
        hint = "A connect  Up/Down scroll  Left/Right tabs";
        fb_render_devices(state, snapshot, &layout);
        break;
    case MESH_UI_SCREEN_STATUS:
    default:
        hint = "Left/Right tabs";
        fb_render_status(state, snapshot, &layout);
        break;
    }

    fb_draw_footer(state, snapshot, &layout, hint);
}

static int fb_scale_from_env(void) {
    const char *value = getenv("MESHCLIENT_FB_SCALE");
    if (value == NULL || value[0] == '\0') {
        return FB_DEFAULT_SCALE;
    }
    char *end = NULL;
    const long parsed = strtol(value, &end, 10);
    if (end == value || parsed < FB_MIN_SCALE || parsed > FB_MAX_SCALE) {
        mesh_log_warn("ui", "MESHCLIENT_FB_SCALE='%s' out of range %d..%d; using %d", value,
                      FB_MIN_SCALE, FB_MAX_SCALE, FB_DEFAULT_SCALE);
        return FB_DEFAULT_SCALE;
    }
    return (int)parsed;
}

static int mesh_ui_backend_fb_init(void **state_out, void *userdata) {
    struct mesh_ui_backend_fb_context *context = (struct mesh_ui_backend_fb_context *)userdata;
    if (context == NULL) {
        return -EINVAL;
    }

    static struct mesh_ui_backend_fb_state state_storage;
    struct mesh_ui_backend_fb_state *state = &state_storage;
    memset(state, 0, sizeof *state);
    state->scale = fb_scale_from_env();

    state->fb_fd = open("/dev/fb0", O_RDWR);
    if (state->fb_fd < 0) {
        mesh_log_warn("ui", "Failed to open /dev/fb0: %s", strerror(errno));
        return -errno;
    }

    if (ioctl(state->fb_fd, FBIOGET_FSCREENINFO, &state->fix) < 0) {
        mesh_log_warn("ui", "FBIOGET_FSCREENINFO failed: %s", strerror(errno));
        close(state->fb_fd);
        state->fb_fd = -1;
        return -errno;
    }

    if (ioctl(state->fb_fd, FBIOGET_VSCREENINFO, &state->var) < 0) {
        mesh_log_warn("ui", "FBIOGET_VSCREENINFO failed: %s", strerror(errno));
        close(state->fb_fd);
        state->fb_fd = -1;
        return -errno;
    }

    state->bytes_per_pixel = state->var.bits_per_pixel / 8;
    state->line_bytes = state->fix.line_length;
    state->fb_size = state->line_bytes * state->var.yres_virtual;
    state->fb_ptr = mmap(NULL, state->fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, state->fb_fd, 0);
    if (state->fb_ptr == MAP_FAILED) {
        mesh_log_warn("ui", "mmap on framebuffer failed: %s", strerror(errno));
        close(state->fb_fd);
        state->fb_fd = -1;
        return -errno;
    }

    mesh_log_info("ui", "Framebuffer UI backend active (%ux%u %u bpp, virtual %ux%u, offset %u,%u)",
                  state->var.xres, state->var.yres, state->var.bits_per_pixel,
                  state->var.xres_virtual, state->var.yres_virtual, state->var.xoffset,
                  state->var.yoffset);

    if (state_out != NULL) {
        *state_out = state;
    }
    return 0;
}

static void mesh_ui_backend_fb_shutdown(void *state_ptr, void *userdata) {
    struct mesh_ui_backend_fb_state *state = (struct mesh_ui_backend_fb_state *)state_ptr;
    if (state != NULL) {
        if (state->fb_ptr != NULL && state->fb_ptr != MAP_FAILED) {
            munmap(state->fb_ptr, state->fb_size);
            state->fb_ptr = NULL;
        }
        if (state->fb_fd >= 0) {
            close(state->fb_fd);
            state->fb_fd = -1;
        }
    }
    (void)userdata;
}

/*
 * The Brick's fb0 is 1024x16384: a stack of 768-row pages that NextUI's SDL flips between, and
 * the Allwinner display engine keeps showing whichever page SDL last presented (observed:
 * rows 768..1535, i.e. page 1) after the launcher hands over. Drawing at row 0 is then
 * invisible. Pan the display back to page 0 after each frame and, in case the driver ignores
 * the pan, mirror the frame into page 1 as well - a 3 MB copy per HUD update is nothing.
 */
static void fb_show_page0(struct mesh_ui_backend_fb_state *state) {
    const size_t page_bytes = (size_t)state->line_bytes * state->var.yres;
    if (state->var.yres_virtual >= 2U * state->var.yres && 2U * page_bytes <= state->fb_size) {
        memcpy(state->fb_ptr + page_bytes, state->fb_ptr, page_bytes);
    }

    struct fb_var_screeninfo var = state->var;
    var.xoffset = 0U;
    var.yoffset = 0U;
    if (ioctl(state->fb_fd, FBIOPAN_DISPLAY, &var) < 0) {
        if (!state->pan_failed_logged) {
            mesh_log_warn("ui", "FBIOPAN_DISPLAY failed: %s; relying on the mirrored page",
                          strerror(errno));
            state->pan_failed_logged = true;
        }
    }
}

static void mesh_ui_backend_fb_present(void *state_ptr, const struct mesh_ui_snapshot *snapshot,
                                       void *userdata) {
    struct mesh_ui_backend_fb_state *state = (struct mesh_ui_backend_fb_state *)state_ptr;
    (void)userdata;
    if (state == NULL || snapshot == NULL || state->fb_ptr == NULL) {
        return;
    }

    fb_render_snapshot(state, snapshot);
    fb_show_page0(state);
    msync(state->fb_ptr, state->fb_size, MS_ASYNC);
}

static const struct mesh_ui_backend k_fb_backend = {
    .name = "fb",
    .init = mesh_ui_backend_fb_init,
    .shutdown = mesh_ui_backend_fb_shutdown,
    .present = mesh_ui_backend_fb_present,
};

bool mesh_ui_backend_fb_is_available(void) { return access("/dev/fb0", R_OK | W_OK) == 0; }

const struct mesh_ui_backend *mesh_ui_backend_fb(void) { return &k_fb_backend; }
