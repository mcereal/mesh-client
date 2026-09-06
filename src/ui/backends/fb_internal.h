#ifndef MESH_UI_BACKENDS_FB_INTERNAL_H
#define MESH_UI_BACKENDS_FB_INTERNAL_H

/*
 * The seams inside the framebuffer backend.
 *
 * fb.c was 1700 lines spanning layers that stack cleanly:
 *
 *   fb_draw.c     pixels, glyphs, rows, the palette and the page geometry
 *   fb_widgets.c  the components screens are assembled from (fb_widgets.h)
 *   fb_screens.c  one renderer per screen, drawn out of a snapshot
 *   fb.c          opening /dev/fb0, the page flip, the backend vtable
 *
 * Calls only ever go downward, so this header is the drawing toolkit the layers above use, and
 * fb_widgets.h is the component set above that. Neither is public - include/mesh/ui/backends/fb.h
 * is - and nothing here should be reached for outside src/ui/backends/.
 *
 * Only fb_draw.c's own primitives live here. Anything that composes several of them into a
 * thing with a name - a button, a list, a field row - belongs in fb_widgets.h.
 *
 * No colour, margin or glyph size is spelled out below this comment. They come from the theme
 * on the state (include/mesh/ui/theme.h) through the accessors on it, which is what lets one
 * table swap the whole look.
 */

#include "mesh/ui/store.h"
#include "mesh/ui/theme.h"

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
    /* What this frame is drawn with: the palette, the metrics and the font. Never NULL once
       fb_state_set_theme() has run, and every accessor below falls back to the default anyway,
       so no drawing function guards it. */
    const struct mesh_ui_theme *theme;
    /* Glyph multiplier for body text; the tab bar and footer use one step smaller. The Brick's
       3.2" panel is 1024 px wide, so 4 gives ~41 columns of legible text. It starts at the
       theme's own and is overridden by MESHCLIENT_FB_SCALE. */
    int scale;
    /* Somebody named this multiplier outright - MESHCLIENT_FB_SCALE on the device, an explicit
       scale through the capture API - so a theme arriving in a snapshot keeps it rather than
       swapping in that theme's default. */
    bool scale_pinned;
};

/*
 * Adopts the theme the snapshot names, when it names one this build knows and is not already
 * drawing. Returns true when the frame's look changed.
 *
 * This is how a theme switch reaches the panel: the app publishes the choice in the client
 * info and the backend picks it up on the next frame, rather than anything reaching in here to
 * push at it. Backends stay stateless in the way that matters - what is on screen is a
 * function of the snapshot.
 */
bool fb_state_follow_snapshot(struct mesh_ui_backend_fb_state *state,
                              const struct mesh_ui_snapshot *snapshot);

/* Sets the theme and takes the scale from it. Pass 0 for `scale` to accept the theme's. */
void fb_state_set_theme(struct mesh_ui_backend_fb_state *state, const struct mesh_ui_theme *theme,
                        int scale);

/* ---- the theme, as the drawing layers ask for it ------------------------------------------ */

/* A colour by role. This is the only way a colour enters the framebuffer layers. */
struct mesh_ui_rgb fb_color(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_color role);
/* A colour by what the content means. What screens use; see enum mesh_ui_tone. */
struct mesh_ui_rgb fb_tone_color(const struct mesh_ui_backend_fb_state *state,
                                 enum mesh_ui_tone tone);
/* Pixels between the panel edge and the body. */
int fb_margin(const struct mesh_ui_backend_fb_state *state);
const struct mesh_ui_metrics *fb_metrics(const struct mesh_ui_backend_fb_state *state);
const struct mesh_ui_font *fb_font(const struct mesh_ui_backend_fb_state *state);

/* Where the chrome ends and the body begins. Filled in by fb_render_snapshot(). */
struct fb_layout {
    int body_y;    /* first body row */
    int footer_y;  /* top of the two footer lines */
    int line;      /* body line advance */
    uint32_t rows; /* body rows available */
    size_t cols;   /* body columns */
    int small;     /* scale for chrome text */
};

/* ---- fb_draw.c: the drawing toolkit ------------------------------------------------------ */

/* Glyph metrics for a multiplier, from the theme's font. */
int fb_char_adv(const struct mesh_ui_backend_fb_state *state, int scale);
int fb_line_adv(const struct mesh_ui_backend_fb_state *state, int scale);
void fb_clear(const struct mesh_ui_backend_fb_state *state, struct mesh_ui_rgb color);
size_t fb_cols(const struct mesh_ui_backend_fb_state *state, int scale);
void fb_draw_glyph(const struct mesh_ui_backend_fb_state *state, int x, int y, uint32_t codepoint,
                   int scale, struct mesh_ui_rgb color);
void fb_draw_row(const struct mesh_ui_backend_fb_state *state, int y, const char *text,
                 struct mesh_ui_rgb color, bool selected);
void fb_draw_text(const struct mesh_ui_backend_fb_state *state, int x, int y, const char *text,
                  int scale, struct mesh_ui_rgb color);
int fb_draw_wrapped(const struct mesh_ui_backend_fb_state *state, int y, const char *text,
                    size_t cols, int max_lines, struct mesh_ui_rgb color);
void fb_fill_rect(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int h,
                  struct mesh_ui_rgb color);
void fb_fit(char *line, size_t cols);
void fb_format_age(uint32_t last_heard, char *out, size_t out_len);
void fb_format_clock(uint32_t rx_time, char *out, size_t out_len);
size_t fb_width(const char *line);

/* ---- fb_screens.c ------------------------------------------------------------------------ */

/* Draws one whole frame: chrome, then whichever screen the snapshot says is up. */
void fb_render_snapshot(struct mesh_ui_backend_fb_state *state,
                        const struct mesh_ui_snapshot *snapshot);

#endif /* MESH_UI_BACKENDS_FB_INTERNAL_H */
