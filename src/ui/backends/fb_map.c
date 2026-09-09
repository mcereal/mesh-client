#define _POSIX_C_SOURCE 200809L

/*
 * The map, drawn.
 *
 * A file of its own rather than a renderer in fb_screens.c, and that is not a size decision: it
 * is the one screen in the client that is not a list. Everything in fb_screens.c describes rows
 * and hands them to a component; this places things at coordinates, because that is what a map
 * is. Keeping it apart is what stops "a screen renderer never computes a pixel" - which is true
 * of every other screen in that file - from becoming a rule with an exception buried in it.
 *
 * What it still does not do is decide anything. Which markers exist is src/ui/map.c, where the
 * viewport is looking is the nav, which marker is selected is mesh_ui_map_selected(), and every
 * colour, radius and margin comes from the theme. This file is placement and ink.
 */

#include "fb_widgets.h"

#include "mesh/geo/coords.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/map.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/waypoints.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <string.h>

/*
 * The graticule's steps, in fixed-point 1e-7 degrees.
 *
 * A 1-2-5 ladder, which is the sequence every ruler and every chart axis is divided by: each
 * step is a round number in the units the reader is being asked to think in, and the ratios
 * between neighbours are small enough that a zoom never has to jump two rungs to find a
 * spacing that fits. Ten degrees at the top, because at zoom 0 the whole world is 256 pixels
 * across and anything finer is a grey field; a thousandth of a degree at the bottom, which is
 * about a hundred metres and finer than any fix on a mesh is reported to.
 */
static const int32_t k_graticule_steps[] = {
    1000,   2000,    5000,    10000,   20000,    50000,    100000,   200000,
    500000, 1000000, 2000000, 5000000, 10000000, 20000000, 50000000, 100000000,
};

/* The narrowest a graticule cell may be drawn. Below this the grid stops being a scale and
   starts being a texture - and a texture behind markers is what makes a marker hard to find. */
#define FB_MAP_GRID_MIN_PX 70

/* How many labels the map will draw. Past this the panel is a wall of names with the markers
   hidden behind them, and the ones that survive are the ones the roster ranked highest - which
   is the order the list the reader came from is in. */
#define FB_MAP_LABELS_MAX 24

/* How many rectangles the collision test will remember: every visible marker, and then the
   labels placed around them. Past this a marker stops blocking a name, which draws a name over
   a dot - the mildest way for a crowded panel to run out of bookkeeping, and the reason the
   number is a cap rather than a failure. */
#define FB_MAP_BOXES_MAX 64

/* A rectangle already spoken for, so the next label can be asked to stand down rather than
   drawn over. */
struct fb_map_box {
    int x;
    int y;
    int w;
    int h;
};

static bool fb_map_boxes_overlap(const struct fb_map_box *a, const struct fb_map_box *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

/*
 * The map's artwork is clipped to the map's own body, and this is what does it.
 *
 * `visible` is a statement about a marker's *centre*, which is all a placement can honestly say -
 * but a marker is not a point when it is drawn. A rounded-position footprint can be a hundred
 * pixels across and a label is a run of text, so a marker whose centre is a pixel inside the top
 * edge paints most of itself over the app bar. Testing the centre and drawing a shape is the
 * whole of that bug, and no amount of care at the call sites fixes it: the shapes have different
 * extents and two of them are drawn by components that do not take a bounding box.
 *
 * So the clip goes on the state, where fb_fill_packed() already honours one - every fill, glyph
 * and icon span in this backend goes through that one function, so a rectangle set here covers
 * the discs, the pins and the names alike.
 *
 * It *intersects* rather than replaces, and that is not defensive: fb_render_snapshot() sets a
 * clip of its own for the partial-redraw path, and a map that overwrote it would repaint rows
 * outside the damage the frame had promised to touch.
 */
struct fb_map_clip {
    bool active;
    struct fb_damage_rect rect;
};

static void fb_map_clip_push(struct mesh_ui_backend_fb_state *state, const struct fb_map_box *body,
                             struct fb_map_clip *saved) {
    saved->active = state->clip_active;
    saved->rect = state->clip;

    struct fb_damage_rect wanted = {
        .x = body->x,
        .y = body->y,
        .right = body->x + body->w,
        .bottom = body->y + body->h,
        .valid = true,
    };
    if (state->clip_active) {
        if (state->clip.x > wanted.x) {
            wanted.x = state->clip.x;
        }
        if (state->clip.y > wanted.y) {
            wanted.y = state->clip.y;
        }
        if (state->clip.right < wanted.right) {
            wanted.right = state->clip.right;
        }
        if (state->clip.bottom < wanted.bottom) {
            wanted.bottom = state->clip.bottom;
        }
    }
    state->clip = wanted;
    state->clip_active = true;
}

static void fb_map_clip_pop(struct mesh_ui_backend_fb_state *state,
                            const struct fb_map_clip *saved) {
    state->clip_active = saved->active;
    state->clip = saved->rect;
}

/*
 * How big a marker is drawn, in pixels.
 *
 * Off the cap height rather than a constant, so it stays in proportion to the names beside it
 * at every glyph scale the theme can be asked for - the same reason an icon in a row slot is
 * measured against mesh_ui_font_cap() rather than against the cell.
 */
static int fb_map_marker_radius(const struct mesh_ui_backend_fb_state *state) {
    const int radius = mesh_ui_font_cap(fb_font(state), state->scale) / 3;
    return radius < 3 ? 3 : radius;
}

/* A filled disc, which is the one shape this file draws that fb_widgets.h has no name for: a
   rounded rectangle whose radius is half its shorter side is a circle, and fb_fill_round_rect()
   already clamps anything larger to exactly that. */
static void fb_map_disc(const struct mesh_ui_backend_fb_state *state, int cx, int cy, int radius,
                        struct mesh_ui_rgb color) {
    if (radius <= 0) {
        return;
    }
    fb_fill_round_rect(state, cx - radius, cy - radius, radius * 2, radius * 2, radius, color);
}

/*
 * The graticule step that gives cells at least FB_MAP_GRID_MIN_PX across.
 *
 * Longitude decides it for both axes. Longitude is linear in Mercator and latitude is not, so a
 * step chosen against latitude would change as the view moved north - a grid that breathed as
 * the reader panned, which reads as a fault rather than as a scale.
 */
static int32_t fb_map_grid_step(const struct mesh_map_viewport *viewport) {
    const double world = (double)MESH_MAP_TILE_SIZE * (double)(1UL << viewport->zoom);
    for (size_t i = 0; i < sizeof k_graticule_steps / sizeof k_graticule_steps[0]; ++i) {
        const double pixels = ((double)k_graticule_steps[i] / 3600000000.0) * world;
        if (pixels >= (double)FB_MAP_GRID_MIN_PX) {
            return k_graticule_steps[i];
        }
    }
    return k_graticule_steps[(sizeof k_graticule_steps / sizeof k_graticule_steps[0]) - 1U];
}

/* Rounds a coordinate down to the graticule below it - where the first line inside the view is.
   Integer division truncates towards zero, so the southern and western hemispheres need the
   extra step or every line there lands one cell inside the view. */
static int32_t fb_map_grid_floor(int32_t value, int32_t step) {
    int32_t floored = (value / step) * step;
    if (value < 0 && floored != value) {
        floored -= step;
    }
    return floored;
}

/*
 * The grid, which is the whole of the basemap for now.
 *
 * It is drawn rather than left blank because a map with nothing under it gives a reader no way
 * to judge that a pan moved anything: two markers on a plain field look the same however far
 * the world has slid under them. Lines of latitude and longitude are the honest thing to fill
 * that with - they are real, they are what the scale bar measures against, and they are the one
 * thing a client with no tiles is entitled to draw.
 */
static void fb_map_draw_grid(const struct mesh_ui_backend_fb_state *state,
                             const struct mesh_map_viewport *viewport,
                             const struct fb_map_box *body, struct mesh_ui_rgb ink) {
    const int32_t step = fb_map_grid_step(viewport);
    const int thickness = fb_rule_height(state, state->scale);

    /* What the corners of the body are looking at, which is what says which lines are inside
       it. Asked of the viewport rather than derived here: the wrap and the polar clamp are
       geo's business and this must not hold a second opinion about either. */
    int32_t north = 0;
    int32_t west = 0;
    int32_t south = 0;
    int32_t east = 0;
    mesh_map_viewport_at(viewport, 0, 0, &north, &west);
    mesh_map_viewport_at(viewport, body->w - 1, body->h - 1, &south, &east);

    /*
     * The meridians. Walked from the western edge in whole steps rather than by incrementing a
     * longitude, so a view straddling the antimeridian - where `east` is numerically smaller
     * than `west` - is walked the same way as any other: the loop counts cells across the
     * panel, and each one's position comes from placing its own coordinate.
     */
    const int32_t first_lon = fb_map_grid_floor(west, step);
    for (int cell = 0; cell <= (body->w / FB_MAP_GRID_MIN_PX) + 2; ++cell) {
        const int64_t raw = (int64_t)first_lon + (int64_t)cell * (int64_t)step;
        struct mesh_map_placement placement;
        if (!mesh_map_viewport_place(viewport, viewport->center_latitude_i,
                                     mesh_geo_longitude_wrap_i(raw), &placement)) {
            continue;
        }
        if (placement.x < 0 || placement.x >= body->w) {
            continue;
        }
        fb_fill_rect(state, body->x + placement.x, body->y, thickness, body->h, ink);
    }

    /* The parallels. Latitude does not wrap, so these are walked as coordinates and stopped at
       the southern edge - and clamped at the poles, where there is nothing further to draw. */
    for (int64_t lat = fb_map_grid_floor(north, step) + step; lat >= (int64_t)south; lat -= step) {
        if (lat > MESH_GEO_LATITUDE_I_MAX || lat < -MESH_GEO_LATITUDE_I_MAX) {
            continue;
        }
        struct mesh_map_placement placement;
        if (!mesh_map_viewport_place(viewport, (int32_t)lat, viewport->center_longitude_i,
                                     &placement)) {
            continue;
        }
        if (placement.y < 0 || placement.y >= body->h) {
            continue;
        }
        fb_fill_rect(state, body->x, body->y + placement.y, body->w, thickness, ink);
    }
}

/*
 * The scale bar: a round distance, and how long that is on this panel.
 *
 * The one thing on the frame that makes the grid readable as a measurement rather than as
 * decoration. The distance is chosen off the same 1-2-5 ladder the grid is, for the same
 * reason - a bar that read "137 m" would be a bar nobody could estimate against.
 */
static void fb_map_draw_scale(const struct mesh_ui_backend_fb_state *state,
                              const struct mesh_map_viewport *viewport,
                              const struct fb_map_box *body, bool imperial, int scale) {
    const double metres_per_pixel = mesh_map_viewport_metres_per_pixel(viewport);
    if (metres_per_pixel <= 0.0) {
        return;
    }

    /* A quarter of the body, which is long enough to be a ruler and short enough to leave the
       markers above it alone. The ladder then rounds it to something sayable. */
    double target = (double)body->w * 0.25 * metres_per_pixel;
    double unit = 1.0;
    while (target >= 10.0) {
        target /= 10.0;
        unit *= 10.0;
    }
    while (target < 1.0 && unit > 1e-6) {
        target *= 10.0;
        unit /= 10.0;
    }
    const double rounded = (target >= 5.0 ? 5.0 : (target >= 2.0 ? 2.0 : 1.0)) * unit;
    const int length = (int)(rounded / metres_per_pixel);
    if (length <= 0 || length > body->w) {
        return;
    }

    char label[24];
    mesh_ui_format_distance(rounded, imperial, label, sizeof label);

    const int pad = fb_space_at(state, MESH_UI_SPACE_SM, scale);
    const int thickness = fb_rule_height(state, state->scale);
    const int cap = mesh_ui_font_cap(fb_font(state), scale);
    const int x = body->x + pad;
    /* Everything is measured up from the body's own bottom edge, so the whole bar lives inside
       the map rather than half in the line underneath it. The ticks stand up from the rule and
       the label sits above them, which is the order a ruler is read in. */
    const int bar_y = body->y + body->h - pad - thickness;
    const int tick = cap / 2;
    const int text_y = bar_y - tick - fb_line_adv(state, scale);

    const struct mesh_ui_rgb ink = fb_color(state, MESH_UI_COLOR_TEXT_DIM);
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_SURFACE_LOW);
    /* The bar and a tick at each end, so the length being measured is the span between two
       marks rather than a line that might be a rule. */
    fb_fill_rect(state, x, bar_y, length, thickness, ink);
    fb_fill_rect(state, x, bar_y - tick, thickness, tick, ink);
    fb_fill_rect(state, x + length - thickness, bar_y - tick, thickness, tick, ink);
    fb_draw_text(state, x, text_y, label, scale, ink, ground);
}

/*
 * The crosshair: where the selection is aimed.
 *
 * Four ticks around a gap rather than a cross through the middle, because the thing being aimed
 * at is a marker a few pixels across and a cross would be drawn straight over it. The gap is
 * what the reader looks *into*.
 */
static void fb_map_draw_crosshair(const struct mesh_ui_backend_fb_state *state,
                                  const struct fb_map_box *body, bool on_something) {
    const int cx = body->x + body->w / 2;
    const int cy = body->y + body->h / 2;
    const int cap = mesh_ui_font_cap(fb_font(state), state->scale);
    const int arm = cap / 2;
    const int gap = cap / 2;
    const int thickness = fb_rule_height(state, state->scale) * 2;
    /*
     * The primary when it is on a marker, the dim ink when it is on nothing - so what A would do
     * is readable off the crosshair alone. It is the same predicate the press asks
     * (mesh_ui_map_selected()), passed in rather than asked again here, which is what keeps the
     * ring, the line under the body and the press from ever being three answers.
     */
    const struct mesh_ui_rgb ink =
        on_something
            ? fb_paint(state, MESH_UI_FAMILY_PRIMARY, MESH_UI_SLOT_BASE, MESH_UI_STATE_REST).fill
            : fb_color(state, MESH_UI_COLOR_TEXT_DIM);

    fb_fill_rect(state, cx - thickness / 2, cy - gap - arm, thickness, arm, ink);
    fb_fill_rect(state, cx - thickness / 2, cy + gap, thickness, arm, ink);
    fb_fill_rect(state, cx - gap - arm, cy - thickness / 2, arm, thickness, ink);
    fb_fill_rect(state, cx + gap, cy - thickness / 2, arm, thickness, ink);
}

/* Which family a marker is drawn in: ourselves, a place, a node, a node the radio has
   forgotten. A table here rather than a chain of conditionals at each of the three places that
   need it - the disc, the ring and the label all ask this one question. */
static enum mesh_ui_family fb_map_marker_family(const struct mesh_ui_map_marker *marker) {
    switch ((enum mesh_ui_map_marker_kind)marker->kind) {
    case MESH_UI_MAP_MARKER_SELF:
        return MESH_UI_FAMILY_PRIMARY;
    case MESH_UI_MAP_MARKER_WAYPOINT:
        return MESH_UI_FAMILY_TERTIARY;
    case MESH_UI_MAP_MARKER_NODE:
    default:
        return marker->stale ? MESH_UI_FAMILY_WARNING : MESH_UI_FAMILY_SECONDARY;
    }
}

/*
 * The line under the map: what the crosshair is on, and how far away it is.
 *
 * It costs a body row and is worth one. A marker is four characters and a dot; who it is, how
 * far off, how old the fix is and how much of it to believe are four more facts that have to go
 * somewhere, and the alternative - a label beside every marker carrying all of them - is the
 * panel covered in text with the map underneath it.
 */
static void fb_map_draw_selection(const struct mesh_ui_backend_fb_state *state,
                                  const struct mesh_ui_snapshot *snapshot,
                                  const struct mesh_ui_map_view *view,
                                  const struct mesh_map_viewport *viewport, int y, int scale) {
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_BG);
    uint32_t index = 0U;
    if (!mesh_ui_map_selected(view, viewport, &index)) {
        fb_draw_text(state, fb_margin(state), y, mesh_str(MESH_STR_MAP_NOTHING_SELECTED), scale,
                     fb_color(state, MESH_UI_COLOR_TEXT_DIM), ground);
        return;
    }

    const struct mesh_ui_map_marker *marker = &view->markers[index];
    const bool imperial = snapshot->settings.units == 1U;

    /*
     * The range from our own radio rather than from the middle of the view.
     *
     * The crosshair is wherever the reader has panned to, so a distance measured from it would
     * change every time they nudged the d-pad and would answer a question nobody asked. "How
     * far is that from me" is the question, and it is the same one the Waypoints tab answers -
     * through the same call, so the two screens cannot disagree about a distance.
     */
    char range[MESH_UI_WAYPOINT_RANGE_MAX];
    range[0] = '\0';
    int32_t our_latitude = 0;
    int32_t our_longitude = 0;
    if (view->has_self && marker->kind != MESH_UI_MAP_MARKER_SELF) {
        our_latitude = view->markers[view->self_index].latitude_i;
        our_longitude = view->markers[view->self_index].longitude_i;
        (void)mesh_ui_waypoint_format_range(our_latitude, our_longitude, marker->latitude_i,
                                            marker->longitude_i, imperial, range, sizeof range);
    }

    /* What else is worth saying, in the order a reader needs it: our own radio names itself,
       a node the radio has forgotten says so, and a rounded fix says how far out it may be. */
    char note[48];
    note[0] = '\0';
    if (marker->kind == MESH_UI_MAP_MARKER_SELF) {
        mesh_str_copy(note, sizeof note, mesh_str(MESH_STR_MAP_SELECTED_SELF));
    } else if (marker->stale) {
        mesh_str_copy(note, sizeof note, mesh_str(MESH_STR_MAP_SELECTED_OFF_RADIO));
    } else if (mesh_ui_settings_precision_metres(marker->precision_bits) > 0U) {
        char footprint[24];
        mesh_ui_settings_format_precision(marker->precision_bits, footprint, sizeof footprint);
        mesh_str_format(note, sizeof note, MESH_STR_MAP_SELECTED_APPROX, footprint);
    }

    char line[128];
    if (range[0] != '\0' && note[0] != '\0') {
        char tail[80];
        mesh_str_format(tail, sizeof tail, MESH_STR_MAP_SELECTED_RANGE, range, note);
        mesh_str_format(line, sizeof line, MESH_STR_MAP_SELECTED_RANGE, marker->label, tail);
    } else if (range[0] != '\0' || note[0] != '\0') {
        mesh_str_format(line, sizeof line, MESH_STR_MAP_SELECTED_RANGE, marker->label,
                        range[0] != '\0' ? range : note);
    } else {
        mesh_str_copy(line, sizeof line, marker->label);
    }

    const struct mesh_ui_paint paint =
        fb_paint(state, fb_map_marker_family(marker), MESH_UI_SLOT_BASE, MESH_UI_STATE_REST);
    fb_draw_text(state, fb_margin(state), y, line, scale, paint.fill, ground);
}

void fb_render_map(struct mesh_ui_backend_fb_state *state, const struct mesh_ui_snapshot *snapshot,
                   struct fb_layout *layout) {
    struct mesh_ui_store view_store;
    fb_store_view(snapshot, &view_store);
    struct mesh_ui_map_view view;
    mesh_ui_map_build(&view_store, &view);

    /*
     * The nav's viewport is copied and resized to the body this backend actually has.
     *
     * A copy rather than the nav's own, because a backend is handed a `const` snapshot and has
     * nothing to write back through - which is exactly why the nav's box is a declared number
     * and not a measured one (MESH_UI_MAP_FIT_WIDTH). The centre and the zoom are the nav's and
     * are used as they come; only the box is this backend's business.
     */
    const int margin = fb_margin(state);
    const int scale = layout->small;

    /*
     * The body box, measured before anything is drawn.
     *
     * The badge counts the markers on the panel, so it cannot be written until the panel has
     * been measured - and the panel cannot be measured until the app bar has taken its rows.
     * fb_app_bar_height() is what breaks that circle: the bar's height is asked for rather than
     * discovered by drawing it, so the bar is drawn once, with the right number in it.
     *
     * One body row is also kept back for the line under the map that says what the crosshair is
     * on. It costs a row and is worth one - see fb_map_draw_selection().
     */
    const int top = layout->body_y + fb_app_bar_height(state, layout, 0U);
    const int bottom = layout->footer_y - fb_gutter(state);
    /*
     * Measured in whole body rows, the way every list on this panel is.
     *
     * The map is not a list, but the row it gives up at the bottom is a row of text like any
     * other - so taking it off the row *count* rather than off the pixel height is what keeps
     * that line sitting exactly where a list's last row would, instead of a few pixels into the
     * action bar. Two rows is the floor: one for the map and one for the line under it.
     */
    const uint32_t rows =
        (bottom > top && layout->line > 0) ? (uint32_t)((bottom - top) / layout->line) : 0U;
    if (rows < 2U) {
        return;
    }
    const int selection_y = top + (int)(rows - 1U) * layout->line;
    const struct fb_map_box body = {
        .x = margin,
        .y = top,
        .w = (int)state->var.xres - margin * 2,
        /* Clear of the line under it by the same step a list leaves between a row and its
           supporting line, so the map's edge and the text below it are not touching. */
        .h = selection_y - top - fb_space(state, MESH_UI_SPACE_SM),
    };

    struct mesh_map_viewport viewport = snapshot->nav.map_viewport;
    mesh_map_viewport_resize(&viewport, body.w > 0 ? body.w : 0, body.h > 0 ? body.h : 0);

    char badge[24];
    mesh_str_format(badge, sizeof badge, MESH_STR_MAP_BADGE_IN_VIEW,
                    mesh_ui_map_visible(&view, &viewport), view.known);
    struct fb_app_bar bar;
    memset(&bar, 0, sizeof bar);
    bar.title = mesh_str(MESH_STR_MAP_TITLE);
    bar.badge = badge;
    /*
     * The primary rather than the warning family the badge was invented for. "4 of 9" is a fact
     * about the screen, not a complaint about it: most of a roster having no fix is the ordinary
     * state of a mesh, and a capsule that read as a problem every time the map opened would be
     * the frame crying wolf.
     */
    bar.badge_family = MESH_UI_FAMILY_PRIMARY;
    fb_draw_app_bar(state, layout, &bar);

    if (body.w <= 0 || body.h <= 0) {
        return;
    }

    /*
     * Everything from here to the pop is inside the map's own box. The app bar above is already
     * drawn and the line below it is drawn after, both deliberately outside - they are chrome
     * about the map rather than part of the picture.
     */
    struct fb_map_clip clip;
    fb_map_clip_push(state, &body, &clip);

    /* The ground the map is drawn on: the recessed surface, so the body reads as a panel the
       markers sit in rather than as the screen's own background with dots on it. */
    fb_fill_rect(state, body.x, body.y, body.w, body.h, fb_color(state, MESH_UI_COLOR_SURFACE_LOW));
    fb_map_draw_grid(state, &viewport, &body, fb_color(state, MESH_UI_COLOR_RULE));

    const int radius = fb_map_marker_radius(state);
    const double metres_per_pixel = mesh_map_viewport_metres_per_pixel(&viewport);

    /*
     * The footprints first, under everything.
     *
     * A sender that rounded its position is telling us the node is somewhere in a circle, and a
     * hard dot drawn for it would be the client claiming a precision it was never sent. Drawn as
     * a filled disc rather than an outline because there is no alpha on this panel: a ring would
     * have to be a fill and a second fill in the ground colour, and the second one would erase
     * the grid inside it - which is the very thing a reader judges the distance against.
     */
    for (uint32_t i = 0; i < view.count; ++i) {
        const uint32_t metres = mesh_ui_settings_precision_metres(view.markers[i].precision_bits);
        if (metres == 0U || metres_per_pixel <= 0.0) {
            continue;
        }
        struct mesh_map_placement placement;
        if (!mesh_map_viewport_place(&viewport, view.markers[i].latitude_i,
                                     view.markers[i].longitude_i, &placement) ||
            !placement.visible) {
            continue;
        }
        const int footprint = (int)((double)metres / metres_per_pixel);
        /* Smaller than the marker is a footprint the marker already covers; larger than the
           panel is a circle with no edge on screen, which reads as a change of background
           rather than as an area. Both draw nothing. */
        if (footprint <= radius || footprint > body.w) {
            continue;
        }
        const struct mesh_ui_paint paint = fb_paint(state, fb_map_marker_family(&view.markers[i]),
                                                    MESH_UI_SLOT_CONTAINER, MESH_UI_STATE_REST);
        fb_map_disc(state, body.x + placement.x, body.y + placement.y, footprint, paint.fill);
    }

    /*
     * Then the markers themselves, in build order - so our own radio ends up under everything
     * else, which is where a marker that is usually in the middle of the panel belongs.
     *
     * Markers and labels are two passes rather than one, and that is not tidiness: drawn
     * together, a marker placed later covers a label drawn earlier, so the names nearest the
     * crowded part of the panel are exactly the ones that come out with a dot through them.
     * Two passes also let a label be asked to stand down for a *marker* and not only for
     * another label, which is the commoner collision - four characters beside a dot reach
     * further than the dot does.
     */
    struct fb_map_box taken[FB_MAP_BOXES_MAX];
    size_t taken_count = 0U;
    const int cap = mesh_ui_font_cap(fb_font(state), scale);
    const struct mesh_ui_rgb ground = fb_color(state, MESH_UI_COLOR_SURFACE_LOW);

    /*
     * The crosshair's own square, claimed before anything else is placed.
     *
     * It is drawn last and over everything, so it is the one thing on the panel that cannot be
     * asked to move - which means a name that lands under it comes out with a stroke through
     * its first letter. Reserving it is what stops that, and it is right that it wins: the
     * crosshair is where the reader is aiming, and the label it displaced belongs to whatever
     * it is aiming at, which the line under the map is already naming in full.
     */
    const int crosshair_reach = mesh_ui_font_cap(fb_font(state), state->scale);
    taken[taken_count++] = (struct fb_map_box){
        .x = body.x + body.w / 2 - crosshair_reach,
        .y = body.y + body.h / 2 - crosshair_reach,
        .w = crosshair_reach * 2,
        .h = crosshair_reach * 2,
    };

    for (uint32_t i = 0; i < view.count; ++i) {
        const struct mesh_ui_map_marker *marker = &view.markers[i];
        struct mesh_map_placement placement;
        if (!mesh_map_viewport_place(&viewport, marker->latitude_i, marker->longitude_i,
                                     &placement) ||
            !placement.visible) {
            continue;
        }
        const int cx = body.x + placement.x;
        const int cy = body.y + placement.y;
        const struct mesh_ui_paint paint =
            fb_paint(state, fb_map_marker_family(marker), MESH_UI_SLOT_BASE, MESH_UI_STATE_REST);

        if (marker->kind == MESH_UI_MAP_MARKER_WAYPOINT) {
            /* A place wears the pin the Waypoints tab wears, for the reason icons.def gives:
               one id per job, and both are saying "somewhere on Earth". */
            const int box = fb_icon_box(state, scale);
            fb_draw_icon(state, cx - box / 2, cy + cap / 2, MESH_UI_ICON_POSITION, scale,
                         paint.fill, ground);
        } else {
            fb_map_disc(state, cx, cy, radius, paint.fill);
            if (marker->kind == MESH_UI_MAP_MARKER_SELF) {
                /* A hole in the middle, which is what tells our own radio apart from a node at a
                   glance without spending a second colour on it. */
                fb_map_disc(state, cx, cy, radius / 2, ground);
            }
        }

        if (taken_count < FB_MAP_BOXES_MAX) {
            taken[taken_count++] = (struct fb_map_box){
                .x = cx - radius, .y = cy - radius, .w = radius * 2, .h = radius * 2};
        }
    }

    /*
     * The names, over the markers and around each other.
     *
     * Greedy and in build order, so what survives a crowded corner is what the roster already
     * ranked highest - our own radio first, then the nodes in the order the list shows them. A
     * label that would collide is dropped rather than shortened: two overlapping names are less
     * readable than one name and a bare dot, and a truncated one is a name the reader cannot
     * match against the list they came from.
     */
    size_t labels_drawn = 0U;
    for (uint32_t i = 0; i < view.count && labels_drawn < FB_MAP_LABELS_MAX; ++i) {
        const struct mesh_ui_map_marker *marker = &view.markers[i];
        if (marker->label[0] == '\0') {
            continue;
        }
        struct mesh_map_placement placement;
        if (!mesh_map_viewport_place(&viewport, marker->latitude_i, marker->longitude_i,
                                     &placement) ||
            !placement.visible) {
            continue;
        }
        const int cx = body.x + placement.x;
        const int cy = body.y + placement.y;
        const struct fb_map_box label = {
            .x = cx + radius + fb_space_at(state, MESH_UI_SPACE_XS, scale),
            .y = cy - cap / 2,
            .w = (int)fb_width(marker->label) * fb_char_adv(state, scale),
            .h = cap,
        };
        if (label.x + label.w >= body.x + body.w) {
            continue;
        }
        bool clear = true;
        for (size_t j = 0; j < taken_count; ++j) {
            if (fb_map_boxes_overlap(&label, &taken[j])) {
                clear = false;
                break;
            }
        }
        if (!clear) {
            continue;
        }
        if (taken_count < FB_MAP_BOXES_MAX) {
            taken[taken_count++] = label;
        }
        ++labels_drawn;
        const struct mesh_ui_paint paint =
            fb_paint(state, fb_map_marker_family(marker), MESH_UI_SLOT_BASE, MESH_UI_STATE_REST);
        fb_draw_text(state, label.x, cy + cap / 2, marker->label, scale, paint.fill, ground);
    }

    uint32_t selected = 0U;
    const bool on_something = mesh_ui_map_selected(&view, &viewport, &selected);
    fb_map_draw_crosshair(state, &body, on_something);
    fb_map_draw_scale(state, &viewport, &body, snapshot->settings.units == 1U, scale);
    fb_map_clip_pop(state, &clip);

    /* Outside the box, because it is what the map has to say rather than part of what it draws. */
    fb_map_draw_selection(state, snapshot, &view, &viewport, selection_y, scale);
}
