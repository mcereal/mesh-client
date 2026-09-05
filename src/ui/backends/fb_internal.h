#ifndef MESH_UI_BACKENDS_FB_INTERNAL_H
#define MESH_UI_BACKENDS_FB_INTERNAL_H

/*
 * The seams inside the framebuffer backend.
 *
 * fb.c was 1700 lines spanning three layers that stack cleanly:
 *
 *   fb_draw.c     pixels, glyphs, rows, the palette and the page geometry
 *   fb_screens.c  one renderer per screen, drawn out of a snapshot
 *   fb.c          opening /dev/fb0, the page flip, the backend vtable
 *
 * Calls only ever go downward, so this header is the drawing toolkit the layer above uses. It
 * is not public - include/mesh/ui/backends/fb.h is - and nothing here should be reached for
 * outside src/ui/backends/.
 */

#include "mesh/ui/store.h"

#include <linux/fb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Palette. Dark ground, cool greys for chrome, one warm colour for things that need the eye.
   Defined in fb_draw.c. */
extern const struct fb_rgb k_fb_bg;
extern const struct fb_rgb k_fb_text;
extern const struct fb_rgb k_fb_dim;
extern const struct fb_rgb k_fb_tab_active_bg;
extern const struct fb_rgb k_fb_cursor_bg;
extern const struct fb_rgb k_fb_white;
extern const struct fb_rgb k_fb_inbound;
extern const struct fb_rgb k_fb_outbound;
extern const struct fb_rgb k_fb_accent;
extern const struct fb_rgb k_fb_good;
extern const struct fb_rgb k_fb_bad;

#define FB_MARGIN 16
#define FB_DEFAULT_SCALE 4
#define FB_MIN_SCALE 2
#define FB_MAX_SCALE 6

/* Where the chrome ends and the body begins, in cells. Filled by fb_draw_tabs(). */
struct fb_layout {
    int body_y;    /* first body row */
    int footer_y;  /* top of the two footer lines */
    int line;      /* body line advance */
    uint32_t rows; /* body rows available */
    size_t cols;   /* body columns */
    int small;     /* scale for chrome text */
};

/* ---- fb_draw.c: the drawing toolkit ------------------------------------------------------ */

int fb_char_adv(int scale);
int fb_line_adv(int scale);
void fb_clear(const struct mesh_ui_backend_fb_state *state, struct fb_rgb color);
size_t fb_cols(const struct mesh_ui_backend_fb_state *state, int scale);
void fb_draw_empty(const struct mesh_ui_backend_fb_state *state, const struct fb_layout *layout, const char *text);
void fb_draw_footer(const struct mesh_ui_backend_fb_state *state, const struct mesh_ui_snapshot *snapshot, const struct fb_layout *layout, const char *hint);
void fb_draw_glyph(const struct mesh_ui_backend_fb_state *state, int x, int y, uint32_t codepoint, int scale, struct fb_rgb color);
void fb_draw_row(const struct mesh_ui_backend_fb_state *state, int y, const char *text, struct fb_rgb color, bool selected);
void fb_draw_tabs(const struct mesh_ui_backend_fb_state *state, const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout);
void fb_draw_text(const struct mesh_ui_backend_fb_state *state, int x, int y, const char *text, int scale, struct fb_rgb color);
void fb_draw_title(const struct mesh_ui_backend_fb_state *state, struct fb_layout *layout, const char *title);
int fb_draw_wrapped(const struct mesh_ui_backend_fb_state *state, int y, const char *text, size_t cols, int max_lines, struct fb_rgb color);
void fb_fill_rect(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int h, struct fb_rgb color);
uint32_t fb_first_visible(uint32_t cursor, uint32_t count, uint32_t visible);
void fb_fit(char *line, size_t cols);
void fb_format_age(uint32_t last_heard, char *out, size_t out_len);
void fb_format_clock(uint32_t rx_time, char *out, size_t out_len);
size_t fb_width(const char *line);

/* ---- fb_screens.c ------------------------------------------------------------------------ */

/* Draws one whole frame: chrome, then whichever screen the snapshot says is up. */
void fb_render_snapshot(struct mesh_ui_backend_fb_state *state, const struct mesh_ui_snapshot *snapshot);

#endif /* MESH_UI_BACKENDS_FB_INTERNAL_H */
