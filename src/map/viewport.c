#include "mesh/map/viewport.h"

#include "mesh/geo/coords.h"

#include <string.h>

/*
 * The viewport, and deliberately no <math.h>.
 *
 * Everything transcendental about a map is a property of the projection, and the projection is
 * in src/geo - so the logarithm, the exponential and the cosine are all one directory down and
 * this file is multiplication, comparison and a nineteen-step loop. That is not tidiness for its
 * own sake: it is what keeps "the geography module is where the maths lives" a rule a reader can
 * check by looking at the includes rather than a claim in a comment.
 */

/* How wide the world is in pixels at a zoom: the tile pyramid's own definition. */
static double viewport_world_pixels(uint8_t zoom) {
    return (double)MESH_MAP_TILE_SIZE * (double)(1UL << zoom);
}

static uint8_t viewport_clamp_zoom(int zoom) {
    if (zoom < MESH_MAP_ZOOM_MIN) {
        return (uint8_t)MESH_MAP_ZOOM_MIN;
    }
    if (zoom > MESH_MAP_ZOOM_MAX) {
        return (uint8_t)MESH_MAP_ZOOM_MAX;
    }
    return (uint8_t)zoom;
}

/* Whether there is anything to draw into. Asked by every projection below, because a viewport
   the nav opened before a panel was measured is the ordinary first frame rather than an error. */
static bool viewport_has_area(const struct mesh_map_viewport *viewport) {
    return viewport != NULL && viewport->width > 0 && viewport->height > 0;
}

/* Where the centre is on the unit square. Always answerable: the centre is kept a valid
   coordinate by every call that writes one. */
static struct mesh_geo_point viewport_center_point(const struct mesh_map_viewport *viewport) {
    struct mesh_geo_point point = {.x = 0.5, .y = 0.5};
    (void)mesh_geo_mercator_forward(viewport->center_latitude_i, viewport->center_longitude_i,
                                    &point);
    return point;
}

void mesh_map_viewport_init(struct mesh_map_viewport *viewport, int32_t latitude_i,
                            int32_t longitude_i, uint8_t zoom) {
    if (viewport == NULL) {
        return;
    }
    memset(viewport, 0, sizeof *viewport);
    viewport->zoom = viewport_clamp_zoom((int)zoom);
    if (mesh_geo_coords_valid(latitude_i, longitude_i)) {
        viewport->center_latitude_i = latitude_i;
        viewport->center_longitude_i = longitude_i;
    }
    /* Otherwise Null Island, which the memset already left behind - and which is a real point
       rather than a sentinel, so nothing downstream has to test for it. */
}

void mesh_map_viewport_resize(struct mesh_map_viewport *viewport, int32_t width, int32_t height) {
    if (viewport == NULL) {
        return;
    }
    viewport->width = width > 0 ? width : 0;
    viewport->height = height > 0 ? height : 0;
}

bool mesh_map_viewport_center_on(struct mesh_map_viewport *viewport, int32_t latitude_i,
                                 int32_t longitude_i) {
    if (viewport == NULL || !mesh_geo_coords_valid(latitude_i, longitude_i)) {
        return false;
    }
    if (viewport->center_latitude_i == latitude_i && viewport->center_longitude_i == longitude_i) {
        return false;
    }
    viewport->center_latitude_i = latitude_i;
    viewport->center_longitude_i = longitude_i;
    return true;
}

bool mesh_map_viewport_offset(const struct mesh_map_viewport *viewport, int32_t latitude_i,
                              int32_t longitude_i, double *out_dx, double *out_dy) {
    if (out_dx != NULL) {
        *out_dx = 0.0;
    }
    if (out_dy != NULL) {
        *out_dy = 0.0;
    }
    if (viewport == NULL) {
        return false;
    }
    struct mesh_geo_point point;
    if (!mesh_geo_mercator_forward(latitude_i, longitude_i, &point)) {
        return false;
    }

    const struct mesh_geo_point center = viewport_center_point(viewport);
    const double world = viewport_world_pixels(viewport->zoom);

    /*
     * The short way round, which is the whole of what makes the antimeridian work.
     *
     * The unit square's two vertical edges are one meridian, so a marker is at x, and also at
     * x + 1, and also at x - 1. Centred at 179 east, a marker at 179 west is two degrees away
     * eastwards and 358 degrees away westwards; taking the difference as it comes would place
     * it most of a world off the left of the panel, which is arithmetically true and useless.
     * Folding the difference into [-0.5, 0.5] picks the copy nearest the centre, every time.
     */
    double dx = point.x - center.x;
    while (dx > 0.5) {
        dx -= 1.0;
    }
    while (dx < -0.5) {
        dx += 1.0;
    }

    if (out_dx != NULL) {
        *out_dx = dx * world;
    }
    if (out_dy != NULL) {
        *out_dy = (point.y - center.y) * world;
    }
    return true;
}

bool mesh_map_viewport_place(const struct mesh_map_viewport *viewport, int32_t latitude_i,
                             int32_t longitude_i, struct mesh_map_placement *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (!viewport_has_area(viewport)) {
        return false;
    }
    /* The offset, and then the box - so a placement and a selection are one derivation with the
       panel added at the end of one of them, rather than two that agree until they do not. */
    double dx = 0.0;
    double dy = 0.0;
    if (!mesh_map_viewport_offset(viewport, latitude_i, longitude_i, &dx, &dy)) {
        return false;
    }

    const double x = (double)viewport->width / 2.0 + dx;
    const double y = (double)viewport->height / 2.0 + dy;

    /*
     * Clamped before the cast rather than after it. A marker half a world away at zoom 18 is
     * some thirty million pixels off the panel, which still fits an int32_t - but the same
     * arithmetic in `y` at the top of the world does not have to, and a double that overflows
     * the cast is undefined rather than merely far away. The clamp is well outside any panel,
     * so nothing visible changes; what it buys is that `visible` is the only thing a caller
     * has to read to know whether to draw.
     */
    const double limit = 1000000.0;
    out->x = (int32_t)(x > limit ? limit : (x < -limit ? -limit : x));
    out->y = (int32_t)(y > limit ? limit : (y < -limit ? -limit : y));
    out->visible =
        out->x >= 0 && out->x < viewport->width && out->y >= 0 && out->y < viewport->height;
    return true;
}

void mesh_map_viewport_at(const struct mesh_map_viewport *viewport, int32_t x, int32_t y,
                          int32_t *out_latitude_i, int32_t *out_longitude_i) {
    if (out_latitude_i != NULL) {
        *out_latitude_i = 0;
    }
    if (out_longitude_i != NULL) {
        *out_longitude_i = 0;
    }
    if (!viewport_has_area(viewport)) {
        return;
    }

    const struct mesh_geo_point center = viewport_center_point(viewport);
    const double world = viewport_world_pixels(viewport->zoom);
    const struct mesh_geo_point point = {
        .x = center.x + ((double)x - (double)viewport->width / 2.0) / world,
        .y = center.y + ((double)y - (double)viewport->height / 2.0) / world,
    };
    /* The inverse wraps x and clamps y itself, which is why nothing is normalised here: two
       opinions about where the world ends is exactly what geo exists to prevent. */
    mesh_geo_mercator_inverse(point, out_latitude_i, out_longitude_i);
}

bool mesh_map_viewport_pan(struct mesh_map_viewport *viewport, int32_t dx, int32_t dy) {
    if (viewport == NULL || (dx == 0 && dy == 0)) {
        return false;
    }
    const double world = viewport_world_pixels(viewport->zoom);
    const struct mesh_geo_point center = viewport_center_point(viewport);
    const struct mesh_geo_point moved = {
        .x = center.x + (double)dx / world,
        .y = center.y + (double)dy / world,
    };

    int32_t latitude_i = 0;
    int32_t longitude_i = 0;
    mesh_geo_mercator_inverse(moved, &latitude_i, &longitude_i);
    if (latitude_i == viewport->center_latitude_i && longitude_i == viewport->center_longitude_i) {
        /*
         * Nothing moved, which is a real state rather than a failure: a view already against the
         * top of the world does not travel further north however long the d-pad is held, and at
         * zoom 0 a whole panel of pan is a fraction of a unit of latitude. Reporting it is what
         * lets the caller not repaint.
         */
        return false;
    }
    viewport->center_latitude_i = latitude_i;
    viewport->center_longitude_i = longitude_i;
    return true;
}

bool mesh_map_viewport_zoom_by(struct mesh_map_viewport *viewport, int delta) {
    if (viewport == NULL || delta == 0) {
        return false;
    }
    const uint8_t zoom = viewport_clamp_zoom((int)viewport->zoom + delta);
    if (zoom == viewport->zoom) {
        return false;
    }
    viewport->zoom = zoom;
    return true;
}

bool mesh_map_viewport_fit(struct mesh_map_viewport *viewport, const struct mesh_geo_point *points,
                           size_t count, int32_t margin) {
    if (viewport == NULL || points == NULL || count == 0U) {
        return false;
    }

    /*
     * The vertical span is a plain minimum and maximum: y does not wrap, because the top and
     * bottom of the picture are two different places.
     */
    double min_y = points[0].y;
    double max_y = points[0].y;
    for (size_t i = 1; i < count; ++i) {
        if (points[i].y < min_y) {
            min_y = points[i].y;
        }
        if (points[i].y > max_y) {
            max_y = points[i].y;
        }
    }

    /*
     * The horizontal span is not, and this is the antimeridian again in its second disguise.
     *
     * x wraps, so the set has no leftmost and no rightmost member - every point is at x, and at
     * x + 1, and at x - 1, and a plain minimum and maximum over those would frame a mesh
     * straddling 180 degrees as though it spanned the Pacific. That bug is invisible everywhere
     * else on Earth, so nothing but a test would ever catch it.
     *
     * The fix is the same fold mesh_map_viewport_place() already applies, used once more: take
     * the first point as a reference and measure every other one as a signed offset from it,
     * the short way round. The extent is then an ordinary minimum and maximum over those
     * offsets, and the centre is the reference plus their midpoint.
     *
     * What this assumes is that the set spans less than half the world, and that assumption is
     * worth stating rather than hiding: beyond it, "the short way round" stops being
     * well-defined - a set spread over more than 180 degrees has no near side - and the frame
     * comes out arbitrary rather than wrong in some particular direction. No mesh is that
     * large. LoRa reaches tens of kilometres, and a roster spanning a hemisphere would be a
     * roster of nodes that cannot hear each other. Choosing this over a sort is choosing an
     * honest limit at a scale nothing reaches over a scratch buffer that would have silently
     * dropped markers past its own end.
     */
    const double reference = points[0].x;
    double min_dx = 0.0;
    double max_dx = 0.0;
    for (size_t i = 1; i < count; ++i) {
        double dx = points[i].x - reference;
        while (dx > 0.5) {
            dx -= 1.0;
        }
        while (dx < -0.5) {
            dx += 1.0;
        }
        if (dx < min_dx) {
            min_dx = dx;
        }
        if (dx > max_dx) {
            max_dx = dx;
        }
    }
    const double span_x = max_dx - min_dx;
    const double span_y = max_y - min_y;

    /*
     * The deepest zoom whose world is still small enough for the span to fit the box.
     *
     * A loop from the far end rather than a logarithm, because there are nineteen levels and
     * because it keeps <math.h> out of this file. It also makes the degenerate cases fall out
     * rather than needing tests of their own: a span of zero fits at every zoom, so a single
     * point - or several at the same place - lands on the maximum and is then held to the
     * caller's own zoom by the branch below.
     */
    const double avail_w = (double)viewport->width - 2.0 * (double)margin;
    const double avail_h = (double)viewport->height - 2.0 * (double)margin;
    int zoom = MESH_MAP_ZOOM_MAX;
    if (avail_w > 0.0 && avail_h > 0.0) {
        while (zoom > MESH_MAP_ZOOM_MIN) {
            const double world = viewport_world_pixels((uint8_t)zoom);
            if (span_x * world <= avail_w && span_y * world <= avail_h) {
                break;
            }
            --zoom;
        }
    } else {
        /* No box to fit into yet. Centring is still worth doing - it is the half of this call
           that does not need pixels - and the zoom is left as the caller set it. */
        zoom = (int)viewport->zoom;
    }

    /*
     * A set with no extent gives no zoom, so the caller's own is kept: one marker is a place to
     * look at, not a reason to fall to building scale, and a mesh whose members all report the
     * same rounded position would otherwise slam to zoom 18 on a fix nobody measured that finely.
     */
    if (span_x <= 0.0 && span_y <= 0.0) {
        zoom = (int)viewport->zoom;
    }

    /* The reference point plus the middle of the offsets around it, folded back onto the
       square - a centre derived this way can land either side of the seam, and the world is
       closed. */
    struct mesh_geo_point folded = {
        .x = reference + (min_dx + max_dx) / 2.0,
        .y = (min_y + max_y) / 2.0,
    };
    while (folded.x >= 1.0) {
        folded.x -= 1.0;
    }
    while (folded.x < 0.0) {
        folded.x += 1.0;
    }

    int32_t latitude_i = 0;
    int32_t longitude_i = 0;
    mesh_geo_mercator_inverse(folded, &latitude_i, &longitude_i);
    viewport->center_latitude_i = latitude_i;
    viewport->center_longitude_i = longitude_i;
    viewport->zoom = viewport_clamp_zoom(zoom);
    return true;
}

double mesh_map_viewport_metres_per_pixel(const struct mesh_map_viewport *viewport) {
    if (viewport == NULL) {
        return 0.0;
    }
    /*
     * No box needed, and that is load-bearing rather than incidental.
     *
     * How much ground a pixel covers is a function of the zoom and the latitude and nothing
     * else - the panel's size does not change the scale, only how much of it fits. Which means
     * this is the one measurement of the map that every caller agrees about whether or not it
     * has been told how big the panel is, and it is therefore what the *selection* is measured
     * in: the nav decides which marker a press opens and the backend decides which one draws a
     * ring, they cannot ask each other how wide the body is, and a selection derived from
     * anything box-dependent would be two answers to one question.
     */
    return mesh_geo_mercator_metres_per_world(viewport->center_latitude_i) /
           viewport_world_pixels(viewport->zoom);
}
