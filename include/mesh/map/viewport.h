#ifndef MESH_MAP_VIEWPORT_H
#define MESH_MAP_VIEWPORT_H

#include "mesh/geo/mercator.h"
#include "mesh/map/tile.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where the map is looking: a centre, a zoom, and a box of pixels to draw into.
 *
 * This is the whole of the map's state. Everything a screen wants to know - where a marker
 * lands, what a press at the middle of the panel is pointing at, how far a pan moved the world,
 * how long a scale bar should be - is a function of these four numbers, which is what makes the
 * map testable without a framebuffer and what stops a renderer accumulating a second opinion
 * about where the map is.
 *
 * `geo` answers in the unit square and knows nothing about zoom; this layer multiplies that by
 * a world size and knows nothing about markers, stores or panels. Nothing here includes a
 * protobuf, the UI store, the framebuffer or a filesystem header - the boundary
 * docs/maps-roadmap.md proposed, now with a caller to hold it honest.
 *
 * Tile addressing was deliberately absent from this header until there was a tile to fetch,
 * which is the habit `geo` was built with - the bounds test alone until a range needed a
 * vector. There is a tile to fetch now: mesh/map/source.h reads one out of a pack, so
 * mesh_map_viewport_tiles() is what asks it for the right ones. The *vocabulary* of a tile is
 * one header down in mesh/map/tile.h, so that a source can speak it without inheriting the
 * projection.
 */

struct mesh_map_viewport {
    /* Where the middle of the box is. Always a valid coordinate: every call below leaves it
       one, so no caller has to range-check what it reads back. */
    int32_t center_latitude_i;
    int32_t center_longitude_i;
    uint8_t zoom;
    /* The box, in pixels. A viewport with no area projects nothing and answers nothing, which
       is the honest reading of a panel that has not been laid out yet. */
    int32_t width;
    int32_t height;
};

/*
 * A point placed in the box: where it landed, and whether that is somewhere the caller can draw.
 *
 * `visible` is a separate answer from the coordinates rather than a NULL return, because a
 * marker just off the edge is still worth counting - the app bar says how many of the markers
 * are in view, and a screen cannot say "3 off screen" about points it was never given
 * positions for.
 */
struct mesh_map_placement {
    int32_t x;
    int32_t y;
    bool visible;
};

/*
 * Starts a viewport at a coordinate, or at Null Island when the coordinate is not a place.
 *
 * The box may be zero: a nav opens the map before a backend has measured a panel, and a
 * viewport that refused to exist until it had pixels would make the nav depend on the
 * framebuffer's layout. mesh_map_viewport_resize() fills it in on the first frame.
 */
void mesh_map_viewport_init(struct mesh_map_viewport *viewport, int32_t latitude_i,
                            int32_t longitude_i, uint8_t zoom);

/* Sets the pixel box, clamping negatives to zero. Called once per frame by whoever knows how
   big the body is; a size that has not changed is not a state change. */
void mesh_map_viewport_resize(struct mesh_map_viewport *viewport, int32_t width, int32_t height);

/* Moves the centre, keeping the zoom. A coordinate that is not a place leaves the centre alone,
   which is the same answer every other ingress in this client gives a bad fix. */
bool mesh_map_viewport_center_on(struct mesh_map_viewport *viewport, int32_t latitude_i,
                                 int32_t longitude_i);

/*
 * Where a coordinate lands in the box.
 *
 * False - and `out` zeroed - only when the coordinate is not a place or the box has no area.
 * Being off the edge is not a failure; it is `visible` being false on an answer that still has
 * coordinates.
 *
 * The longitude nearest the centre wins, which is what makes the antimeridian work: a viewport
 * centred at 179 east and a marker at 179 west are two degrees apart, and the placement that
 * put the marker a whole world to the left would be arithmetically correct and useless.
 */
bool mesh_map_viewport_place(const struct mesh_map_viewport *viewport, int32_t latitude_i,
                             int32_t longitude_i, struct mesh_map_placement *out);

/*
 * How far a coordinate is from the middle of the view, in pixels, without reference to the box.
 *
 * The half of a placement that does not need a panel: mesh_map_viewport_place() is exactly this
 * plus the centre of the box, and it is written that way so the two cannot drift.
 *
 * It exists because the *selection* has to be measured where a marker is drawn rather than
 * across the ground, and has to be answerable by a caller with no box - see mesh_ui_map_selected()
 * for why the nav and a backend can never agree on one. Measuring it in the projection is also
 * the only thing that gets the poles right: a fix beyond the display limit is drawn at the limit,
 * so a marker at 88 degrees north and a view centred on it are the same point on the picture and
 * three degrees apart on the ground. A distance across the ground would refuse a marker sitting
 * under the crosshair.
 *
 * False - and the offsets zeroed - when the coordinate is not a point on Earth. The horizontal
 * offset takes the short way round, the same fold a placement takes.
 */
bool mesh_map_viewport_offset(const struct mesh_map_viewport *viewport, int32_t latitude_i,
                              int32_t longitude_i, double *out_dx, double *out_dy);

/* What a pixel in the box is pointing at. The inverse of the above, and the reason a press at
   the middle of the panel can name a place. */
void mesh_map_viewport_at(const struct mesh_map_viewport *viewport, int32_t x, int32_t y,
                          int32_t *out_latitude_i, int32_t *out_longitude_i);

/*
 * Pans by a number of pixels, at the current zoom.
 *
 * East and south are positive, matching the box's own axes: a pan of +10 in x moves the *view*
 * ten pixels east, so the world under it appears to slide west. Latitude is clamped at the
 * display limit and longitude wraps, so a pan can be repeated forever without a caller ever
 * bounds-testing one - which is what lets a held d-pad be a loop over this function.
 *
 * False when nothing moved: a pan of nothing, or one already against the top of the world.
 */
bool mesh_map_viewport_pan(struct mesh_map_viewport *viewport, int32_t dx, int32_t dy);

/*
 * Steps the zoom by whole levels, keeping the centre where it is.
 *
 * Integer levels only, because a raster pyramid has integer levels: a fractional zoom is a
 * resampled tile, which is a blurred picture on a panel that cannot afford one. Clamped at both
 * ends; false when it was already there.
 */
bool mesh_map_viewport_zoom_by(struct mesh_map_viewport *viewport, int delta);

/*
 * Frames a set of points: centres on them and picks the deepest zoom that shows them all.
 *
 * `margin` is how many pixels of air to leave around the outermost point, so a marker at the
 * edge of the set is not drawn half off the panel.
 *
 * The set is taken as it comes, in the longitudes it was given, and the span is measured the
 * short way round the world - so a mesh either side of the antimeridian frames as the few
 * kilometres it occupies rather than as the 359 degrees between its two extreme longitudes.
 *
 * False, and the viewport untouched, when there is nothing to frame. One point is not a
 * degenerate case: it centres and keeps the zoom, because there is no span to derive one from.
 */
bool mesh_map_viewport_fit(struct mesh_map_viewport *viewport, const struct mesh_geo_point *points,
                           size_t count, int32_t margin);

/*
 * Which tiles of the pyramid the box is standing on.
 *
 * The half of a basemap that is arithmetic: a caller with this and a source has everything it
 * needs to fill the panel, and neither of them has to know where the map is looking. What comes
 * back is a rectangle in tile space plus where its corner lands, so a blit is that corner plus a
 * multiple of a tile - see struct mesh_map_tile_span for why it is a rectangle and not a list.
 *
 * False - and `out` zeroed - when the viewport has no area, which is the ordinary first frame
 * rather than an error, or when the box has been panned so far up or down that no row of the
 * world is under it.
 *
 * The span is measured against the box the viewport *has*, which is why a renderer resizes its
 * own copy to the real body before asking. Handing it the nav's declared
 * MESH_UI_MAP_FIT_WIDTH would fetch fewer tiles than the reader can see - the same asymmetry
 * that makes a declared box the safe side for a fit and the wrong side for anything that has to
 * cover the panel.
 */
bool mesh_map_viewport_tiles(const struct mesh_map_viewport *viewport,
                             struct mesh_map_tile_span *out);

/*
 * How many metres a pixel covers at the centre of the view - what a scale bar is drawn from.
 *
 * At the centre, and that is worth saying out loud: Mercator's scale changes with latitude, so
 * a bar drawn from this number is honest across the middle of the panel and optimistic at the
 * top of a view near a pole. Every map with a scale bar has this property; the alternative is a
 * bar that changes length as you pan north, which reads as a fault.
 *
 * 0 when the viewport has no area.
 */
double mesh_map_viewport_metres_per_pixel(const struct mesh_map_viewport *viewport);

#ifdef __cplusplus
}
#endif

#endif /* MESH_MAP_VIEWPORT_H */
