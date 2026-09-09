#ifndef MESH_GEO_MERCATOR_H
#define MESH_GEO_MERCATOR_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Where a coordinate lands on a flat picture of the world - the third piece of the `geo` module
 * docs/maps-roadmap.md proposes, written now because a map needs it and not before.
 *
 * Web Mercator, because it is what every raster tile set on Earth is cut into: a tile pyramid
 * addressed by (z, x, y) is a statement about *this* projection, so a client that drew its
 * markers in any other one would have them slide off the basemap the day one arrives. There is
 * no basemap yet; the projection is chosen for the one that is coming, which is the whole
 * reason it is this projection and not a simpler one.
 *
 * Like coords.h and vector.h this knows nothing about protobufs, the UI store or the
 * framebuffer, and nothing about zoom levels, tiles or pixels either: it answers in the unit
 * square, and the map layer above it decides how big the world is. That split is what keeps
 * `geo` reusable by anything that needs a coordinate turned into a position - a full map, a
 * one-line location preview, a future compass rose - without any of them inheriting a viewport.
 *
 * This is the second file in the tree to include <math.h>, after vector.c. The projection needs
 * a logarithm going out and an exponential coming back, and there is no fixed-point way to
 * those that is not a table nobody can check against the definition.
 */

/*
 * The latitude the picture stops at, in fixed-point 1e-7 degrees: 85.05112878 degrees north and
 * south.
 *
 * It is deliberately *not* MESH_GEO_LATITUDE_I_MAX, and the two must never be merged. That one
 * is a fact about Earth - the poles are places, and a node that reports being at one is
 * reporting something true. This one is a fact about a picture: Mercator sends the poles to
 * infinity, so a map has to stop somewhere, and 85.05112878 is where the world becomes a square
 * and the tile pyramid closes. A validator that used the display limit would start refusing
 * real fixes from Svalbard; a projection that used the geographic one would divide by zero at
 * the pole. Hence two constants, named for the two different questions.
 *
 * The value is atan(sinh(pi)) in degrees - the latitude whose projected y is exactly one
 * half-world from the equator - rounded to the nearest unit of 1e-7 degrees, which is a
 * hundredth of a millimetre and lands just *outside* the true limit. Outside rather than inside
 * matters: the projection closes the unit square by clamping, and a constant a unit short of
 * the corner would leave the world's own edge a few billionths of a world away from it.
 */
#define MESH_GEO_MERCATOR_LATITUDE_I_MAX 850511288

/*
 * A point on the unit square: the whole world is x and y in [0, 1], with (0, 0) at the
 * north-west corner - 180 degrees west, the top of the picture - and y increasing southwards,
 * which is the direction a screen's rows already run and the direction tile numbering already
 * counts in.
 *
 * Doubles rather than fixed point because the caller multiplies by a world size that can be
 * 256 << 20 pixels, and a fixed-point unit square would quantise the far end of that to
 * something coarser than the panel. The arithmetic is a handful of multiplies per marker per
 * frame on a screen that draws a hundred of them at most.
 */
struct mesh_geo_point {
    double x;
    double y;
};

/*
 * Projects a coordinate onto the unit square.
 *
 * False - and `out` zeroed - when the pair is not a point on Earth, which is
 * mesh_geo_coords_valid()'s question and deliberately not a second copy of it.
 *
 * A latitude beyond the display limit is *clamped*, not refused: a node at 88 degrees north is
 * somewhere real and the honest thing to draw is the top edge of the picture, not nothing. That
 * is the one place where the two limits above are both consulted in one call - the geographic
 * one decides whether there is an answer, and the display one decides where it goes.
 */
bool mesh_geo_mercator_forward(int32_t latitude_i, int32_t longitude_i, struct mesh_geo_point *out);

/*
 * The inverse: a point on the unit square back to a coordinate.
 *
 * `x` wraps rather than clamping, because the unit square's left and right edges are the same
 * meridian and a map panned past one arrives at the other. `y` is clamped to the square, so a
 * caller that has walked off the top gets the display limit rather than a latitude outside the
 * range every ingress in this client refuses.
 *
 * Never fails: every point in the plane has a coordinate under this treatment, which is what
 * lets a pan be arithmetic rather than a bounds test at every step.
 */
void mesh_geo_mercator_inverse(struct mesh_geo_point point, int32_t *out_latitude_i,
                               int32_t *out_longitude_i);

/*
 * A longitude folded back into [-180, 180] degrees, in fixed point.
 *
 * The antimeridian is where a client that never thought about it goes wrong: two nodes either
 * side of it are a few kilometres apart and 360 degrees apart in the arithmetic. Panning east
 * off 180 has to arrive at -180, and this is the one function that says so - the same reason
 * the bounds test lives in one place rather than at each ingress.
 */
int32_t mesh_geo_longitude_wrap_i(int64_t longitude_i);

/*
 * How many metres of ground one whole world-width covers at this latitude.
 *
 * The projection's own scale factor, which is what a scale bar is ultimately drawn from: on
 * Mercator a picture is stretched by 1/cos(latitude), so a pixel near a pole covers far less
 * ground than one at the equator. The caller divides by however many pixels wide it has decided
 * the world is - this knows nothing about zoom, and that division is the map layer's business.
 *
 * The equatorial circumference of the WGS 84 ellipsoid rather than the mean radius vector.c
 * measures distances with, and the difference is deliberate. That one answers "how far apart
 * are two nodes", where a sphere is the honest simplification. This one answers "how big is the
 * picture", and the picture is a tile pyramid cut on the sphere of radius 6378137 by every
 * renderer that has ever produced one - so agreeing with the pyramid is the point. The two
 * disagree by about a tenth of a percent, which is a metre in a kilometre and below anything
 * either is ever asked to show.
 *
 * A latitude beyond the display limit is clamped to it, so this never divides by zero at a
 * pole. Latitudes that are not on Earth answer 0.
 */
double mesh_geo_mercator_metres_per_world(int32_t latitude_i);

#endif /* MESH_GEO_MERCATOR_H */
