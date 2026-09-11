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

#include "mesh/map/source.h"
#include "mesh/map/tile_cache.h"
#include "mesh/ui/anim.h"
#include "mesh/ui/icon.h"
#include "mesh/ui/route.h"
#include "mesh/ui/store.h"
#include "mesh/ui/theme.h"

#include <linux/fb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fb_glyph_cache;
struct fb_thread_cache;
struct fb_render_cache;

/*
 * The pictures under the map: a tile pack, the tiles decoded out of it, and the buffer one
 * tile's bytes are read into.
 *
 * It hangs off the backend rather than off the store, and that is the same boundary the rest of
 * the map already draws. A backend is handed a `const` snapshot: it cannot write a tile back
 * into the nav, and the nav must not learn how wide this panel's body is - so which tiles are
 * wanted is a question only the thing that measured the body can ask, and what it gets back is
 * pixels in a format only a thing that knows the panel can use. Both halves live here.
 *
 * Allocated on open, so a client with no pack pays one pointer. mesh/map/ is what is inside
 * it; src/ui/backends/fb_map.c is the only file that touches these fields.
 */
struct fb_basemap {
    struct mesh_map_source source;
    struct mesh_map_tile_cache cache;
    /* One tile's encoded bytes. A caller's local in the flag that describes a pack, and a
       long-lived block here, because the fill loop reads one on the frame path and a megabyte
       is not a thing to put on the stack under a renderer. */
    uint8_t *encoded;
    bool open;
    /*
     * Whether the last frame wanted a tile it did not have.
     *
     * This is what asks for the next frame: a frame is otherwise a function of the snapshot,
     * and no press and no packet says that a tile is still on its way. One tile is read per
     * frame (docs/maps-roadmap.md's one decode per turn of the event loop), so a full view
     * fills over about twenty frames with input serviced between them rather than in one stall
     * the length of a dropped connection.
     */
    bool pending;
};

struct fb_damage_rect {
    int x, y, right, bottom;
    bool valid;
};

struct mesh_ui_backend_fb_state {
    struct fb_glyph_cache *glyph_cache;
    struct fb_thread_cache *thread_cache;
    struct fb_render_cache *render_cache;
    /* NULL until a pack is opened, which is every run that was not pointed at one. */
    struct fb_basemap *basemap;
    bool partial_disabled;
    bool clip_active;
    struct fb_damage_rect clip;
    struct fb_damage_rect animation_damage;
    bool thread_cache_disabled;
    uint8_t *draw_buffer;
    uint8_t *previous_frame;
    bool frame_valid;
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
    /*
     * The place the last frame was drawn for, and how far the current one has slid into view.
     *
     * This is the snackbar's problem one level up, and it is here for the same reason: a
     * snapshot says where the user *is*, never that they have just arrived, so the only thing
     * that can tell an entrance from an exit is something that remembers the previous frame.
     * The backend is where that legitimately lives - see the note at the top of anim.h - and
     * mesh/ui/route.h is where the comparison itself lives, so a second backend that wants to
     * animate differently, or not at all, asks the same question and answers it its own way.
     *
     * `slide` runs 0 -> ONE as the arriving screen travels the last of the panel's width;
     * `slide_dir` is the side it came from, +1 for the right and -1 for the left, and 0 when
     * nothing is travelling. Not a slot in the animation table because that table is for
     * widgets with nowhere of their own to keep a position, and the frame is not one of those.
     */
    struct mesh_ui_route route;
    bool route_valid;
    struct mesh_ui_anim slide;
    int slide_dir;
    /*
     * The frame's content transform: what fb_shift_begin() has moved the body by, and the band
     * it is confined to while it is moved. See fb_shift_begin().
     */
    int shift_x;
    int shift_top;
    int shift_bottom;
    bool shift_active;
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
 * The move this frame is part of, as the distance the arriving screen still has to travel: a
 * positive offset for one coming in from the right, negative from the left, 0 for a frame that
 * is not moving. Reads the nav's *place* (mesh/ui/route.h) against the one the last frame was
 * drawn for, so nothing outside this backend has to record how it got here.
 *
 * Call once per frame, before fb_shift_begin(): it is what advances the remembered place.
 */
int fb_transition_offset(struct mesh_ui_backend_fb_state *state, const struct mesh_ui_nav *nav);

/*
 * Slides everything drawn until fb_shift_end() by `dx` pixels, clipped to rows [top, bottom).
 *
 * The one transform in the drawing layer, and the only thing here that knows content can come
 * from off the panel. It is a single call rather than an offset threaded through the widgets
 * because a component that took one would be a component with a pixel coordinate in it: a
 * screen renderer describes its content and a widget places it against `struct fb_layout`, and
 * neither has any business knowing the frame is mid-transition. Every pixel this backend writes
 * goes through fb_fill_packed(), so putting it there covers glyphs, icons, emoji, fills and
 * rounded corners at once - and covers anything added later without being told to.
 *
 * The band is what keeps a transition to the part of the frame that changed: the navigation bar
 * and the action bar are the same on both sides of a move, and chrome that slid with the body
 * would be the client claiming the whole application had been replaced. It is stated in rows
 * rather than derived from the layout because the caller is the only thing that knows where the
 * body it is about to draw begins and ends.
 *
 * Not nestable, deliberately: there is one transform per frame and a second would be a second
 * opinion about where the body is.
 */
void fb_shift_begin(struct mesh_ui_backend_fb_state *state, int dx, int top, int bottom);
void fb_shift_end(struct mesh_ui_backend_fb_state *state);

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

/* The gap `space` asks for, in pixels, at the body scale. */
int fb_space(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_space space);

/* The same gap at an explicit glyph multiplier, for the widgets that are drawn at one that is
   not the body's - a rule under the tab strip, a switch on a chrome-scale row. A gap beside
   smaller text has to be smaller too, or the scale stops being a scale. */
int fb_space_at(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_space space, int scale);

/* The glyph multiplier `type` is drawn at, given this state's body scale. */
int fb_type_scale(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_type type);

/*
 * The half-margin: the inset a panel sits in, and the gutter the scroll rail lives in.
 *
 * Not part of the spacing scale, deliberately. The spacing scale is glyph-relative - it is the
 * room around *text* - and this tracks the body margin instead, because what it measures is how
 * far a panel is from the edge of the screen. A theme that asks for bigger text wants roomier
 * padding and the same inset; one that asks for a roomier margin wants the opposite.
 */
int fb_gutter(const struct mesh_ui_backend_fb_state *state);

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
    /* The glyph multiplier chrome is drawn at: MESH_UI_TYPE_LABEL, resolved once in
       fb_render_snapshot() and carried here so every piece of chrome in the frame agrees. */
    int small;
    /*
     * The first pixel below the navigation bar, its closing rule included.
     *
     * Not the same number as `body_y`, and that is the point: between the two there is the gap
     * the bar leaves before the body starts, and the screen progress bar hangs in it. A caller
     * that derived it from `body_y` would be subtracting a gap the navigation bar chose, which
     * is the navigation bar's arithmetic written out a second time - and `body_y` has moved on
     * by then anyway, because the app bar and the banner both advance it.
     */
    int nav_y;
    /*
     * Whether there is a screen behind this one to go back to, from mesh_ui_action_bar_goes_
     * back() - the top app bar's leading slot.
     *
     * Here rather than on `struct fb_app_bar` because a screen renderer is the wrong place to
     * be asked: it is a fact about the nav, the tables in src/ui/actions.c already decide it
     * for the action bar at the bottom, and the two pieces of chrome disagreeing about whether
     * B leaves is exactly the drift a second opinion would introduce. fb_render_snapshot()
     * asks once and both bars read the same answer.
     */
    bool back;
};

/* Copies changed row spans from ordinary RAM into page 0 and its display mirror.
   Returns bytes written across both pages; force initializes pages owned by the launcher. */
size_t fb_copy_damage(struct mesh_ui_backend_fb_state *state, const uint8_t *frame,
                      uint8_t *previous, bool force);
void fb_render_cache_free(struct mesh_ui_backend_fb_state *state);
void fb_animation_damage(struct mesh_ui_backend_fb_state *state, int x, int y, int w, int h);
void fb_thread_cache_free(struct mesh_ui_backend_fb_state *state);
void fb_glyph_cache_free(struct mesh_ui_backend_fb_state *state);

/* ---- fb_draw.c: the drawing toolkit ------------------------------------------------------ */

/* Glyph metrics for a multiplier, from the theme's font. */
int fb_char_adv(const struct mesh_ui_backend_fb_state *state, int scale);
int fb_line_adv(const struct mesh_ui_backend_fb_state *state, int scale);
void fb_clear(const struct mesh_ui_backend_fb_state *state, struct mesh_ui_rgb color);
size_t fb_cols(const struct mesh_ui_backend_fb_state *state, int scale);
/*
 * Text and one glyph of it, in `ink` over `ground`.
 *
 * `ground` is the colour the caller has just filled behind the text, and it is a parameter for
 * the same reason fb_draw_icon()'s is: a glyph carries coverage, not a mask, and blending its
 * edges needs to know what they are blending into. What is already on the panel is not
 * readable from here, and a caller that has just filled a row is the only thing that knows
 * what colour it filled it with. Getting it wrong does not lose the text - it puts a faint
 * halo of the wrong colour around it.
 */
void fb_draw_glyph(const struct mesh_ui_backend_fb_state *state, int x, int y, uint32_t codepoint,
                   int scale, struct mesh_ui_rgb ink, struct mesh_ui_rgb ground);
/* A whole row from the body margin, drawing its own cursor fill - so it knows its own ground
   and does not take one. */
/*
 * The fill a selected row lays down, and the ground everything on that row is then drawn
 * against - the background colour when the row is not the cursor's.
 *
 * Shared rather than written out per row shape because a list mixes them: a plain row and a
 * section heading in the same list highlighting to two slightly different rectangles is a
 * cursor that changes shape as it walks, which is exactly what a duplicated `y - scale` and a
 * duplicated height produced. `rows` is how many body rows the row occupies.
 */
struct mesh_ui_rgb fb_draw_row_fill(const struct mesh_ui_backend_fb_state *state, int y,
                                    uint32_t rows, bool selected);

void fb_draw_row(const struct mesh_ui_backend_fb_state *state, int y, const char *text,
                 struct mesh_ui_rgb color, bool selected);
void fb_draw_text(const struct mesh_ui_backend_fb_state *state, int x, int y, const char *text,
                  int scale, struct mesh_ui_rgb ink, struct mesh_ui_rgb ground);
/* The box an icon is drawn in: one text cell, so a row that puts one in front of its words is
   still measured in columns like every other row. */
int fb_icon_box(const struct mesh_ui_backend_fb_state *state, int scale);
/* What an icon is actually *drawn* at, which is a little wider than the cell it occupies - a
   symbol has to stand as tall as the capitals beside it, and the advance is narrower than the
   glyph body is tall. Beside fb_icon_box() because it answers the other half of "how big is an
   icon": the cell is what the column arithmetic counts, this is what a component fitting one
   inside a box of its own - a checkbox's tick - has to measure against. */
int fb_icon_drawn(const struct mesh_ui_backend_fb_state *state, int scale);
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
                    size_t cols, int max_lines, struct mesh_ui_rgb color,
                    struct mesh_ui_rgb ground);
/* The same from an explicit left edge, for text inset into a container rather than into the
   body - a dialog's supporting paragraph. */
int fb_draw_wrapped_at(const struct mesh_ui_backend_fb_state *state, int x, int y, const char *text,
                       size_t cols, int max_lines, struct mesh_ui_rgb color,
                       struct mesh_ui_rgb ground);
void fb_fill_rect(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int h,
                  struct mesh_ui_rgb color);
/*
 * A rectangle of BGRA pixels - what mesh_map_tile_decode() produces - drawn at `x`, `y`.
 *
 * `stride` is the source's row length in bytes, so a caller may hand over part of a larger
 * image. Clipped by the same rules every fill in this backend is, which is what puts a map tile
 * inside the map's own body rather than over the app bar above it.
 */
void fb_blit_bgra(const struct mesh_ui_backend_fb_state *state, int x, int y, int w, int h,
                  const uint8_t *pixels, size_t stride);
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

/* ---- fb_map.c ----------------------------------------------------------------------------- */

/* The map over the node list: the graticule, the markers and what the crosshair is on. Its own
   file because it is the one screen that places things at coordinates rather than describing
   rows - see the paragraph at the top of it. */
void fb_render_map(struct mesh_ui_backend_fb_state *state, const struct mesh_ui_snapshot *snapshot,
                   struct fb_layout *layout);

/*
 * Opens the tile pack at `path` and hangs it off the state. 0, or -errno from the pack reader.
 *
 * A second open closes the first and **clears the cache with it**: a key is three numbers about
 * the world rather than about a file, so two packs of the same city hold different pictures at
 * the same key - carried across a swap, the map draws the old pack's streets under the new
 * pack's attribution and nothing on the frame looks wrong.
 */
int fb_basemap_open(struct mesh_ui_backend_fb_state *state, const char *path);

/*
 * Opens whatever pack this device has, if any: MESHCLIENT_MAP_PACK when it is set, otherwise
 * the conventional file a sideload lands at. Missing is the ordinary case and is not an error -
 * the map draws its graticule and says nothing.
 *
 * The device backend calls this and the capture harness deliberately does not: a scene names
 * its pack, because a frame that quietly picked up whatever pack the developer had installed
 * would render differently on two machines.
 */
void fb_basemap_open_default(struct mesh_ui_backend_fb_state *state);

/* Closes the pack, releases the cache and the read buffer, and leaves the state with no
   basemap. Safe on a state that never opened one. */
void fb_basemap_close(struct mesh_ui_backend_fb_state *state);

/* Whether the frame just drawn wanted a tile it did not have - what fb_state_animating() adds
   to the animations when it decides whether another frame is owed. */
bool fb_basemap_pending(const struct mesh_ui_backend_fb_state *state);

/* Clears that, so a frame drawing any other screen stops the map asking for the next one. Called
   once per frame by fb_render_snapshot(), before anything is drawn. */
void fb_basemap_frame_begin(struct mesh_ui_backend_fb_state *state);

/* ---- fb_screens.c ------------------------------------------------------------------------ */

/* Draws one whole frame: chrome, then whichever screen the snapshot says is up. */
void fb_render_snapshot(struct mesh_ui_backend_fb_state *state,
                        const struct mesh_ui_snapshot *snapshot);

#endif /* MESH_UI_BACKENDS_FB_INTERNAL_H */
