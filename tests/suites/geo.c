/*
 * The geography module: the questions every ingress asks about a coordinate, and the picture a
 * map draws one on.
 */

#include "framework/mesh_test.h"

#include "mesh/geo/coords.h"
#include "mesh/geo/mercator.h"

#include <math.h>

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

/* ---- the projection ------------------------------------------------------------------------ */

/*
 * How close is close enough, in fixed-point 1e-7 degrees.
 *
 * Eleven units is about a millimetre of latitude. Every round trip below goes out through a
 * logarithm and back through an exponential, so it cannot be exact, and pinning it at the last
 * bit would be pinning this machine's libm rather than the projection. A millimetre is four
 * orders of magnitude finer than the best fix any node on a mesh reports.
 */
#define GEO_FIXED_EPSILON 11

static bool geo_near(int32_t a, int32_t b) {
    const int64_t diff = (int64_t)a - (int64_t)b;
    return (diff < 0 ? -diff : diff) <= GEO_FIXED_EPSILON;
}

static bool geo_near_unit(double a, double b) { return fabs(a - b) <= 1e-9; }

/*
 * The three points every Web Mercator implementation agrees about, which is what makes them
 * worth pinning: the projection is a contract with a tile pyramid nobody in this tree controls,
 * so the test is against the definition rather than against our own inverse.
 */
MESH_TEST_CASE(geo_mercator_landmarks, unit) {
    struct mesh_geo_point point;

    /* Null Island is the centre of the picture. */
    MESH_TEST_FAIL_IF(!mesh_geo_mercator_forward(0, 0, &point), "null island projects");
    MESH_TEST_FAIL_IF(!geo_near_unit(point.x, 0.5) || !geo_near_unit(point.y, 0.5),
                      "null island is the centre");

    /* The north-west corner of the world: the display limit, on the antimeridian. */
    MESH_TEST_FAIL_IF(!mesh_geo_mercator_forward(MESH_GEO_MERCATOR_LATITUDE_I_MAX,
                                                 -MESH_GEO_LONGITUDE_I_MAX, &point),
                      "the corner projects");
    MESH_TEST_FAIL_IF(!geo_near_unit(point.x, 0.0) || !geo_near_unit(point.y, 0.0),
                      "the display limit is the top-left corner");

    /* And the south-east one. */
    MESH_TEST_FAIL_IF(!mesh_geo_mercator_forward(-MESH_GEO_MERCATOR_LATITUDE_I_MAX,
                                                 MESH_GEO_LONGITUDE_I_MAX, &point),
                      "the far corner projects");
    MESH_TEST_FAIL_IF(!geo_near_unit(point.x, 1.0) || !geo_near_unit(point.y, 1.0),
                      "the southern limit is the bottom-right corner");

    /*
     * 45 degrees north, which is the one latitude with a memorable ordinate: the Mercator y of
     * 45 degrees is ln(tan(67.5 degrees)) = 0.881374, and normalised that is 0.359725. A number
     * from the definition rather than from this file's own forward pass, which is the whole
     * point of a golden value.
     */
    MESH_TEST_FAIL_IF(!mesh_geo_mercator_forward(450000000, 0, &point), "45 north projects");
    MESH_TEST_FAIL_IF(fabs(point.y - 0.3597249) > 1e-6, "45 north lands where Mercator says");

    record_success(test_name);
}

/*
 * Out and back, over the whole range a fix can occupy.
 *
 * This is the property that matters to a map: a marker projected onto the screen and a screen
 * position unprojected back have to name the same place, or a press on a marker opens a
 * different node from the one under the cursor.
 */
MESH_TEST_CASE(geo_mercator_round_trip, unit) {
    const struct {
        const char *label;
        int32_t latitude;
        int32_t longitude;
    } cases[] = {
        {"null island", 0, 0},
        {"a real fix", 476182000, -1223301000},
        {"southern hemisphere", -337000000, 1512000000},
        {"the equator, east", 0, 1000000000},
        {"just inside the display limit", MESH_GEO_MERCATOR_LATITUDE_I_MAX - 1000, 0},
        {"just inside it, southward", -MESH_GEO_MERCATOR_LATITUDE_I_MAX + 1000, 0},
        {"the antimeridian, east", 100000000, MESH_GEO_LONGITUDE_I_MAX},
        {"the antimeridian, west", 100000000, -MESH_GEO_LONGITUDE_I_MAX},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        struct mesh_geo_point point;
        MESH_TEST_FAIL_IF(!mesh_geo_mercator_forward(cases[i].latitude, cases[i].longitude, &point),
                          cases[i].label);
        int32_t latitude = 0;
        int32_t longitude = 0;
        mesh_geo_mercator_inverse(point, &latitude, &longitude);
        MESH_TEST_FAIL_IF(!geo_near(latitude, cases[i].latitude), cases[i].label);
        /*
         * The antimeridian is the one place a round trip may legitimately change the sign: 180
         * east and 180 west are the same meridian, and the wrap keeps whichever end the
         * arithmetic lands on. So the longitude is compared with that identification allowed.
         */
        const bool same = geo_near(longitude, cases[i].longitude);
        const bool antimeridian = geo_near(longitude, -cases[i].longitude) &&
                                  (cases[i].longitude == MESH_GEO_LONGITUDE_I_MAX ||
                                   cases[i].longitude == -MESH_GEO_LONGITUDE_I_MAX);
        MESH_TEST_FAIL_IF(!same && !antimeridian, cases[i].label);
    }

    record_success(test_name);
}

/*
 * The two limits are two different questions, and this is the test that stops them being merged.
 *
 * A fix at 88 degrees north is a real fix - mesh_geo_coords_valid() accepts it, because Svalbard
 * is a place - and what the projection owes it is the top edge of the picture rather than a
 * refusal or an infinity.
 */
MESH_TEST_CASE(geo_mercator_clamps_the_picture_not_the_earth, unit) {
    MESH_TEST_FAIL_IF(!mesh_geo_coords_valid(880000000, 0), "88 north is a place");
    MESH_TEST_FAIL_IF(MESH_GEO_MERCATOR_LATITUDE_I_MAX >= MESH_GEO_LATITUDE_I_MAX,
                      "the display limit is inside the geographic one");

    struct mesh_geo_point beyond;
    struct mesh_geo_point limit;
    MESH_TEST_FAIL_IF(!mesh_geo_mercator_forward(880000000, 0, &beyond), "88 north projects");
    MESH_TEST_FAIL_IF(!mesh_geo_mercator_forward(MESH_GEO_MERCATOR_LATITUDE_I_MAX, 0, &limit),
                      "the limit projects");
    MESH_TEST_FAIL_IF(!geo_near_unit(beyond.y, limit.y), "beyond the limit draws at the limit");
    MESH_TEST_FAIL_IF(beyond.y < 0.0, "and stays on the square");

    /* The pole itself, which is the case that would divide by zero if the clamp were missing. */
    MESH_TEST_FAIL_IF(!mesh_geo_mercator_forward(MESH_GEO_LATITUDE_I_MAX, 0, &beyond),
                      "the pole projects");
    MESH_TEST_FAIL_IF(!geo_near_unit(beyond.y, 0.0), "the pole draws at the top edge");
    MESH_TEST_FAIL_IF(!mesh_geo_mercator_forward(-MESH_GEO_LATITUDE_I_MAX, 0, &beyond),
                      "the south pole projects");
    MESH_TEST_FAIL_IF(!geo_near_unit(beyond.y, 1.0), "and draws at the bottom edge");

    /* And a coordinate that is not a place at all still has no projection. */
    MESH_TEST_FAIL_IF(mesh_geo_mercator_forward(1500000000, 0, &beyond), "200 degrees is refused");

    record_success(test_name);
}

/*
 * The antimeridian, which is where a map that never thought about it goes wrong: panning east
 * off 180 has to arrive at -180 rather than at 181, a longitude every ingress in this client
 * refuses.
 */
MESH_TEST_CASE(geo_longitude_wraps_at_the_antimeridian, unit) {
    const struct {
        const char *label;
        int64_t input;
        int32_t expected;
    } cases[] = {
        {"the prime meridian", 0, 0},
        {"an ordinary longitude", -1223301000, -1223301000},
        {"the antimeridian east", MESH_GEO_LONGITUDE_I_MAX, MESH_GEO_LONGITUDE_I_MAX},
        {"the antimeridian west", -(int64_t)MESH_GEO_LONGITUDE_I_MAX, -MESH_GEO_LONGITUDE_I_MAX},
        /* One unit past the seam comes out one unit the other side of it. */
        {"a step past east", (int64_t)MESH_GEO_LONGITUDE_I_MAX + 1, -MESH_GEO_LONGITUDE_I_MAX + 1},
        {"a step past west", -(int64_t)MESH_GEO_LONGITUDE_I_MAX - 1, MESH_GEO_LONGITUDE_I_MAX - 1},
        /* A whole turn is no move at all, and several turns is still no move: the fold is
           arithmetic rather than a loop, so a caller panning at the finest zoom cannot spin it. */
        {"once round", 2 * (int64_t)MESH_GEO_LONGITUDE_I_MAX + 900000000, 900000000},
        {"five times round", 10 * (int64_t)MESH_GEO_LONGITUDE_I_MAX - 450000000, -450000000},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        MESH_TEST_FAIL_IF(mesh_geo_longitude_wrap_i(cases[i].input) != cases[i].expected,
                          cases[i].label);
    }

    /* And whatever it answers is a longitude the rest of the client will accept. */
    for (int64_t degrees = -720; degrees <= 720; degrees += 7) {
        const int32_t wrapped = mesh_geo_longitude_wrap_i(degrees * 10000000);
        MESH_TEST_FAIL_IF(!mesh_geo_coords_valid(0, wrapped), "a wrapped longitude is valid");
    }

    record_success(test_name);
}

/* An unprojection handed something that is not a number has no answer and must not invent one
   that later divides a viewport by a NaN; Null Island is the stand-in, and it is a real place. */
MESH_TEST_CASE(geo_mercator_inverse_survives_nonsense, unit) {
    int32_t latitude = 12345;
    int32_t longitude = 12345;
    mesh_geo_mercator_inverse((struct mesh_geo_point){.x = NAN, .y = 0.5}, &latitude, &longitude);
    MESH_TEST_FAIL_IF(latitude != 0 || longitude != 0, "a NaN yields null island");

    mesh_geo_mercator_inverse((struct mesh_geo_point){.x = 0.5, .y = INFINITY}, &latitude,
                              &longitude);
    MESH_TEST_FAIL_IF(latitude != 0 || longitude != 0, "an infinity yields null island");

    /* Off the top of the square is off the picture, not round the other side of the world. */
    mesh_geo_mercator_inverse((struct mesh_geo_point){.x = 0.5, .y = -3.0}, &latitude, &longitude);
    MESH_TEST_FAIL_IF(!geo_near(latitude, MESH_GEO_MERCATOR_LATITUDE_I_MAX),
                      "above the square is the northern limit");
    mesh_geo_mercator_inverse((struct mesh_geo_point){.x = 0.5, .y = 4.0}, &latitude, &longitude);
    MESH_TEST_FAIL_IF(!geo_near(latitude, -MESH_GEO_MERCATOR_LATITUDE_I_MAX),
                      "below it is the southern limit");

    /* Off the side is the other side: two whole worlds east is where we started. */
    mesh_geo_mercator_inverse((struct mesh_geo_point){.x = 2.25, .y = 0.5}, &latitude, &longitude);
    MESH_TEST_FAIL_IF(!geo_near(longitude, -900000000), "past the edge wraps round");

    record_success(test_name);
}
