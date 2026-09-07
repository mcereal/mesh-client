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

#include "mesh/ui/anim.h"
#include "mesh/ui/icon.h"
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
    /*
     * The clock this frame is drawn against, and what is still moving in it.
     *
     * A frame is otherwise a function of the snapshot alone, and a snapshot has no notion of
     * "was": it says a switch is on, never that it has just become on. These two are what a
     * widget that animates asks instead - the time, and where it had got to last frame, keyed
     * by an id the screen supplies. See include/mesh/ui/anim.h for why that lives here rather
     * than in the store.
     *
     * `now_ms` is set once per frame by whoever is driving: the monotonic clock on the device,
     * a number the scene script names in a capture. Nothing below reads a clock of its own, so
     * a capture renders the same frame every time it is asked for.
     */
    uint64_t now_ms;
    struct mesh_ui_anim_table anim;
    /*
     * The notice the snackbar is showing.
     *
     * The animation table remembers where the container has slid to; this remembers *what is
     * written on it*, and it is here for the second half of the same reason. A snackbar leaves
     * by sliding out, and by the time it does the store has already forgotten the text - the
     * nav clears an expired toast, which is what makes the next frame happen at all. Without a
     * copy the container would slide out blank, or more likely vanish on the frame it expired
     * and never slide at all.
     *
     * It is also what tells one notice from the next: a toast arriving while another is up is
     * a second arrival, not a text swap, and comparing against this is how the widget knows.
     */
    char snackbar[MESH_UI_NAV_TOAST_MAX];
    uint64_t snackbar_until_ms; /* the deadline that identifies it; see struct fb_snackbar */
};

/*
 * The clock for the next frame. Call before fb_render_snapshot().
 *
 * Time never goes backwards here: a caller that hands over an earlier reading than the last is
 * ignored, because an animation window that starts in the future never finishes and the knob
 * would stick.
 */
void fb_state_set_now(struct mesh_ui_backend_fb_state *state, uint64_t now_ms);

/* Whether anything on the last frame is still moving, and so whether another frame is owed.
   What the controller's repaint timer asks. */
bool fb_state_animating(const struct mesh_ui_backend_fb_state *state);

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
/*
 * A fill and the ink that goes on it, for one family, one slot and one interaction state.
 *
 * This is what a widget that *fills* something asks for, and fb_color() is what one that only
 * writes ink asks for. The difference matters: a fill and its label are a pair the theme was
 * validated as a pair, and every component that picked them up separately - the button, the
 * switch, the chat bubble - is a component that could be handed a combination nothing checked.
 */
struct mesh_ui_paint fb_paint(const struct mesh_ui_backend_fb_state *state,
                              enum mesh_ui_family family, enum mesh_ui_slot slot,
                              enum mesh_ui_state ui_state);
/* A stated fill with a state layer over it, for the neutral surfaces - which have no family to
   ask, but are still drawn under a cursor. `ink` is what the layer mixes in. */
struct mesh_ui_rgb fb_state_layer(const struct mesh_ui_backend_fb_state *state,
                                  enum mesh_ui_color fill, enum mesh_ui_color ink,
                                  enum mesh_ui_state ui_state);
/* Pixels between the panel edge and the body. */
int fb_margin(const struct mesh_ui_backend_fb_state *state);
/* The corner radius for a kind of container, at the frame's own glyph scale. The only way a
   radius enters the framebuffer layers, for the reason fb_color() is the only way a colour
   does; see enum mesh_ui_shape. */
int fb_radius(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_shape shape);

/* How long `motion` lasts on this state's theme, in milliseconds. The duration half of an
   animation; the curve is still named at the call site. */
uint32_t fb_motion(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_motion motion);
/* The hairline thickness an edge is drawn at - a card's, a field's. One place, because an
   outline is two fills and both have to agree about how thick it is. */
int fb_edge(const struct mesh_ui_backend_fb_state *state);
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
/* The box an icon is drawn in: one text cell, so a row that puts one in front of its words is
   still measured in columns like every other row. */
int fb_icon_box(const struct mesh_ui_backend_fb_state *state, int scale);
/* The largest multiplier fb_draw_icon() will draw at: everything in a row is drawn at the
   text's scale, and the empty state's symbol is the one thing bigger than that. */
#define FB_ICON_SCALE_MAX (MESH_UI_SCALE_MAX * 3)

/*
 * One icon, in `ink`, blended against `ground` - the colour the caller has just filled behind
 * it. Placed like a glyph: `x` is the cell's left edge and `y` the text baseline it lines up
 * with. MESH_UI_ICON_NONE draws nothing, so a slot that is empty needs no test.
 */
void fb_draw_icon(const struct mesh_ui_backend_fb_state *state, int x, int y,
                  enum mesh_ui_icon icon, int scale, struct mesh_ui_rgb ink,
                  struct mesh_ui_rgb ground);
int fb_draw_wrapped(const struct mesh_ui_backend_fb_state *state, int y, const char *text,
                    size_t cols, int max_lines, struct mesh_ui_rgb color);
/* The same from an explicit left edge, for text inset into a container rather than into the
   body - a dialog's supporting paragraph. */
int fb_draw_wrapped_at(const struct mesh_ui_backend_fb_state *state, int x, int y, const char *text,
                       size_t cols, int max_lines, struct mesh_ui_rgb color);
void fb_fill_rect(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int h,
                  struct mesh_ui_rgb color);
/*
 * The same box with its corners taken off, `radius` pixels each - the shape an avatar disc, a
 * count pill and a selected row are. A radius of half the shorter side is a circle (or a
 * capsule); anything larger is clamped to that, so a caller can ask for "as round as it goes"
 * without measuring first.
 *
 * There is no anti-aliasing: the panel is 1024 px across a 3.2" screen, so a stepped edge on a
 * 60 px disc is already below what the eye resolves, and blending would need a background this
 * function cannot see - a disc is drawn over the ground on one row and over the cursor fill on
 * the next.
 */
void fb_fill_round_rect(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int h,
                        int radius, struct mesh_ui_rgb color);
void fb_fit(char *line, size_t cols);
void fb_format_age(uint32_t last_heard, char *out, size_t out_len);
void fb_format_clock(uint32_t rx_time, char *out, size_t out_len);
size_t fb_width(const char *line);

/* ---- fb_screens.c ------------------------------------------------------------------------ */

/* Draws one whole frame: chrome, then whichever screen the snapshot says is up. */
void fb_render_snapshot(struct mesh_ui_backend_fb_state *state,
                        const struct mesh_ui_snapshot *snapshot);

#endif /* MESH_UI_BACKENDS_FB_INTERNAL_H */
