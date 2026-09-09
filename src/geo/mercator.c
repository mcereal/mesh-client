#include "mesh/geo/mercator.h"

#include "mesh/geo/coords.h"

#include <math.h>
#include <string.h>

/*
 * Pi, spelled out - the same constant vector.c spells out, and for the same reason: glibc hides
 * M_PI unless a feature-test macro asks for X/Open, and this tree builds -std=c17 with
 * CMAKE_C_EXTENSIONS OFF.
 *
 * It is written twice rather than hoisted into a shared header because two files needing a
 * number is not a module boundary, and a `geo_math.h` holding one #define would be a header
 * whose whole content is a number that has not changed since Archimedes.
 */
#define MESH_GEO_PI 3.14159265358979323846

/* Fixed-point 1e-7 degrees to radians in one step, as vector.c does it: folding the two
   conversions into one constant rounds once rather than twice. */
#define MESH_GEO_FIXED_TO_RAD (MESH_GEO_PI / 1800000000.0)
/* Radians back to fixed-point 1e-7 degrees, the same fold in the other direction. */
#define MESH_GEO_RAD_TO_FIXED (1800000000.0 / MESH_GEO_PI)

/* 360 degrees at 1e-7, which is the whole way round and therefore the wrap's period. */
#define MESH_GEO_LONGITUDE_I_SPAN (2 * (int64_t)MESH_GEO_LONGITUDE_I_MAX)

int32_t mesh_geo_longitude_wrap_i(int64_t longitude_i) {
    /*
     * Arithmetic rather than a loop, because a caller panning at zoom 20 can hand this a value
     * hundreds of turns off the map and a loop would spin once per turn.
     *
     * The remainder is taken first and lands in (-360, 360); the two tests then fold it into
     * [-180, 180]. Both ends stay reachable on purpose: the antimeridian is a real meridian and
     * a fix on it must survive the round trip, which is why this is not the usual
     * half-open [-180, 180) fold. A value of exactly +180 or -180 is left as it came.
     */
    int64_t wrapped = longitude_i % MESH_GEO_LONGITUDE_I_SPAN;
    if (wrapped > MESH_GEO_LONGITUDE_I_MAX) {
        wrapped -= MESH_GEO_LONGITUDE_I_SPAN;
    } else if (wrapped < -(int64_t)MESH_GEO_LONGITUDE_I_MAX) {
        wrapped += MESH_GEO_LONGITUDE_I_SPAN;
    }
    return (int32_t)wrapped;
}

bool mesh_geo_mercator_forward(int32_t latitude_i, int32_t longitude_i,
                               struct mesh_geo_point *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (!mesh_geo_coords_valid(latitude_i, longitude_i)) {
        return false;
    }

    /*
     * The display clamp, applied to a coordinate the range check has already accepted. See the
     * header: a fix at 88 degrees north is a real fix, and the top edge of the picture is where
     * it honestly goes. Refusing it would lose a node; passing it through would ask tan() for
     * a quarter turn.
     */
    int32_t clamped = latitude_i;
    if (clamped > MESH_GEO_MERCATOR_LATITUDE_I_MAX) {
        clamped = MESH_GEO_MERCATOR_LATITUDE_I_MAX;
    } else if (clamped < -MESH_GEO_MERCATOR_LATITUDE_I_MAX) {
        clamped = -MESH_GEO_MERCATOR_LATITUDE_I_MAX;
    }

    /* x is linear in longitude: half a world at the prime meridian, 0 at 180 west. */
    out->x = ((double)longitude_i / (double)MESH_GEO_LONGITUDE_I_SPAN) + 0.5;

    /*
     * y is the Mercator ordinate, normalised so that the equator is 0.5 and the display limit
     * is 0 and 1. The inverse Gudermannian is written as log(tan(pi/4 + lat/2)) rather than as
     * asinh(tan(lat)) because the first is the definition every tile-pyramid document states
     * and the second is an identity a reader has to verify; they agree to the last bit over
     * this range and only one of them can be checked against the specification.
     */
    const double lat_rad = (double)clamped * MESH_GEO_FIXED_TO_RAD;
    const double y = log(tan(MESH_GEO_PI / 4.0 + lat_rad / 2.0));
    out->y = 0.5 - y / (2.0 * MESH_GEO_PI);

    /*
     * Both ends nailed shut. The arithmetic above lands a hair outside the square at the
     * clamped limit - the constant is the rounded decimal degrees, not the exact irrational -
     * and a marker at x or y of 1.0000000004 is one the caller multiplies by a world size and
     * places a pixel off the edge of a world that is supposed to be closed.
     */
    if (out->x < 0.0) {
        out->x = 0.0;
    } else if (out->x > 1.0) {
        out->x = 1.0;
    }
    if (out->y < 0.0) {
        out->y = 0.0;
    } else if (out->y > 1.0) {
        out->y = 1.0;
    }
    return true;
}

void mesh_geo_mercator_inverse(struct mesh_geo_point point, int32_t *out_latitude_i,
                               int32_t *out_longitude_i) {
    /*
     * A NaN or an infinity reaching here is not a coordinate, and neither is what the
     * arithmetic below would make of one. There is no way to report that - the header promises
     * this never fails, because a pan that had to be bounds-tested at every step would push the
     * test into every caller - so the answer is Null Island, which is a real point and is what
     * a viewport with nothing in it already centres on.
     */
    if (!isfinite(point.x) || !isfinite(point.y)) {
        if (out_latitude_i != NULL) {
            *out_latitude_i = 0;
        }
        if (out_longitude_i != NULL) {
            *out_longitude_i = 0;
        }
        return;
    }

    if (out_longitude_i != NULL) {
        /* Wraps rather than clamps: the square's two vertical edges are one meridian, and a map
           panned off the right arrives at the left. */
        const double degrees_i = (point.x - 0.5) * (double)MESH_GEO_LONGITUDE_I_SPAN;
        *out_longitude_i = mesh_geo_longitude_wrap_i((int64_t)llround(degrees_i));
    }

    if (out_latitude_i == NULL) {
        return;
    }
    /* Clamps rather than wraps: north of the top edge is not south of the bottom one, it is
       off the picture, and the display limit is the honest thing to report from there. */
    double y = point.y;
    if (y < 0.0) {
        y = 0.0;
    } else if (y > 1.0) {
        y = 1.0;
    }
    const double mercator_y = (0.5 - y) * (2.0 * MESH_GEO_PI);
    const double lat_rad = 2.0 * atan(exp(mercator_y)) - MESH_GEO_PI / 2.0;
    int64_t latitude_i = llround(lat_rad * MESH_GEO_RAD_TO_FIXED);
    /*
     * And clamped again on the way out, because the round trip through exp() and atan() at the
     * very edge of the square can land a unit past the constant this file clamped forward to -
     * which would hand a caller a latitude its own projection refuses to accept back.
     */
    if (latitude_i > MESH_GEO_MERCATOR_LATITUDE_I_MAX) {
        latitude_i = MESH_GEO_MERCATOR_LATITUDE_I_MAX;
    } else if (latitude_i < -MESH_GEO_MERCATOR_LATITUDE_I_MAX) {
        latitude_i = -MESH_GEO_MERCATOR_LATITUDE_I_MAX;
    }
    *out_latitude_i = (int32_t)latitude_i;
}

/*
 * The equatorial circumference of the WGS 84 ellipsoid: 2 * pi * 6378137 metres.
 *
 * See the header for why this and not the mean radius vector.c measures with. In short: that
 * one is measuring the Earth, this one is measuring the picture, and the picture is cut on this
 * sphere by every tile renderer there is.
 */
#define MESH_GEO_MERCATOR_EQUATOR_M 40075016.6855785

double mesh_geo_mercator_metres_per_world(int32_t latitude_i) {
    if (latitude_i > MESH_GEO_LATITUDE_I_MAX || latitude_i < -MESH_GEO_LATITUDE_I_MAX) {
        return 0.0;
    }
    /* The display clamp again, for the reason forward() applies it: cos() of a quarter turn is
       zero, and a scale bar of zero metres per world is a division nobody wants to find. */
    int32_t clamped = latitude_i;
    if (clamped > MESH_GEO_MERCATOR_LATITUDE_I_MAX) {
        clamped = MESH_GEO_MERCATOR_LATITUDE_I_MAX;
    } else if (clamped < -MESH_GEO_MERCATOR_LATITUDE_I_MAX) {
        clamped = -MESH_GEO_MERCATOR_LATITUDE_I_MAX;
    }
    return MESH_GEO_MERCATOR_EQUATOR_M * cos((double)clamped * MESH_GEO_FIXED_TO_RAD);
}
