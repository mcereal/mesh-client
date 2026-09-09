/*
 * The map viewport: where it is looking, and what that makes of a coordinate.
 *
 * All of it is arithmetic on four numbers, so all of it is testable with no framebuffer, no
 * radio and no store - which is the whole argument for the module boundary
 * docs/maps-roadmap.md drew. A marker landing in the wrong place is the one map bug a
 * screenshot cannot catch, because a screenshot of a map that is looking somewhere else still
 * looks like a map.
 */

#include "framework/mesh_test.h"

#include "mesh/geo/coords.h"
#include "mesh/map/viewport.h"

#include <math.h>
#include <stdlib.h>

/* A panel-sized box, so the numbers below are the ones the device actually works in. */
#define MAP_TEST_WIDTH 1024
#define MAP_TEST_HEIGHT 600

/* Somewhere with a fix in the capture harness's demo mesh, so the two agree about where "here"
   is when a scene and a test disagree about a frame. */
#define MAP_TEST_LATITUDE 476180000
#define MAP_TEST_LONGITUDE (-1223320000)

static void map_test_viewport(struct mesh_map_viewport *viewport, uint8_t zoom) {
    mesh_map_viewport_init(viewport, MAP_TEST_LATITUDE, MAP_TEST_LONGITUDE, zoom);
    mesh_map_viewport_resize(viewport, MAP_TEST_WIDTH, MAP_TEST_HEIGHT);
}

static bool map_near(int32_t a, int32_t b, int32_t slack) {
    const int64_t diff = (int64_t)a - (int64_t)b;
    return (diff < 0 ? -diff : diff) <= slack;
}

/*
 * The centre of the view is the centre of the box, and the box's axes run the way a screen's do.
 *
 * Three facts in one case because they are one fact: if any of them is wrong every marker on
 * the panel is somewhere else, and which of the three broke is not something a picture reveals.
 */
MESH_TEST_CASE(map_viewport_places_its_own_centre, unit) {
    struct mesh_map_viewport viewport;
    map_test_viewport(&viewport, 14);

    struct mesh_map_placement placement;
    MESH_TEST_FAIL_IF(
        !mesh_map_viewport_place(&viewport, MAP_TEST_LATITUDE, MAP_TEST_LONGITUDE, &placement),
        "the centre places");
    MESH_TEST_FAIL_IF(!map_near(placement.x, MAP_TEST_WIDTH / 2, 1), "and lands mid-panel");
    MESH_TEST_FAIL_IF(!map_near(placement.y, MAP_TEST_HEIGHT / 2, 1), "on both axes");
    MESH_TEST_FAIL_IF(!placement.visible, "and is visible");

    /* North is up and east is right, which is the one convention a north-up map owes the
       reader. A latitude a little larger is a smaller y. */
    struct mesh_map_placement north;
    struct mesh_map_placement east;
    MESH_TEST_FAIL_IF(!mesh_map_viewport_place(&viewport, MAP_TEST_LATITUDE + 100000,
                                               MAP_TEST_LONGITUDE, &north),
                      "north places");
    MESH_TEST_FAIL_IF(!mesh_map_viewport_place(&viewport, MAP_TEST_LATITUDE,
                                               MAP_TEST_LONGITUDE + 100000, &east),
                      "east places");
    MESH_TEST_FAIL_IF(north.y >= placement.y, "north is up");
    MESH_TEST_FAIL_IF(east.x <= placement.x, "east is right");

    record_success(test_name);
}

/*
 * A pixel and the coordinate under it name each other.
 *
 * This is the property a press depends on: the cursor is the middle of the panel, the map asks
 * what is under it, and the marker drawn there has to be the marker that opens. Break the round
 * trip and A opens a different node from the one the reader is looking at - which is not a
 * crash and is not visible in a still.
 */
MESH_TEST_CASE(map_viewport_pixel_and_place_agree, unit) {
    const int32_t probes[][2] = {
        {MAP_TEST_WIDTH / 2, MAP_TEST_HEIGHT / 2}, {0, 0},
        {MAP_TEST_WIDTH - 1, MAP_TEST_HEIGHT - 1}, {17, MAP_TEST_HEIGHT - 40},
        {MAP_TEST_WIDTH - 3, 9},
    };

    for (uint8_t zoom = 4; zoom <= 18; zoom += 7) {
        struct mesh_map_viewport viewport;
        map_test_viewport(&viewport, zoom);
        for (size_t i = 0; i < sizeof probes / sizeof probes[0]; ++i) {
            int32_t latitude = 0;
            int32_t longitude = 0;
            mesh_map_viewport_at(&viewport, probes[i][0], probes[i][1], &latitude, &longitude);
            MESH_TEST_FAIL_IF(!mesh_geo_coords_valid(latitude, longitude),
                              "a pixel names a real place");

            struct mesh_map_placement placement;
            MESH_TEST_FAIL_IF(!mesh_map_viewport_place(&viewport, latitude, longitude, &placement),
                              "and that place places");
            /*
             * A pixel of slack, because the coordinate in between is rounded to 1e-7 degrees -
             * which at zoom 18 is a fifth of a pixel, and at the equator is a centimetre.
             */
            MESH_TEST_FAIL_IF(!map_near(placement.x, probes[i][0], 1), "back to the same column");
            MESH_TEST_FAIL_IF(!map_near(placement.y, probes[i][1], 1), "and the same row");
        }
    }

    record_success(test_name);
}

/* Off the edge is not a failure: it is an answer with `visible` false, because the screen still
   has to be able to say how many markers it is not showing. */
MESH_TEST_CASE(map_viewport_reports_what_is_off_the_panel, unit) {
    struct mesh_map_viewport viewport;
    map_test_viewport(&viewport, 16);

    struct mesh_map_placement placement;
    /* A degree of latitude away is some hundred kilometres, which at zoom 16 is far off the
       top of a 600 px panel. */
    MESH_TEST_FAIL_IF(!mesh_map_viewport_place(&viewport, MAP_TEST_LATITUDE + 10000000,
                                               MAP_TEST_LONGITUDE, &placement),
                      "a distant fix still places");
    MESH_TEST_FAIL_IF(placement.visible, "but is not visible");
    MESH_TEST_FAIL_IF(placement.y >= 0, "and is off the top rather than nowhere");

    /* A coordinate that is not a place has no position at all, which is the one case that is
       a failure. */
    MESH_TEST_FAIL_IF(mesh_map_viewport_place(&viewport, 1500000000, 0, &placement),
                      "200 degrees does not place");

    /* And a viewport with no panel yet answers nothing rather than dividing by a zero box. */
    struct mesh_map_viewport unmeasured;
    mesh_map_viewport_init(&unmeasured, MAP_TEST_LATITUDE, MAP_TEST_LONGITUDE, 14);
    MESH_TEST_FAIL_IF(
        mesh_map_viewport_place(&unmeasured, MAP_TEST_LATITUDE, MAP_TEST_LONGITUDE, &placement),
        "an unmeasured viewport places nothing");
    MESH_TEST_FAIL_IF(mesh_map_viewport_metres_per_pixel(&unmeasured) != 0.0,
                      "and has no scale to report");

    record_success(test_name);
}

/* Panning moves the view the way the box's axes run, and a pan and its opposite cancel. */
MESH_TEST_CASE(map_viewport_pans, unit) {
    struct mesh_map_viewport viewport;
    map_test_viewport(&viewport, 14);
    const int32_t latitude = viewport.center_latitude_i;
    const int32_t longitude = viewport.center_longitude_i;

    MESH_TEST_FAIL_IF(!mesh_map_viewport_pan(&viewport, 200, 0), "east moves");
    MESH_TEST_FAIL_IF(viewport.center_longitude_i <= longitude, "and moves east");
    MESH_TEST_FAIL_IF(viewport.center_latitude_i != latitude, "without moving north or south");

    MESH_TEST_FAIL_IF(!mesh_map_viewport_pan(&viewport, -200, 0), "and back");
    MESH_TEST_FAIL_IF(!map_near(viewport.center_longitude_i, longitude, 2), "to where it started");

    MESH_TEST_FAIL_IF(!mesh_map_viewport_pan(&viewport, 0, 120), "south moves");
    MESH_TEST_FAIL_IF(viewport.center_latitude_i >= latitude, "and moves south");
    MESH_TEST_FAIL_IF(!mesh_map_viewport_pan(&viewport, 0, -120), "and back");
    MESH_TEST_FAIL_IF(!map_near(viewport.center_latitude_i, latitude, 2), "to where it started");

    /* A pan of nothing is not a state change, which is what stops a held d-pad repainting a
       frame that is identical to the one before it. */
    MESH_TEST_FAIL_IF(mesh_map_viewport_pan(&viewport, 0, 0), "a pan of nothing does nothing");

    record_success(test_name);
}

/*
 * The top of the world is a wall, and it is a wall you can walk away from.
 *
 * A pan that clamped without saying so would leave the caller repainting forever; one that
 * wrapped would put a reader who held Up at the north pole out in the southern ocean.
 */
MESH_TEST_CASE(map_viewport_stops_at_the_top_of_the_world, unit) {
    struct mesh_map_viewport viewport;
    mesh_map_viewport_init(&viewport, 840000000, 0, 3);
    mesh_map_viewport_resize(&viewport, MAP_TEST_WIDTH, MAP_TEST_HEIGHT);

    for (int i = 0; i < 200; ++i) {
        (void)mesh_map_viewport_pan(&viewport, 0, -64);
    }
    MESH_TEST_FAIL_IF(viewport.center_latitude_i != MESH_GEO_MERCATOR_LATITUDE_I_MAX,
                      "north stops at the display limit");
    MESH_TEST_FAIL_IF(mesh_map_viewport_pan(&viewport, 0, -64),
                      "and says that nothing moved from there");
    MESH_TEST_FAIL_IF(!mesh_geo_coords_valid(viewport.center_latitude_i,
                                             viewport.center_longitude_i),
                      "the centre is still a place");
    /* And it is a wall in one direction only. */
    MESH_TEST_FAIL_IF(!mesh_map_viewport_pan(&viewport, 0, 64), "southward is still open");

    for (int i = 0; i < 400; ++i) {
        (void)mesh_map_viewport_pan(&viewport, 0, 64);
    }
    MESH_TEST_FAIL_IF(viewport.center_latitude_i != -MESH_GEO_MERCATOR_LATITUDE_I_MAX,
                      "and south stops too");

    record_success(test_name);
}

/*
 * The antimeridian, which is the one place on Earth where a map that never thought about it
 * comes apart - and where nothing but a test would notice, because the failure is invisible
 * everywhere else.
 */
MESH_TEST_CASE(map_viewport_crosses_the_antimeridian, unit) {
    struct mesh_map_viewport viewport;
    /* Fiji-ish: a few hundred metres west of the seam. */
    mesh_map_viewport_init(&viewport, -180000000, 1799990000, 13);
    mesh_map_viewport_resize(&viewport, MAP_TEST_WIDTH, MAP_TEST_HEIGHT);

    /* A neighbour just the other side of the line is a neighbour, not half a world away. */
    struct mesh_map_placement placement;
    MESH_TEST_FAIL_IF(!mesh_map_viewport_place(&viewport, -180000000, -1799990000, &placement),
                      "the neighbour places");
    MESH_TEST_FAIL_IF(!placement.visible, "and is on the panel");
    MESH_TEST_FAIL_IF(placement.x <= MAP_TEST_WIDTH / 2, "just east of the centre");

    /* Panning across the seam keeps the centre a coordinate the rest of the client accepts. */
    for (int i = 0; i < 40; ++i) {
        (void)mesh_map_viewport_pan(&viewport, 64, 0);
        MESH_TEST_FAIL_IF(!mesh_geo_coords_valid(viewport.center_latitude_i,
                                                 viewport.center_longitude_i),
                          "every step across the seam is a place");
    }
    MESH_TEST_FAIL_IF(viewport.center_longitude_i > 0, "and the view came out the western side");

    record_success(test_name);
}

/* Zoom is whole levels, clamped at both ends, and it says when it did nothing. */
MESH_TEST_CASE(map_viewport_zooms_in_whole_levels, unit) {
    struct mesh_map_viewport viewport;
    map_test_viewport(&viewport, 14);
    const int32_t latitude = viewport.center_latitude_i;

    MESH_TEST_FAIL_IF(!mesh_map_viewport_zoom_by(&viewport, 1), "in");
    MESH_TEST_FAIL_IF(viewport.zoom != 15, "one whole level");
    MESH_TEST_FAIL_IF(viewport.center_latitude_i != latitude, "and the centre stays put");

    /* Twice the zoom is half the ground per pixel. */
    const double before = mesh_map_viewport_metres_per_pixel(&viewport);
    MESH_TEST_FAIL_IF(!mesh_map_viewport_zoom_by(&viewport, 1), "in again");
    const double after = mesh_map_viewport_metres_per_pixel(&viewport);
    MESH_TEST_FAIL_IF(fabs(after * 2.0 - before) > before * 1e-9, "each level halves the scale");

    while (mesh_map_viewport_zoom_by(&viewport, 1)) {
    }
    MESH_TEST_FAIL_IF(viewport.zoom != MESH_MAP_ZOOM_MAX, "and stops at the deepest level");
    while (mesh_map_viewport_zoom_by(&viewport, -1)) {
    }
    MESH_TEST_FAIL_IF(viewport.zoom != MESH_MAP_ZOOM_MIN, "and at the shallowest");
    MESH_TEST_FAIL_IF(mesh_map_viewport_zoom_by(&viewport, 0), "a zoom of nothing does nothing");

    record_success(test_name);
}

/* The scale a bar is drawn from: metres per pixel, at the centre, and stretched by Mercator the
   further from the equator the view is. */
MESH_TEST_CASE(map_viewport_reports_its_scale, unit) {
    struct mesh_map_viewport equator;
    mesh_map_viewport_init(&equator, 0, 0, 10);
    mesh_map_viewport_resize(&equator, MAP_TEST_WIDTH, MAP_TEST_HEIGHT);

    /*
     * The number every tile pyramid quotes for zoom 0 at the equator is 156543.03 metres per
     * pixel, so zoom 10 is that over 1024. A golden value from the specification rather than
     * from our own arithmetic, for the reason the projection's landmarks are.
     */
    const double expected = 156543.03392 / 1024.0;
    const double measured = mesh_map_viewport_metres_per_pixel(&equator);
    MESH_TEST_FAIL_IF(fabs(measured - expected) > expected * 1e-6, "zoom 10 at the equator");

    /* And a view at 60 degrees north covers half as much ground per pixel, because cos(60) is
       a half - the stretch that makes Greenland look like Africa. */
    struct mesh_map_viewport north;
    mesh_map_viewport_init(&north, 600000000, 0, 10);
    mesh_map_viewport_resize(&north, MAP_TEST_WIDTH, MAP_TEST_HEIGHT);
    const double up_north = mesh_map_viewport_metres_per_pixel(&north);
    MESH_TEST_FAIL_IF(fabs(up_north * 2.0 - measured) > measured * 1e-6,
                      "sixty degrees north is half the ground per pixel");

    record_success(test_name);
}

/* Framing a set: everything in it lands on the panel, and the zoom is the deepest one that
   manages it. */
MESH_TEST_CASE(map_viewport_fits_a_set_of_points, unit) {
    const int32_t coords[][2] = {
        {476182000, -1223301000},
        {476180000, -1223320000},
        {476580000, -1222800000},
        {476205000, -1223429000},
    };
    const size_t count = sizeof coords / sizeof coords[0];
    const int32_t margin = 24;

    struct mesh_geo_point points[4];
    for (size_t i = 0; i < count; ++i) {
        MESH_TEST_FAIL_IF(!mesh_geo_mercator_forward(coords[i][0], coords[i][1], &points[i]),
                          "the set projects");
    }

    struct mesh_map_viewport viewport;
    map_test_viewport(&viewport, MESH_MAP_ZOOM_DEFAULT);
    MESH_TEST_FAIL_IF(!mesh_map_viewport_fit(&viewport, points, count, margin), "the set frames");

    for (size_t i = 0; i < count; ++i) {
        struct mesh_map_placement placement;
        MESH_TEST_FAIL_IF(!mesh_map_viewport_place(&viewport, coords[i][0], coords[i][1],
                                                   &placement),
                          "each point places");
        MESH_TEST_FAIL_IF(!placement.visible, "and every one of them is on the panel");
        MESH_TEST_FAIL_IF(placement.x < margin || placement.x >= MAP_TEST_WIDTH - margin,
                          "inside the margin horizontally");
        MESH_TEST_FAIL_IF(placement.y < margin || placement.y >= MAP_TEST_HEIGHT - margin,
                          "and vertically");
    }

    /* Deepest, not merely sufficient: one level further in has to push something off. */
    const uint8_t fitted = viewport.zoom;
    MESH_TEST_FAIL_IF(!mesh_map_viewport_zoom_by(&viewport, 1), "there is a level below it");
    bool all_inside = true;
    for (size_t i = 0; i < count; ++i) {
        struct mesh_map_placement placement;
        (void)mesh_map_viewport_place(&viewport, coords[i][0], coords[i][1], &placement);
        if (!placement.visible) {
            all_inside = false;
        }
    }
    MESH_TEST_FAIL_IF(all_inside, "and it does not fit at that level");
    MESH_TEST_FAIL_IF(fitted > MESH_MAP_ZOOM_MAX, "the fitted zoom is a real level");

    record_success(test_name);
}

/*
 * The degenerate sets, each of which has a different right answer.
 *
 * Nothing to frame is a refusal; one point is a place to look at with no span to derive a zoom
 * from, so the caller's zoom stands. A fit that fell to the deepest level on a single marker
 * would put a reader at building scale over a fix reported to the nearest few hundred metres.
 */
MESH_TEST_CASE(map_viewport_fit_handles_thin_sets, unit) {
    struct mesh_map_viewport viewport;
    map_test_viewport(&viewport, 11);

    MESH_TEST_FAIL_IF(mesh_map_viewport_fit(&viewport, NULL, 0, 16), "nothing does not frame");
    MESH_TEST_FAIL_IF(viewport.zoom != 11, "and leaves the view alone");

    struct mesh_geo_point one;
    MESH_TEST_FAIL_IF(!mesh_geo_mercator_forward(511000000, -1200000, &one), "one point projects");
    MESH_TEST_FAIL_IF(!mesh_map_viewport_fit(&viewport, &one, 1U, 16), "one point frames");
    MESH_TEST_FAIL_IF(viewport.zoom != 11, "keeping the zoom it had");
    MESH_TEST_FAIL_IF(!map_near(viewport.center_latitude_i, 511000000, 2), "and centring on it");

    /* Several markers at one place is the same question again: a mesh whose members all report
       the same rounded position has no extent, and no extent is no reason to zoom. */
    const struct mesh_geo_point same[3] = {one, one, one};
    MESH_TEST_FAIL_IF(!mesh_map_viewport_fit(&viewport, same, 3U, 16), "a stack of them frames");
    MESH_TEST_FAIL_IF(viewport.zoom != 11, "and still keeps the zoom");

    record_success(test_name);
}

/*
 * Framing a set that straddles the antimeridian.
 *
 * The bug this exists for: a plain minimum and maximum over longitudes puts the two halves of
 * such a mesh 359 degrees apart, and the frame zooms out to the whole Pacific to "fit" two
 * nodes that can hear each other.
 */
MESH_TEST_CASE(map_viewport_fits_across_the_antimeridian, unit) {
    const int32_t coords[][2] = {
        {-180100000, 1799200000},  /* just west of the seam */
        {-180050000, -1799300000}, /* just east of it */
        {-179900000, 1799600000},
    };
    const size_t count = sizeof coords / sizeof coords[0];

    struct mesh_geo_point points[3];
    for (size_t i = 0; i < count; ++i) {
        MESH_TEST_FAIL_IF(!mesh_geo_mercator_forward(coords[i][0], coords[i][1], &points[i]),
                          "the straddling set projects");
    }

    struct mesh_map_viewport viewport;
    map_test_viewport(&viewport, MESH_MAP_ZOOM_DEFAULT);
    MESH_TEST_FAIL_IF(!mesh_map_viewport_fit(&viewport, points, count, 24), "it frames");

    /*
     * The set spans about a tenth of a degree, so a frame that understood the seam is somewhere
     * around zoom 12 and a frame that did not is down at 1 or 2. Asserting on a range rather
     * than an exact level keeps this a test of the wrap rather than of the panel's dimensions.
     */
    MESH_TEST_FAIL_IF(viewport.zoom < 9, "at a zoom that saw the short way round");

    for (size_t i = 0; i < count; ++i) {
        struct mesh_map_placement placement;
        MESH_TEST_FAIL_IF(!mesh_map_viewport_place(&viewport, coords[i][0], coords[i][1],
                                                   &placement),
                          "each point places");
        MESH_TEST_FAIL_IF(!placement.visible, "and all three are on the panel at once");
    }

    record_success(test_name);
}
