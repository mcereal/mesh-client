/* Coordinates: the one question every ingress asks before it stores a fix. */

#include "framework/mesh_test.h"

#include "mesh/geo/coords.h"

/*
 * The wire type is four times wider than the range a coordinate can occupy, so this is the
 * test that stands between an sfixed32 off the air and a point on a map. It is pinned here
 * rather than at each call site because ingestion, the cache and the keyboard all ask it, and
 * three copies of a bounds check is how they come to disagree about where Earth ends.
 */
MESH_TEST_CASE(geo_coords_range, unit) {
    const struct {
        const char *label;
        int32_t latitude;
        int32_t longitude;
        bool valid;
    } cases[] = {
        /* Null Island is a real point in the Gulf of Guinea. It is also where a node with a
           half-initialised GPS most often claims to be, but rejecting it here would be a guess
           about the sender's firmware dressed up as a range check. */
        {"null island", 0, 0, true},
        {"a real fix", 447654321, -680012345, true},
        /* Both extremes are inclusive: the poles and the antimeridian are places. */
        {"north pole", MESH_GEO_LATITUDE_I_MAX, 0, true},
        {"south pole", -MESH_GEO_LATITUDE_I_MAX, 0, true},
        {"antimeridian east", 0, MESH_GEO_LONGITUDE_I_MAX, true},
        {"antimeridian west", 0, -MESH_GEO_LONGITUDE_I_MAX, true},
        /* One unit past either end is not. */
        {"over the pole", MESH_GEO_LATITUDE_I_MAX + 1, 0, false},
        {"under the pole", -MESH_GEO_LATITUDE_I_MAX - 1, 0, false},
        {"past the antimeridian", 0, MESH_GEO_LONGITUDE_I_MAX + 1, false},
        {"before the antimeridian", 0, -MESH_GEO_LONGITUDE_I_MAX - 1, false},
        /* A longitude is valid at a latitude that is not, and the pair is still rejected:
           the check is about the point, not about either number on its own. */
        {"good longitude, bad latitude", 1500000000, 100, false},
        {"good latitude, bad longitude", 100, 1900000000, false},
        /* What an sfixed32 can actually hold, which is most of what a hostile sender can send. */
        {"int32 max", 2147483647, 2147483647, false},
        {"int32 min", -2147483647 - 1, -2147483647 - 1, false},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        MESH_TEST_FAIL_IF(mesh_geo_coords_valid(cases[i].latitude, cases[i].longitude) !=
                              cases[i].valid,
                          cases[i].label);
    }

    record_success(test_name);
}
