#include "mesh/geo/vector.h"

#include "mesh/geo/coords.h"

#include <math.h>
#include <string.h>

/*
 * The IUGG mean radius. One radius rather than a datum: see the header for why a sphere is
 * enough for every distance this client can be asked about.
 */
#define MESH_GEO_EARTH_RADIUS_M 6371008.8

/*
 * Pi, spelled out.
 *
 * <math.h> has one, but glibc hides M_PI unless a feature-test macro asks for X/Open, and this
 * tree builds -std=c17 with CMAKE_C_EXTENSIONS OFF. Turning that on for one constant would be
 * widening the language the whole client is compiled in to reach a number that has not changed
 * since Archimedes.
 */
#define MESH_GEO_PI 3.14159265358979323846

/* Fixed-point 1e-7 degrees to radians, in one step: the two conversions fold into one constant
   and one multiply, and doing them separately would round twice. */
#define MESH_GEO_FIXED_TO_RAD (MESH_GEO_PI / 1800000000.0)

bool mesh_geo_vector_between(int32_t from_latitude_i, int32_t from_longitude_i,
                             int32_t to_latitude_i, int32_t to_longitude_i,
                             struct mesh_geo_vector *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);

    if (!mesh_geo_coords_valid(from_latitude_i, from_longitude_i) ||
        !mesh_geo_coords_valid(to_latitude_i, to_longitude_i)) {
        return false;
    }

    const double lat1 = (double)from_latitude_i * MESH_GEO_FIXED_TO_RAD;
    const double lon1 = (double)from_longitude_i * MESH_GEO_FIXED_TO_RAD;
    const double lat2 = (double)to_latitude_i * MESH_GEO_FIXED_TO_RAD;
    const double lon2 = (double)to_longitude_i * MESH_GEO_FIXED_TO_RAD;

    const double sin_lat1 = sin(lat1);
    const double cos_lat1 = cos(lat1);
    const double sin_lat2 = sin(lat2);
    const double cos_lat2 = cos(lat2);
    const double d_lat = lat2 - lat1;
    const double d_lon = lon2 - lon1;

    /*
     * Haversine rather than the spherical law of cosines, which is one line shorter and loses
     * its precision at exactly the distances a mesh works over: acos() of a number within a
     * float's last bits of 1.0 is where two nodes a hundred metres apart come out as zero.
     */
    const double sin_half_lat = sin(d_lat / 2.0);
    const double sin_half_lon = sin(d_lon / 2.0);
    const double a =
        sin_half_lat * sin_half_lat + cos_lat1 * cos_lat2 * sin_half_lon * sin_half_lon;
    /* Clamped because rounding can put `a` a hair over 1.0 for antipodal points, and sqrt() of
       the negative that leaves is a NaN travelling all the way to a row of text. */
    const double clamped = a > 1.0 ? 1.0 : (a < 0.0 ? 0.0 : a);
    out->distance_m = 2.0 * MESH_GEO_EARTH_RADIUS_M * atan2(sqrt(clamped), sqrt(1.0 - clamped));

    /*
     * The initial bearing of the great circle, which is the direction to set off in rather than
     * the one to hold: over a mesh's distances the two are the same to well under a compass
     * point, and over a longer one the difference is real and the great circle is the honest
     * answer.
     *
     * Zero displacement is the case with no answer. Comparing the coordinates rather than the
     * distance is deliberate: it is exact, where a distance is the result of the arithmetic
     * above and can be a denormal rather than a zero.
     */
    if (from_latitude_i == to_latitude_i && from_longitude_i == to_longitude_i) {
        return true;
    }
    const double y = sin(d_lon) * cos_lat2;
    const double x = cos_lat1 * sin_lat2 - sin_lat1 * cos_lat2 * cos(d_lon);
    double bearing = atan2(y, x) * (180.0 / MESH_GEO_PI);
    if (bearing < 0.0) {
        bearing += 360.0;
    }
    /* fmod would leave 360.0 reachable through rounding at the seam; a bearing that is not
       less than 360 is one the compass would index off the end of its table. */
    if (bearing >= 360.0) {
        bearing = 0.0;
    }
    out->bearing_deg = bearing;
    out->has_bearing = true;
    return true;
}

enum mesh_geo_compass mesh_geo_compass_of(double bearing_deg) {
    if (!isfinite(bearing_deg)) {
        return MESH_GEO_COMPASS_N;
    }
    double wrapped = fmod(bearing_deg, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    /* Half a point of offset before the divide is what centres each point on its own name:
       without it "north" would run from 0 to 45 and everything would read a point clockwise. */
    const unsigned point = (unsigned)((wrapped + 22.5) / 45.0) % (unsigned)MESH_GEO_COMPASS_COUNT;
    return (enum mesh_geo_compass)point;
}
