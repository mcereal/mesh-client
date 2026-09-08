#ifndef MESH_GEO_VECTOR_H
#define MESH_GEO_VECTOR_H

#include <stdbool.h>
#include <stdint.h>

/*
 * How far away something is and which way it lies - the second piece of the `geo` module
 * docs/maps-roadmap.md proposes, written because a waypoint list needs it rather than
 * speculatively.
 *
 * This is the answer a handheld with no map can still give: a place is a name, a distance and
 * a direction from where you are. The same call serves the map layer when it arrives, which is
 * the whole reason it is here and not inside a screen.
 *
 * Like coords.h this knows nothing about protobufs, the UI store or the framebuffer. It is the
 * one place in the client that includes <math.h>, and the reason libm is linked at all.
 */

/*
 * The eight points, in the order a compass rose walks them from north.
 *
 * Eight rather than sixteen because the reading is a direction to start walking in, not a
 * heading to steer, and because "NNE" is three cells of a row that already carries a name, a
 * distance and an age. Sixteen would also be sixteen strings each locale has to get right.
 */
enum mesh_geo_compass {
    MESH_GEO_COMPASS_N = 0,
    MESH_GEO_COMPASS_NE,
    MESH_GEO_COMPASS_E,
    MESH_GEO_COMPASS_SE,
    MESH_GEO_COMPASS_S,
    MESH_GEO_COMPASS_SW,
    MESH_GEO_COMPASS_W,
    MESH_GEO_COMPASS_NW,
    MESH_GEO_COMPASS_COUNT,
};

/*
 * The displacement from one point on Earth to another.
 *
 * `has_bearing` is the part worth reading twice: two coordinates that are the same point have a
 * distance of zero and no direction at all, and atan2(0, 0) answers 0 - which is "due north",
 * a fact this has no business claiming. A row asking for the way to where it already is gets
 * told there is no way rather than being pointed at the pole.
 */
struct mesh_geo_vector {
    double distance_m;  /* great-circle distance in metres, never negative */
    double bearing_deg; /* initial bearing, degrees clockwise from true north, [0, 360) */
    bool has_bearing;
};

/*
 * Fills `out` with the vector from the first coordinate to the second.
 *
 * False - and `out` zeroed - when either pair is not a point on Earth, which is
 * mesh_geo_coords_valid()'s question and deliberately not a second copy of it. A caller with a
 * fix it has not range-checked gets an honest "cannot say" rather than a distance derived from
 * 200 degrees of latitude.
 *
 * The sphere, not the ellipsoid: haversine on the IUGG mean radius is within about half a
 * percent of the geodesic anywhere on Earth, which is a couple of hundred metres over a
 * distance no LoRa mesh spans, and none of the inputs is that good anyway - a position rounded
 * to `precision_bits` has already been thrown a kilometre or more.
 */
bool mesh_geo_vector_between(int32_t from_latitude_i, int32_t from_longitude_i,
                             int32_t to_latitude_i, int32_t to_longitude_i,
                             struct mesh_geo_vector *out);

/* Which of the eight points a bearing falls in. Each point owns the 45 degrees centred on it,
   so anything from 337.5 round to 22.5 is north. Bearings outside [0, 360) are wrapped. */
enum mesh_geo_compass mesh_geo_compass_of(double bearing_deg);

#endif /* MESH_GEO_VECTOR_H */
