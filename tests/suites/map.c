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

#include "mesh/core/session.h"
#include "mesh/geo/coords.h"
#include "mesh/map/viewport.h"
#include "mesh/ui/node_detail.h"

#include <math.h>
#include <stdlib.h>

/* A panel-sized box, so the numbers below are the ones the device actually works in. */
#define MAP_TEST_WIDTH 1024
#define MAP_TEST_HEIGHT 600

/* Somewhere with a fix in the capture harness's demo mesh, so the two agree about where "here"
   is when a scene and a test disagree about a frame. */
#define MAP_TEST_LATITUDE 476180000
#define MAP_TEST_LONGITUDE (-1223320000)

/* The first node number of the wide roster below. Ours, so the self marker is exercised too. */
#define MAP_WIDE_SELF 0x40000000U

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
    MESH_TEST_FAIL_IF(
        !mesh_map_viewport_place(&viewport, MAP_TEST_LATITUDE + 100000, MAP_TEST_LONGITUDE, &north),
        "north places");
    MESH_TEST_FAIL_IF(
        !mesh_map_viewport_place(&viewport, MAP_TEST_LATITUDE, MAP_TEST_LONGITUDE + 100000, &east),
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
        {MAP_TEST_WIDTH / 2, MAP_TEST_HEIGHT / 2},
        {0, 0},
        {MAP_TEST_WIDTH - 1, MAP_TEST_HEIGHT - 1},
        {17, MAP_TEST_HEIGHT - 40},
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
    /* Its scale, though, it does know: metres per pixel is a function of the zoom and the
       latitude, not of the panel - which is what lets the nav and a backend agree about which
       marker is selected without either of them knowing how wide the other's body is. */
    MESH_TEST_FAIL_IF(mesh_map_viewport_metres_per_pixel(&unmeasured) <= 0.0,
                      "but it still knows its own scale");
    map_test_viewport(&viewport, 14);
    mesh_map_viewport_init(&unmeasured, MAP_TEST_LATITUDE, MAP_TEST_LONGITUDE, 14);
    MESH_TEST_FAIL_IF(mesh_map_viewport_metres_per_pixel(&unmeasured) !=
                          mesh_map_viewport_metres_per_pixel(&viewport),
                      "and it is the same scale a measured one reports");

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
    MESH_TEST_FAIL_IF(
        !mesh_geo_coords_valid(viewport.center_latitude_i, viewport.center_longitude_i),
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
        MESH_TEST_FAIL_IF(
            !mesh_geo_coords_valid(viewport.center_latitude_i, viewport.center_longitude_i),
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
        MESH_TEST_FAIL_IF(
            !mesh_map_viewport_place(&viewport, coords[i][0], coords[i][1], &placement),
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
        MESH_TEST_FAIL_IF(
            !mesh_map_viewport_place(&viewport, coords[i][0], coords[i][1], &placement),
            "each point places");
        MESH_TEST_FAIL_IF(!placement.visible, "and all three are on the panel at once");
    }

    record_success(test_name);
}

/* ---- the markers, and the nav that walks them ------------------------------------------------ */

/*
 * The rest of this file is the map as a *screen*: which of the things the client knows about go
 * on it, what a press does, and where B lands. It shares a file with the viewport above because
 * they are one feature, and the split between them is the one the module boundary already draws
 * - everything above is arithmetic with no store in sight, everything below is a store and a nav.
 */

#include "support/ui_fixture.h"

#include "mesh/ui/map.h"
#include "mesh/ui/nav.h"

#include <stdio.h>
#include <string.h>

/* The fixture's roster with fixes on two of its three nodes, and one shared place. Two rather
   than three so that "a node with no position is known and not drawn" has a case. */
static void map_test_populate(struct mesh_ui_store *store) {
    mesh_test_nav_populate(store);

    struct mesh_ui_handshake_state handshake = store->handshake;
    handshake.nodes[0].position.valid = true; /* ourselves */
    handshake.nodes[0].position.latitude_i = MAP_TEST_LATITUDE;
    handshake.nodes[0].position.longitude_i = MAP_TEST_LONGITUDE;
    handshake.nodes[1].position.valid = true; /* ALFA, a few hundred metres off */
    handshake.nodes[1].position.latitude_i = MAP_TEST_LATITUDE + 30000;
    handshake.nodes[1].position.longitude_i = MAP_TEST_LONGITUDE + 30000;
    handshake.nodes[1].position.precision_bits = 16U;
    /* BRVO deliberately has none: a node without a fix is a node the map counts and does not
       draw, which is the ordinary state of most of a real roster. */
    mesh_ui_store_set_handshake(store, &handshake);

    struct mesh_ui_waypoint place;
    memset(&place, 0, sizeof place);
    place.id = 7U;
    place.has_coords = true;
    place.latitude_i = MAP_TEST_LATITUDE - 20000;
    place.longitude_i = MAP_TEST_LONGITUDE + 10000;
    snprintf(place.name, sizeof place.name, "%s", "Cache");
    struct mesh_ui_waypoint_list places;
    memset(&places, 0, sizeof places);
    places.entries[0] = place;
    places.count = 1U;
    mesh_ui_store_set_waypoints(store, &places);
    mesh_ui_store_consume_updates(store, NULL);
}

/* Which of the things the client knows about have somewhere to be drawn - and the two counts
   that are not the same number. */
MESH_TEST_CASE(map_markers_are_the_things_with_a_position, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    map_test_populate(&store);

    struct mesh_ui_map_view view;
    mesh_ui_map_build(&store, &view);

    /* Two nodes with fixes and one place; four nodes-and-places known. */
    MESH_TEST_FAIL_IF(view.count != 3U, "three things have a position");
    MESH_TEST_FAIL_IF(view.known != 4U, "and four are known");
    MESH_TEST_FAIL_IF(!view.has_self, "one of them is our own radio");
    MESH_TEST_FAIL_IF(view.self_index != 0U, "drawn first, so everything else is over it");
    MESH_TEST_FAIL_IF(view.markers[0].kind != MESH_UI_MAP_MARKER_SELF, "and typed as ourselves");
    MESH_TEST_FAIL_IF(strcmp(view.markers[0].label, "ME") != 0, "labelled by its short name");

    MESH_TEST_FAIL_IF(view.markers[1].kind != MESH_UI_MAP_MARKER_NODE, "then the other node");
    MESH_TEST_FAIL_IF(view.markers[1].precision_bits != 16U,
                      "carrying the rounding its sender declared");
    MESH_TEST_FAIL_IF(view.markers[2].kind != MESH_UI_MAP_MARKER_WAYPOINT, "then the place");
    MESH_TEST_FAIL_IF(strcmp(view.markers[2].label, "Cache") != 0, "labelled by its name");

    /* A node the radio has forgotten is still drawn, and says so - the roster deliberately
       outlives the NodeDB, and a marker dropped for that reason would lose a node we know. */
    struct mesh_ui_handshake_state handshake = store.handshake;
    handshake.nodes[1].in_nodedb = false;
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_ui_store_consume_updates(&store, NULL);
    mesh_ui_map_build(&store, &view);
    MESH_TEST_FAIL_IF(view.count != 3U, "a forgotten node keeps its marker");
    MESH_TEST_FAIL_IF(!view.markers[1].stale, "and is marked as one the radio no longer carries");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * A selection is a kind and an id, and it survives the roster being re-ranked under it.
 *
 * docs/maps-roadmap.md's fifth pre-work item named this exact case: "a map-only node may be
 * outside the detail roster" and "publication reorders nodes". A marker under a crosshair is
 * another index into another ordering of the same list, and matching on the number alone is not
 * enough either - a waypoint id is a small counter and a node number is arbitrary, so the two
 * spaces collide by ordinary coincidence rather than by bad luck.
 */
MESH_TEST_CASE(map_selection_names_a_thing_not_a_row, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    map_test_populate(&store);

    struct mesh_ui_map_view view;
    mesh_ui_map_build(&store, &view);

    uint32_t index = 0U;
    MESH_TEST_FAIL_IF(!mesh_ui_map_find(&view, MESH_UI_MAP_MARKER_WAYPOINT, 7U, &index),
                      "the place is findable");
    const int32_t latitude = view.markers[index].latitude_i;

    /* A node numbered the same as the waypoint's id must not answer for it. */
    struct mesh_ui_handshake_state handshake = store.handshake;
    handshake.nodes[1].node_id = 7U;
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_ui_store_consume_updates(&store, NULL);
    mesh_ui_map_build(&store, &view);

    MESH_TEST_FAIL_IF(!mesh_ui_map_find(&view, MESH_UI_MAP_MARKER_WAYPOINT, 7U, &index),
                      "the place is still findable");
    MESH_TEST_FAIL_IF(view.markers[index].kind != MESH_UI_MAP_MARKER_WAYPOINT,
                      "and it is still the place");
    MESH_TEST_FAIL_IF(view.markers[index].latitude_i != latitude, "in the same place");

    uint32_t node_index = 0U;
    MESH_TEST_FAIL_IF(!mesh_ui_map_find(&view, MESH_UI_MAP_MARKER_NODE, 7U, &node_index),
                      "and the node numbered 7 is findable too");
    MESH_TEST_FAIL_IF(node_index == index, "as a different marker");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * Panning is aiming: what A opens is whatever the middle of the view is nearest, and nothing
 * when it is nearest nothing.
 */
MESH_TEST_CASE(map_the_crosshair_selects_by_panning, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    map_test_populate(&store);

    struct mesh_ui_map_view view;
    mesh_ui_map_build(&store, &view);

    struct mesh_map_viewport viewport;
    mesh_map_viewport_init(&viewport, MAP_TEST_LATITUDE, MAP_TEST_LONGITUDE, 16);

    uint32_t index = 0U;
    MESH_TEST_FAIL_IF(!mesh_ui_map_selected(&view, &viewport, &index),
                      "centred on our own radio, it is what is selected");
    MESH_TEST_FAIL_IF(view.markers[index].kind != MESH_UI_MAP_MARKER_SELF, "and it is ourselves");

    /* Pan onto the other node and it takes the selection. */
    (void)mesh_map_viewport_center_on(&viewport, store.handshake.nodes[1].position.latitude_i,
                                      store.handshake.nodes[1].position.longitude_i);
    MESH_TEST_FAIL_IF(!mesh_ui_map_selected(&view, &viewport, &index), "the other node selects");
    MESH_TEST_FAIL_IF(view.markers[index].id != store.handshake.nodes[1].node_id,
                      "and it is that node");

    /* Pan into open country and nothing is selected - which is a state, not a failure. */
    (void)mesh_map_viewport_center_on(&viewport, MAP_TEST_LATITUDE + 5000000,
                                      MAP_TEST_LONGITUDE + 5000000);
    MESH_TEST_FAIL_IF(mesh_ui_map_selected(&view, &viewport, &index), "empty grid selects nothing");

    /*
     * And the radius travels with the zoom, because it is a statement about aiming on a panel.
     * The same view zoomed far enough out has every marker within a few pixels of the middle.
     */
    mesh_map_viewport_init(&viewport, MAP_TEST_LATITUDE, MAP_TEST_LONGITUDE, 2);
    MESH_TEST_FAIL_IF(!mesh_ui_map_selected(&view, &viewport, &index),
                      "zoomed out, the cluster is under the crosshair");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* The Nodes list's first row opens the map, and refuses out loud when there is nothing to put
   on one. */
MESH_TEST_CASE(map_opens_from_the_node_list, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store); /* a roster with no fixes anywhere in it */

    struct mesh_ui_action action;
    store.nav.screen = MESH_UI_SCREEN_NODES;
    MESH_TEST_FAIL_IF(store.nav.cursor[MESH_UI_SCREEN_NODES] != MESH_UI_NODES_MAP_ROW,
                      "the list opens on its map row");
    MESH_TEST_FAIL_IF(mesh_ui_map_has_markers(&store), "and nothing has a position");

    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(store.nav.map_open, "so the press does not open a map");
    MESH_TEST_FAIL_IF(store.nav.toast[0] == '\0', "and says why rather than doing nothing");

    /* Give something a position and the same press opens. */
    struct mesh_ui_handshake_state handshake = store.handshake;
    handshake.nodes[1].position.valid = true;
    handshake.nodes[1].position.latitude_i = MAP_TEST_LATITUDE;
    handshake.nodes[1].position.longitude_i = MAP_TEST_LONGITUDE;
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_ui_store_consume_updates(&store, NULL);

    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(!store.nav.map_open, "now it opens");
    MESH_TEST_FAIL_IF(!mesh_geo_coords_valid(store.nav.map_viewport.center_latitude_i,
                                             store.nav.map_viewport.center_longitude_i),
                      "looking at somewhere real");
    MESH_TEST_FAIL_IF(!map_near(store.nav.map_viewport.center_latitude_i, MAP_TEST_LATITUDE, 200),
                      "framed on the one thing it has");

    /* B goes back to the list, on the row it was opened from. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    MESH_TEST_FAIL_IF(store.nav.map_open, "B closes it");
    MESH_TEST_FAIL_IF(store.nav.cursor[MESH_UI_SCREEN_NODES] != MESH_UI_NODES_MAP_ROW,
                      "landing back on the row that opened it");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * The d-pad moves the world here, and the shoulders still move the tabs.
 *
 * Both halves matter. The first is the whole reason nav_map.c takes its presses ahead of the
 * routing that turns Left and Right into tabs; the second is what stops that costing the reader
 * the tab strip, and it is the reason the two pairs - which are the same press on every other
 * screen in the client - are allowed to part company on this one.
 */
MESH_TEST_CASE(map_the_dpad_moves_the_world, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    map_test_populate(&store);

    struct mesh_ui_action action;
    store.nav.screen = MESH_UI_SCREEN_NODES;
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(!store.nav.map_open, "the map opened");

    const int32_t longitude = store.nav.map_viewport.center_longitude_i;
    const int32_t latitude = store.nav.map_viewport.center_latitude_i;

    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    MESH_TEST_FAIL_IF(store.nav.screen != MESH_UI_SCREEN_NODES,
                      "Right pans rather than changing tab");
    MESH_TEST_FAIL_IF(store.nav.map_viewport.center_longitude_i <= longitude, "and pans east");

    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_UP, &action);
    MESH_TEST_FAIL_IF(store.nav.map_viewport.center_latitude_i <= latitude, "Up pans north");

    /* The shoulders are untouched, so the strip above the body still works from here. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_R1, &action);
    MESH_TEST_FAIL_IF(store.nav.screen == MESH_UI_SCREEN_NODES, "R1 still changes tab");
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_L1, &action);
    MESH_TEST_FAIL_IF(store.nav.screen != MESH_UI_SCREEN_NODES, "and L1 comes back");
    MESH_TEST_FAIL_IF(!store.nav.map_open, "with the map still open");

    /* X and Y are the zoom, in whole levels and clamped. */
    const uint8_t zoom = store.nav.map_viewport.zoom;
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    MESH_TEST_FAIL_IF(store.nav.map_viewport.zoom != zoom + 1U, "X goes one level closer");
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_Y, &action);
    MESH_TEST_FAIL_IF(store.nav.map_viewport.zoom != zoom, "Y goes one level wider");

    /* START frames everything again, however far the view has been walked. */
    for (int i = 0; i < 30; ++i) {
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_START, &action);
    struct mesh_ui_map_view view;
    mesh_ui_map_build(&store, &view);
    for (uint32_t i = 0; i < view.count; ++i) {
        struct mesh_map_placement placement;
        struct mesh_map_viewport drawn = store.nav.map_viewport;
        mesh_map_viewport_resize(&drawn, MESH_UI_MAP_FIT_WIDTH, MESH_UI_MAP_FIT_HEIGHT);
        MESH_TEST_FAIL_IF(!mesh_map_viewport_place(&drawn, view.markers[i].latitude_i,
                                                   view.markers[i].longitude_i, &placement),
                          "each marker places after a fit");
        MESH_TEST_FAIL_IF(!placement.visible, "and every one of them is back on the panel");
    }

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * Puts a marker a given number of pixels from the middle of a view.
 *
 * The map's own arithmetic backwards - mesh_map_viewport_at() answers what coordinate is drawn
 * at a pixel - so a case below can say "300 to the east and 40 down" and mean it on the picture,
 * which is the only space the presses under test are measured in.
 */
static void map_test_marker_at(const struct mesh_map_viewport *viewport,
                               struct mesh_ui_map_view *view, uint32_t id, int32_t dx_px,
                               int32_t dy_px) {
    struct mesh_ui_map_marker *marker = &view->markers[view->count];
    memset(marker, 0, sizeof *marker);
    marker->kind = (uint8_t)MESH_UI_MAP_MARKER_NODE;
    marker->id = id;
    mesh_map_viewport_at(viewport, viewport->width / 2 + dx_px, viewport->height / 2 + dy_px,
                         &marker->latitude_i, &marker->longitude_i);
    (void)mesh_geo_mercator_forward(marker->latitude_i, marker->longitude_i, &marker->point);
    marker->openable = true;
    ++view->count;
}

/*
 * A direction goes to the nearest marker that way, and "that way" is a quadrant.
 *
 * The four 45-degree quadrants tile the plane, which is the property the whole thing rests on:
 * every marker that is not already under the crosshair is ahead of exactly one of the four
 * presses, so there is nothing on the panel a reader cannot reach in the direction it looks
 * like it is in.
 */
MESH_TEST_CASE(map_a_direction_goes_to_the_nearest_marker_that_way, unit) {
    struct mesh_map_viewport viewport;
    map_test_viewport(&viewport, 16);

    struct mesh_ui_map_view view;
    memset(&view, 0, sizeof view);
    map_test_marker_at(&viewport, &view, 1U, 160, 40);  /* east, and further */
    map_test_marker_at(&viewport, &view, 2U, 120, 20);  /* east, and nearer */
    map_test_marker_at(&viewport, &view, 3U, -150, 10); /* west */
    map_test_marker_at(&viewport, &view, 4U, 40, 80);   /* south, not east: |cross| > along */

    uint32_t index = 0U;
    MESH_TEST_FAIL_IF(!mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_EAST, &index),
                      "there is something east");
    MESH_TEST_FAIL_IF(view.markers[index].id != 2U, "and it is the nearer of the two");
    MESH_TEST_FAIL_IF(!mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_WEST, &index),
                      "and something west");
    MESH_TEST_FAIL_IF(view.markers[index].id != 3U, "which is the one drawn west");
    MESH_TEST_FAIL_IF(!mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_SOUTH, &index),
                      "the marker 40 across and 80 down is south");
    MESH_TEST_FAIL_IF(view.markers[index].id != 4U, "rather than east");
    MESH_TEST_FAIL_IF(mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_NORTH, &index),
                      "and nothing at all is north");

    /* Reach: a marker further ahead than one step is left to a pan, which walks it into range
       so the press after it can land. */
    memset(&view, 0, sizeof view);
    map_test_marker_at(&viewport, &view, 5U, MESH_UI_MAP_PAN_STEP_X + 20, 0);
    MESH_TEST_FAIL_IF(mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_EAST, &index),
                      "beyond one step east is out of reach");
    memset(&view, 0, sizeof view);
    map_test_marker_at(&viewport, &view, 6U, MESH_UI_MAP_PAN_STEP_X - 20, 0);
    MESH_TEST_FAIL_IF(!mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_EAST, &index),
                      "just inside one step is not");

    /* And the side of the step is bounded too: a marker within reach ahead but further to the
       side than a press moves the world sideways is a lurch rather than a step, so it is left
       to the pan that heads towards it. */
    memset(&view, 0, sizeof view);
    map_test_marker_at(&viewport, &view, 11U, MESH_UI_MAP_PAN_STEP_X - 10,
                       MESH_UI_MAP_PAN_STEP_Y + 20);
    MESH_TEST_FAIL_IF(mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_EAST, &index),
                      "east does not answer with a marker most of a panel to the south");
    memset(&view, 0, sizeof view);
    map_test_marker_at(&viewport, &view, 12U, MESH_UI_MAP_PAN_STEP_X - 10,
                       MESH_UI_MAP_PAN_STEP_Y - 20);
    MESH_TEST_FAIL_IF(!mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_EAST, &index),
                      "and inside the step's own box it does");

    /* What is already under the crosshair is behind the press rather than ahead of it. */
    memset(&view, 0, sizeof view);
    map_test_marker_at(&viewport, &view, 7U, 0, 0);
    MESH_TEST_FAIL_IF(mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_EAST, &index) ||
                          mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_WEST, &index) ||
                          mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_NORTH, &index) ||
                          mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_SOUTH, &index),
                      "the selected marker is not a destination");

    /*
     * And "under the crosshair" is the disc, not the point. A marker ten pixels ahead of centre
     * is already selected - a fall-back pan stopping short of one leaves exactly that - so a
     * press must advance past it rather than spend itself nudging the view onto something the
     * line under the map is already naming. It is excluded by identity, which is why the marker
     * beside it inside the same disc is still a destination.
     */
    memset(&view, 0, sizeof view);
    map_test_marker_at(&viewport, &view, 8U, 10, 0);  /* selected, and not centred */
    map_test_marker_at(&viewport, &view, 9U, 150, 0); /* the next one east */
    uint32_t aimed = 0U;
    MESH_TEST_FAIL_IF(!mesh_ui_map_selected(&view, &viewport, &aimed) ||
                          view.markers[aimed].id != 8U,
                      "the near marker is the selected one");
    MESH_TEST_FAIL_IF(!mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_EAST, &index),
                      "east still has somewhere to go");
    MESH_TEST_FAIL_IF(view.markers[index].id != 9U,
                      "and it is the next marker, not a nudge onto the selected one");

    map_test_marker_at(&viewport, &view, 10U, 16, 0); /* inside the same disc, not selected */
    MESH_TEST_FAIL_IF(!mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_EAST, &index) ||
                          view.markers[index].id != 10U,
                      "a second marker under the crosshair is still a destination");

    record_success(test_name);
}

/*
 * The other half of aiming: a press between two distant markers explores rather than cycles.
 *
 * This is the correction to the correction. A press that went to the nearest marker in its
 * quadrant *however far away it was* could aim at anything and could look around at nothing: on
 * a mesh with a few dozen positioned nodes every tap of a direction had a marker to answer
 * with, so the view jumped from node to node and the ground between two of them was not merely
 * awkward to stop on, it was unreachable - the reader's own report, "with every dpad tap it
 * just cycles to the next".
 *
 * What bounds it is the step itself: a press moves the world about one pan, whether or not
 * there is something to land on. So the case below walks a marker three steps away into reach
 * one press at a time, and the last press is the one that lands on it exactly - which is both
 * halves at once, a map that can be looked around and a marker that can still be aimed at.
 */
MESH_TEST_CASE(map_a_direction_explores_before_it_lands, unit) {
    struct mesh_map_viewport viewport;
    map_test_viewport(&viewport, 16);

    struct mesh_ui_map_view view;
    memset(&view, 0, sizeof view);
    map_test_marker_at(&viewport, &view, 1U, MESH_UI_MAP_PAN_STEP_X * 3, 0);

    /* Three steps east is nothing a press may land on, so the first two presses are pans - and
       a pan is what the reader wanted: the view moves over the ground in between. */
    uint32_t index = 0U;
    for (int press = 0; press < 2; ++press) {
        MESH_TEST_FAIL_IF(mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_EAST, &index),
                          "a marker further off than one step is not a destination yet");
        MESH_TEST_FAIL_IF(!mesh_map_viewport_pan(&viewport, MESH_UI_MAP_PAN_STEP_X, 0),
                          "so the press pans instead");
        MESH_TEST_FAIL_IF(mesh_ui_map_selected(&view, &viewport, &index),
                          "and stops on open ground, which is what exploring is");
    }

    /* Now it is within the step, and the press that finds it lands on its own coordinates. */
    MESH_TEST_FAIL_IF(!mesh_ui_map_step(&view, &viewport, MESH_UI_MAP_EAST, &index),
                      "the third press has it in reach");
    MESH_TEST_FAIL_IF(view.markers[index].id != 1U, "and it is the marker walked up to");
    MESH_TEST_FAIL_IF(!mesh_map_viewport_center_on(&viewport, view.markers[index].latitude_i,
                                                   view.markers[index].longitude_i),
                      "the view centres on it");
    uint32_t selected = 0U;
    MESH_TEST_FAIL_IF(!mesh_ui_map_selected(&view, &viewport, &selected) ||
                          view.markers[selected].id != 1U,
                      "so the marker the press walked to is the one under the crosshair");

    record_success(test_name);
}

/*
 * The regression this exists for: the crosshair can be put on a marker a pan could never reach.
 *
 * The first half of the case is the bug, written down. A pan of a fixed number of pixels only
 * ever visits a lattice a fifth of the body wide and a fifth of it tall, and the crosshair
 * captures a disc of MESH_UI_MAP_SELECT_RADIUS_PX - so a marker whose offset falls between the
 * lattice's points is unselectable at that zoom no matter how long the reader pans, and about
 * five markers in six are. On the device that reads as "the crosshair skips over it", and
 * zooming appears to help only because it re-phases the lattice.
 *
 * The loop below asserts that unreachability against the old rule rather than describing it,
 * which is what stops a future press-sized pan from quietly bringing it back.
 */
MESH_TEST_CASE(map_the_dpad_reaches_what_a_pan_could_not, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    map_test_populate(&store);

    struct mesh_ui_action action;
    store.nav.screen = MESH_UI_SCREEN_NODES;
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(!store.nav.map_open, "the map opened");

    /* Standing on our own radio, at a zoom a reader would actually use. */
    store.nav.map_viewport.zoom = 16U;
    (void)mesh_map_viewport_center_on(&store.nav.map_viewport, MAP_TEST_LATITUDE,
                                      MAP_TEST_LONGITUDE);

    /*
     * BRVO, put 90 pixels east and 30 south of the crosshair: off the lattice in both axes, by
     * more than the capture radius in each. Contrived only in being exact - between two lattice
     * points is where most of the panel is.
     */
    int32_t latitude_i = 0;
    int32_t longitude_i = 0;
    mesh_map_viewport_at(&store.nav.map_viewport, store.nav.map_viewport.width / 2 + 90,
                         store.nav.map_viewport.height / 2 + 30, &latitude_i, &longitude_i);
    struct mesh_ui_handshake_state handshake = store.handshake;
    handshake.nodes[2].position.valid = true;
    handshake.nodes[2].position.latitude_i = latitude_i;
    handshake.nodes[2].position.longitude_i = longitude_i;
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_ui_store_consume_updates(&store, NULL);

    struct mesh_ui_map_view view;
    mesh_ui_map_build(&store, &view);
    uint32_t target = 0U;
    MESH_TEST_FAIL_IF(!mesh_ui_map_find(&view, MESH_UI_MAP_MARKER_NODE, 0x3000U, &target),
                      "the node is on the map");

    /* Every place a fixed pan could ever leave the crosshair, out to six presses each way. */
    for (int across = -6; across <= 6; ++across) {
        for (int down = -6; down <= 6; ++down) {
            struct mesh_map_viewport lattice = store.nav.map_viewport;
            (void)mesh_map_viewport_pan(&lattice, across * (MESH_UI_MAP_FIT_WIDTH / 5),
                                        down * (MESH_UI_MAP_FIT_HEIGHT / 5));
            double dx = 0.0;
            double dy = 0.0;
            MESH_TEST_FAIL_IF(!mesh_map_viewport_offset(&lattice, view.markers[target].latitude_i,
                                                        view.markers[target].longitude_i, &dx, &dy),
                              "the marker places from every one of them");
            MESH_TEST_FAIL_IF(dx * dx + dy * dy <= (double)MESH_UI_MAP_SELECT_RADIUS_PX *
                                                       (double)MESH_UI_MAP_SELECT_RADIUS_PX,
                              "no fixed pan ever brings this marker under the crosshair");
        }
    }

    /* One press east, and the view is on it exactly - which is what makes the aim exact rather
       than merely better. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    MESH_TEST_FAIL_IF(store.nav.map_viewport.center_latitude_i != latitude_i ||
                          store.nav.map_viewport.center_longitude_i != longitude_i,
                      "the press lands on the marker's own coordinates");

    mesh_ui_map_build(&store, &view);
    uint32_t selected = 0U;
    MESH_TEST_FAIL_IF(!mesh_ui_map_selected(&view, &store.nav.map_viewport, &selected),
                      "so something is under the crosshair");
    MESH_TEST_FAIL_IF(view.markers[selected].id != 0x3000U, "and it is the node aimed at");

    /* And A opens it, which is the whole point of being able to aim. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(!store.nav.node_detail_open || store.nav.node_detail_node != 0x3000U,
                      "the press that was impossible before opens that node");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * With nothing that way, the same press still pans - by the step, exactly as it always did.
 *
 * The fallback is what keeps "look west" honest over open grid, and it is what walks a marker
 * beyond the step's reach into it. A map that only moved between markers would be a map that
 * could not be looked around, which is most of what the reader does once there is a basemap
 * under it.
 */
MESH_TEST_CASE(map_a_direction_pans_when_nothing_is_that_way, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    map_test_populate(&store);

    struct mesh_ui_action action;
    store.nav.screen = MESH_UI_SCREEN_NODES;
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(!store.nav.map_open, "the map opened");

    /* Well out in open country, with the whole roster behind us to the east. */
    store.nav.map_viewport.zoom = 16U;
    (void)mesh_map_viewport_center_on(&store.nav.map_viewport, MAP_TEST_LATITUDE,
                                      MAP_TEST_LONGITUDE - 50000000);

    struct mesh_map_viewport expected = store.nav.map_viewport;
    MESH_TEST_FAIL_IF(!mesh_map_viewport_pan(&expected, -(MESH_UI_MAP_FIT_WIDTH / 5), 0),
                      "a step west is a real move");

    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    MESH_TEST_FAIL_IF(store.nav.map_viewport.center_longitude_i != expected.center_longitude_i ||
                          store.nav.map_viewport.center_latitude_i != expected.center_latitude_i,
                      "and the press is exactly that step");

    struct mesh_ui_map_view view;
    mesh_ui_map_build(&store, &view);
    uint32_t index = 0U;
    MESH_TEST_FAIL_IF(mesh_ui_map_selected(&view, &store.nav.map_viewport, &index),
                      "with nothing under the crosshair when it stops");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * A opens what is under the crosshair, and B comes back to the map rather than to the list.
 *
 * The second half is the one worth a test: a node opened from a map and closed onto the node
 * list is a reader losing the view they had panned to, and the only thing that prevents it is
 * the map staying open underneath the detail.
 */
MESH_TEST_CASE(map_opens_a_marker_and_comes_back_to_the_map, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    map_test_populate(&store);

    struct mesh_ui_action action;
    store.nav.screen = MESH_UI_SCREEN_NODES;
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* the map row */
    MESH_TEST_FAIL_IF(!store.nav.map_open, "the map opened");

    /* Onto ALFA, whose fix the fixture put a few hundred metres north-east. */
    const uint32_t alfa = store.handshake.nodes[1].node_id;
    (void)mesh_map_viewport_center_on(&store.nav.map_viewport,
                                      store.handshake.nodes[1].position.latitude_i,
                                      store.handshake.nodes[1].position.longitude_i);
    store.nav.map_viewport.zoom = 16U;

    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(!store.nav.node_detail_open, "A opened the marker under the crosshair");
    MESH_TEST_FAIL_IF(store.nav.node_detail_node != alfa, "and it is that node");
    MESH_TEST_FAIL_IF(!store.nav.map_open, "with the map still underneath it");
    /*
     * And on a row the cursor may stand on, which is the marker's door into a screen whose row
     * 0 is the actions group's heading. This press does not go through nav.c's own open, so it
     * is the one that can quietly go back to writing 0 and land the reader on a title where A
     * does nothing - the exact state the heading skip exists to remove.
     */
    {
        struct mesh_ui_node_item rows[MESH_UI_NODE_ITEMS_MAX];
        const struct mesh_ui_node_summary *opened =
            mesh_ui_node_detail_find(&store.handshake, store.nav.node_detail_node);
        const uint32_t rowc =
            mesh_ui_node_detail_build(opened, false, 0U, &store.traceroute, false, &store.handshake,
                                      NULL, rows, MESH_UI_NODE_ITEMS_MAX);
        const uint32_t at = store.nav.cursor[MESH_UI_SCREEN_NODES];
        MESH_TEST_FAIL_IF(at >= rowc || rows[at].kind == MESH_UI_NODE_ROW_HEADING,
                          "and on a row the cursor may stand on, not the heading above it");
    }

    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    MESH_TEST_FAIL_IF(store.nav.node_detail_open, "B closes the detail");
    MESH_TEST_FAIL_IF(!store.nav.map_open, "back onto the map it was opened from");

    /* A place goes to the tab that owns places, which is a change of tab and does not come
       back - the node detail's "Message this node" makes the same move for the same reason. */
    (void)mesh_map_viewport_center_on(&store.nav.map_viewport, MAP_TEST_LATITUDE - 20000,
                                      MAP_TEST_LONGITUDE + 10000);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(store.nav.screen != MESH_UI_SCREEN_WAYPOINTS, "a place opens on its own tab");
    MESH_TEST_FAIL_IF(!store.nav.waypoint_detail_open || store.nav.waypoint_detail_id != 7U,
                      "showing that place");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* A map with nothing left to draw closes itself, the way a node detail whose node has gone
   does - a screen of empty graticule reads as a map panned into the ocean, not as an empty one. */
MESH_TEST_CASE(map_closes_when_there_is_nothing_left_to_draw, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    map_test_populate(&store);

    struct mesh_ui_action action;
    store.nav.screen = MESH_UI_SCREEN_NODES;
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(!store.nav.map_open, "the map opened");

    struct mesh_ui_handshake_state handshake;
    memset(&handshake, 0, sizeof handshake);
    handshake.has_my_info = true;
    handshake.my_info.node_num = 0x1000U;
    mesh_ui_store_set_handshake(&store, &handshake);
    struct mesh_ui_waypoint_list places;
    memset(&places, 0, sizeof places);
    mesh_ui_store_set_waypoints(&store, &places);

    /* A real snapshot, because that is where the clamp runs: mesh_ui_store_consume_updates()
       refuses a NULL one before it gets there, so a test that passed NULL would be asserting
       that nothing happened rather than that the right thing did. */
    static struct mesh_ui_snapshot snapshot;
    MESH_TEST_FAIL_IF(!mesh_ui_store_consume_updates(&store, &snapshot), "a frame was published");

    MESH_TEST_FAIL_IF(store.nav.map_open, "an emptied roster closes the map");
    MESH_TEST_FAIL_IF(snapshot.nav.map_open, "and the frame drawn from it shows the list");
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * The map's presses belong to the map's own screen, and to nothing else.
 *
 * `map_open` deliberately survives a change of tab - every tab in this client keeps its own
 * place, and coming back to Nodes should show the view that was left. What must not survive is
 * the *key handling*: nav_map.c takes the d-pad ahead of the routing that turns Left and Right
 * into tabs, so a guard that asked only whether a map was open somewhere would have the arrows,
 * A, B, X, Y and START acting on a screen the reader cannot see.
 *
 * Two ways in, and the second is the worse one. A shoulder press walks off the Nodes tab with
 * the map still open underneath it; and A on a waypoint marker jumps to the Waypoints tab, where
 * the first B would close the hidden map rather than the place that is actually on the panel.
 */
MESH_TEST_CASE(map_keys_belong_to_the_screen_the_map_is_on, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    map_test_populate(&store);

    struct mesh_ui_action action;
    store.nav.screen = MESH_UI_SCREEN_NODES;
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(!store.nav.map_open, "the map opened");

    /* Off to the next tab, with the map still open behind us - which is the point. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_R1, &action);
    MESH_TEST_FAIL_IF(store.nav.screen == MESH_UI_SCREEN_NODES, "R1 left the Nodes tab");
    MESH_TEST_FAIL_IF(!store.nav.map_open, "and the map is still open behind it");

    const enum mesh_ui_screen elsewhere = store.nav.screen;
    const int32_t longitude = store.nav.map_viewport.center_longitude_i;
    const uint8_t zoom = store.nav.map_viewport.zoom;

    /* Now every press the map claims has to belong to the tab in front of the reader. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    MESH_TEST_FAIL_IF(store.nav.map_viewport.center_longitude_i != longitude,
                      "Right on another tab must not pan the hidden map");
    MESH_TEST_FAIL_IF(store.nav.screen == elsewhere, "it moves along the tabs, as it always does");

    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_LEFT, &action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    MESH_TEST_FAIL_IF(store.nav.map_viewport.zoom != zoom,
                      "X on another tab must not zoom the hidden map");
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    MESH_TEST_FAIL_IF(!store.nav.map_open, "B on another tab must not close the hidden map");

    /* And back onto Nodes, where the map is what is on the panel and the presses are its own. */
    while (store.nav.screen != MESH_UI_SCREEN_NODES) {
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_L1, &action);
    }
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_X, &action);
    MESH_TEST_FAIL_IF(store.nav.map_viewport.zoom != zoom + 1U, "X on the map zooms it again");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* The same guard, reached the other way: a place opened from the map lands on the Waypoints tab,
   and the first B there has to close the place rather than the map left behind on Nodes. */
MESH_TEST_CASE(map_hands_the_keys_over_when_a_place_opens, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    map_test_populate(&store);

    struct mesh_ui_action action;
    store.nav.screen = MESH_UI_SCREEN_NODES;
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(!store.nav.map_open, "the map opened");

    (void)mesh_map_viewport_center_on(&store.nav.map_viewport, MAP_TEST_LATITUDE - 20000,
                                      MAP_TEST_LONGITUDE + 10000);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(store.nav.screen != MESH_UI_SCREEN_WAYPOINTS, "the place opened on its tab");
    MESH_TEST_FAIL_IF(!store.nav.waypoint_detail_open, "showing that place");

    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    MESH_TEST_FAIL_IF(store.nav.waypoint_detail_open,
                      "B closes the place that is on the panel, not the map behind it");
    MESH_TEST_FAIL_IF(!store.nav.map_open, "and the map is still where it was left");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * A marker under the crosshair is selectable, including the ones the projection had to clamp.
 *
 * A fix at 88 degrees north is real - mesh_geo_coords_valid() accepts it, because Svalbard is a
 * place - and the projection draws it at the top edge of the picture rather than refusing it.
 * That clamp is what makes this case: the marker is *drawn* at the display limit, so a view
 * framed on it puts it exactly under the crosshair, while the coordinate it carries is still
 * three degrees further north. A selection measured between the two coordinates therefore reads
 * hundreds of kilometres and refuses a marker the reader can see dead centre.
 *
 * The fix is that the selection is measured where the marker is *drawn* - in the projection,
 * the same arithmetic the renderer does - rather than across the ground. That keeps the ring and
 * the press one decision in the one case where the ground and the picture disagree.
 */
MESH_TEST_CASE(map_selects_a_marker_the_projection_had_to_clamp, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_handshake_state handshake = store.handshake;
    handshake.nodes[1].position.valid = true;
    handshake.nodes[1].position.latitude_i = 880000000; /* well past the display limit */
    handshake.nodes[1].position.longitude_i = 150000000;
    mesh_ui_store_set_handshake(&store, &handshake);
    mesh_ui_store_consume_updates(&store, NULL);

    struct mesh_ui_map_view view;
    mesh_ui_map_build(&store, &view);
    MESH_TEST_FAIL_IF(view.count != 1U, "the far northern node is a marker");

    struct mesh_map_viewport viewport;
    mesh_map_viewport_init(&viewport, 0, 0, MESH_MAP_ZOOM_DEFAULT);
    mesh_map_viewport_resize(&viewport, MESH_UI_MAP_FIT_WIDTH, MESH_UI_MAP_FIT_HEIGHT);
    struct mesh_geo_point points[MESH_UI_MAP_MARKERS_MAX];
    const uint32_t count = mesh_ui_map_points(&view, points, MESH_UI_MAP_MARKERS_MAX);
    MESH_TEST_FAIL_IF(!mesh_map_viewport_fit(&viewport, points, (size_t)count, 0), "it frames");

    /* Drawn dead centre, because both the marker and the view's own centre clamp to the same
       parallel - which is exactly why the two coordinates no longer match. */
    struct mesh_map_placement placement;
    MESH_TEST_FAIL_IF(!mesh_map_viewport_place(&viewport, view.markers[0].latitude_i,
                                               view.markers[0].longitude_i, &placement),
                      "the marker places");
    MESH_TEST_FAIL_IF(!map_near(placement.x, MESH_UI_MAP_FIT_WIDTH / 2, 2) ||
                          !map_near(placement.y, MESH_UI_MAP_FIT_HEIGHT / 2, 2),
                      "and lands under the crosshair");
    MESH_TEST_FAIL_IF(viewport.center_latitude_i == view.markers[0].latitude_i,
                      "while the two coordinates genuinely differ");

    uint32_t index = 0U;
    MESH_TEST_FAIL_IF(!mesh_ui_map_selected(&view, &viewport, &index),
                      "so a marker on the crosshair is what A opens");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* ---- the map's own roster ----------------------------------------------------------------------
 */

/*
 * The two rosters are two sizes, and the map's is the session's.
 *
 * store.h is nanopb-free by construction and mesh/core/session.h is not, so the map's cap is a
 * second declaration of the session's - the waypoint limits' arrangement, and it needs the
 * waypoint limits' check. The inequality below is the one that matters: if the map's roster
 * ever narrowed to the list's, every marker this file is about would quietly disappear again
 * and nothing else would fail.
 */
MESH_TEST_CASE(map_roster_agrees_across_the_seam, unit) {
    MESH_TEST_FAIL_IF(MESH_UI_MAX_MAP_NODES != MESH_SESSION_MAX_NODES,
                      "the map should hold every node the session can");
    MESH_TEST_FAIL_IF(MESH_UI_MAX_MAP_NODES <= MESH_UI_MAX_HANDSHAKE_NODES,
                      "and more of them than the ranked rows carry, or this bought nothing");
    MESH_TEST_FAIL_IF(MESH_UI_MAP_MARKERS_MAX != MESH_UI_MAX_MAP_NODES + MESH_UI_MAX_WAYPOINTS,
                      "so the marker set holds that roster and the whole waypoint book");
    record_success(test_name);
}

/* A roster of `count` positioned nodes a few hundred metres apart, ourselves first, of which
   the first `rows` are the ones the ranking published. */
static void map_test_wide_roster(struct mesh_ui_handshake_state *hs, uint32_t count,
                                 uint32_t rows) {
    memset(hs, 0, sizeof *hs);
    hs->config_complete = true;
    hs->has_my_info = true;
    hs->my_info.node_num = MAP_WIDE_SELF;
    hs->nodes_known = count;

    hs->node_count = rows > MESH_UI_MAX_HANDSHAKE_NODES ? MESH_UI_MAX_HANDSHAKE_NODES : rows;
    for (uint32_t i = 0; i < hs->node_count; ++i) {
        hs->nodes[i].node_id = MAP_WIDE_SELF + i;
        hs->nodes[i].in_nodedb = true;
        snprintf(hs->nodes[i].short_name, sizeof hs->nodes[i].short_name, "N%03u", i % 1000U);
    }

    hs->map_node_count = count;
    for (uint32_t i = 0; i < count; ++i) {
        struct mesh_ui_map_node *node = &hs->map_nodes[i];
        node->node_id = MAP_WIDE_SELF + i;
        node->latitude_i = MAP_TEST_LATITUDE + (int32_t)i * 3000;
        node->longitude_i = MAP_TEST_LONGITUDE + (int32_t)i * 3000;
        node->in_nodedb = true;
        node->has_row = (i < hs->node_count);
        snprintf(node->label, sizeof node->label, "N%03u", i % 1000U);
    }
}

/*
 * The map draws nodes the list never published, which is the whole of the change.
 *
 * docs/maps-roadmap.md's fourth pre-work item left this open and its own §"What steps 1 and 2
 * became" called it "the largest single thing between this map and the one this document
 * describes": the session holds MESH_SESSION_MAX_NODES and the ranking publishes 128 rows, and
 * the map was drawing the rows. A node's rank says how likely you are to talk to it, which has
 * nothing to do with whether its marker belongs on the panel.
 */
MESH_TEST_CASE(map_draws_nodes_the_list_never_published, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_handshake_state hs;
    map_test_wide_roster(&hs, 200U, MESH_UI_MAX_HANDSHAKE_NODES);
    mesh_ui_store_set_handshake(&store, &hs);
    mesh_ui_store_consume_updates(&store, NULL);

    struct mesh_ui_map_view view;
    mesh_ui_map_build(&store, &view);

    MESH_TEST_FAIL_IF(view.count != 200U, "every positioned node the session holds is a marker");
    MESH_TEST_FAIL_IF(view.known != 200U, "and the badge counts the roster, not the rows");
    MESH_TEST_FAIL_IF(!view.has_self || view.self_index != 0U,
                      "our own radio is still drawn first, under everything else");

    /* The 199th, which no row carries: the marker the old map was missing. */
    uint32_t index = 0U;
    MESH_TEST_FAIL_IF(
        !mesh_ui_map_find(&view, MESH_UI_MAP_MARKER_NODE, MAP_WIDE_SELF + 199U, &index),
        "including one ranked far below the list's cut");
    MESH_TEST_FAIL_IF(strcmp(view.markers[index].label, "N199") != 0,
                      "labelled from the map's own roster rather than from a row it has none of");
    MESH_TEST_FAIL_IF(mesh_ui_node_detail_find(&store.handshake, MAP_WIDE_SELF + 199U) != NULL,
                      "and it genuinely has no row - otherwise this test proves nothing");
    MESH_TEST_FAIL_IF(view.markers[index].openable, "so the map says it cannot be opened");

    /* One inside the cut, to show the flag is a fact about the node and not about the map. */
    MESH_TEST_FAIL_IF(!mesh_ui_map_find(&view, MESH_UI_MAP_MARKER_NODE, MAP_WIDE_SELF + 5U, &index),
                      "a node the list did publish is also a marker");
    MESH_TEST_FAIL_IF(!view.markers[index].openable, "and that one opens");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * A handshake nobody published still draws its rows.
 *
 * A roster loaded from the cache before the first publish, a hand-built fixture, the capture
 * harness: none of them fills the map's roster, and the rows are the best any of them has. The
 * fallback is what stops a second array being something a producer has to remember - the
 * failure that would cause is a map that is silently empty, which no build and no screenshot
 * would catch.
 */
MESH_TEST_CASE(map_falls_back_to_the_published_rows, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    map_test_populate(&store);

    struct mesh_ui_handshake_state hs = store.handshake;
    MESH_TEST_FAIL_IF(hs.map_node_count != 0U, "the fixture publishes no map roster");
    mesh_ui_store_set_handshake(&store, &hs);
    mesh_ui_store_consume_updates(&store, NULL);

    struct mesh_ui_map_view view;
    mesh_ui_map_build(&store, &view);
    MESH_TEST_FAIL_IF(view.count != 3U, "the rows' own positions are still drawn");
    MESH_TEST_FAIL_IF(!mesh_ui_map_has_markers(&store),
                      "and the row that offers the map agrees there is something on it");
    MESH_TEST_FAIL_IF(!view.markers[0].openable || !view.markers[1].openable,
                      "every one of them opens: the fallback source is the rows themselves");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * A on a node the client cannot open does nothing, and does it cleanly.
 *
 * The alternative is what happens without the guard: the detail opens, mesh_ui_nav_clamp()
 * cannot resolve the id against the rows and closes it on the same frame, and the list cursor
 * the press reset on its way past stays reset. A dead press that also moves something is worse
 * than a dead press, and this screen already has a deliberate one - A on empty grid - so the
 * silence is the established answer rather than a new one.
 */
MESH_TEST_CASE(map_press_refuses_a_node_it_cannot_open, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct mesh_ui_handshake_state hs;
    map_test_wide_roster(&hs, 200U, MESH_UI_MAX_HANDSHAKE_NODES);
    mesh_ui_store_set_handshake(&store, &hs);
    mesh_ui_store_consume_updates(&store, NULL);

    struct mesh_ui_action action;
    store.nav.screen = MESH_UI_SCREEN_NODES;
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action); /* the map row */
    MESH_TEST_FAIL_IF(!store.nav.map_open, "the map opened");

    /* Aimed at the last node, which is far enough down the ranking to have no row. */
    (void)mesh_map_viewport_center_on(&store.nav.map_viewport, hs.map_nodes[199].latitude_i,
                                      hs.map_nodes[199].longitude_i);
    store.nav.map_viewport.zoom = 16U;

    struct mesh_ui_map_view view;
    mesh_ui_map_build(&store, &view);
    uint32_t index = 0U;
    MESH_TEST_FAIL_IF(!mesh_ui_map_selected(&view, &store.nav.map_viewport, &index),
                      "the crosshair is on a marker");
    MESH_TEST_FAIL_IF(view.markers[index].id != MAP_WIDE_SELF + 199U, "and on the right one");

    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(store.nav.node_detail_open, "A opens no detail it cannot fill");
    MESH_TEST_FAIL_IF(!store.nav.map_open, "and leaves the reader on the map");
    /*
     * And records nothing on the way past. Without the guard the press writes the id, the
     * detail opens, and mesh_ui_nav_clamp() closes it again on the same frame - which looks
     * identical on the two flags above and leaves the nav naming a node it is not showing.
     */
    MESH_TEST_FAIL_IF(store.nav.node_detail_node == MAP_WIDE_SELF + 199U,
                      "and aims the detail at nothing");

    /* The same press on a node the list did publish still opens its detail, so the guard is
       about the node and not about the map. */
    (void)mesh_map_viewport_center_on(&store.nav.map_viewport, hs.map_nodes[5].latitude_i,
                                      hs.map_nodes[5].longitude_i);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF(!store.nav.node_detail_open, "a node with a row still opens");
    MESH_TEST_FAIL_IF(store.nav.node_detail_node != MAP_WIDE_SELF + 5U, "and it is that node");

    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * The span and the placement are two readings of one view, so they have to land on the same
 * pixel.
 *
 * This is the case that catches every off-by-one in the tile arithmetic at once. Blit the span
 * the way a renderer will - the first tile's corner at (origin_x, origin_y), each one after it a
 * tile further along - and the world pixel under the middle of the panel must be the same one
 * mesh_map_viewport_place() puts the centre's own coordinate at. A span shifted by a tile, or a
 * corner computed with a truncation instead of a floor, still looks like a map; it is a map of
 * somewhere 256 metres away.
 */
MESH_TEST_CASE(map_tile_span_agrees_with_a_placement, unit) {
    for (uint8_t zoom = 10U; zoom <= 18U; ++zoom) {
        struct mesh_map_viewport viewport;
        map_test_viewport(&viewport, zoom);

        struct mesh_map_tile_span span;
        MESH_TEST_FAIL_IF(!mesh_map_viewport_tiles(&viewport, &span), "a panel covers tiles");
        MESH_TEST_FAIL_IF(span.zoom != zoom, "at the zoom it was asked about");
        MESH_TEST_FAIL_IF(span.origin_x > 0 || span.origin_x <= -MESH_MAP_TILE_SIZE,
                          "the first column starts at or before the box's own corner");
        MESH_TEST_FAIL_IF(span.origin_y > 0 || span.origin_y <= -MESH_MAP_TILE_SIZE,
                          "and so does the first row, away from the world's edge");

        /* The span has to cover the box: the last tile's far edge is at or past the far corner. */
        MESH_TEST_FAIL_IF(span.origin_x + span.columns * MESH_MAP_TILE_SIZE < MAP_TEST_WIDTH,
                          "the columns reach the right-hand edge");
        MESH_TEST_FAIL_IF(span.origin_y + span.rows * MESH_MAP_TILE_SIZE < MAP_TEST_HEIGHT,
                          "and the rows reach the bottom");

        /* Which tile the centre of the panel falls in, by walking the blit. */
        const int32_t column = (MAP_TEST_WIDTH / 2 - span.origin_x) / MESH_MAP_TILE_SIZE;
        const int32_t row = (MAP_TEST_HEIGHT / 2 - span.origin_y) / MESH_MAP_TILE_SIZE;
        struct mesh_map_tile_key key;
        MESH_TEST_FAIL_IF(!mesh_map_tile_span_key(&span, column, row, &key),
                          "the middle of the panel is on a tile of the span");

        /* And where the centre's own coordinate says it is, in that tile's pixels. */
        struct mesh_map_placement placement;
        MESH_TEST_FAIL_IF(
            !mesh_map_viewport_place(&viewport, MAP_TEST_LATITUDE, MAP_TEST_LONGITUDE, &placement),
            "the centre places");
        const int32_t tile_x = span.origin_x + column * MESH_MAP_TILE_SIZE;
        const int32_t tile_y = span.origin_y + row * MESH_MAP_TILE_SIZE;
        MESH_TEST_FAIL_IF(placement.x < tile_x || placement.x >= tile_x + MESH_MAP_TILE_SIZE,
                          "and lands inside the tile the blit put under it");
        MESH_TEST_FAIL_IF(placement.y < tile_y || placement.y >= tile_y + MESH_MAP_TILE_SIZE,
                          "on both axes");
    }
    record_success(test_name);
}

/*
 * A view on the antimeridian asks for tiles on both sides of the world, and the fold is what
 * makes them addressable.
 *
 * The same seam mesh_map_viewport_offset() takes the short way round: a box centred at 180
 * degrees covers the last columns of the world and the first ones, and a span that did not fold
 * would be asking a pack for tile 262144 at zoom 18 - which is not a tile, so the eastern half
 * of the panel would simply be blank.
 */
MESH_TEST_CASE(map_tile_span_wraps_across_the_antimeridian, unit) {
    /* Either side of the seam, because the two run off opposite edges of the world and only one
       of them needs the flooring divide: a box a fraction of a pixel west of longitude 180 west
       starts at tile -1, and a truncation says tile 0 - which drops the panel's leftmost
       column exactly where nobody looks. */
    const int32_t centres[] = {1799999000, -1799999000};
    for (size_t i = 0U; i < sizeof centres / sizeof centres[0]; ++i) {
        struct mesh_map_viewport viewport;
        mesh_map_viewport_init(&viewport, MAP_TEST_LATITUDE, centres[i], 8U);
        mesh_map_viewport_resize(&viewport, MAP_TEST_WIDTH, MAP_TEST_HEIGHT);

        struct mesh_map_tile_span span;
        MESH_TEST_FAIL_IF(!mesh_map_viewport_tiles(&viewport, &span), "the seam covers tiles");

        const int32_t world = 1 << 8;
        MESH_TEST_FAIL_IF(span.x0 >= 0 && span.x0 + span.columns <= world,
                          "and the span runs off one edge of the world");

        bool saw_last = false;
        bool saw_first = false;
        for (int32_t column = 0; column < span.columns; ++column) {
            struct mesh_map_tile_key key;
            MESH_TEST_FAIL_IF(!mesh_map_tile_span_key(&span, column, 0, &key),
                              "every column of the span names a tile");
            MESH_TEST_FAIL_IF((int32_t)key.x >= world, "and every one of them is on the world");
            saw_last = saw_last || (int32_t)key.x == world - 1;
            saw_first = saw_first || key.x == 0U;
        }
        MESH_TEST_FAIL_IF(!saw_last || !saw_first, "with the world's two edges both on the panel");
    }
    record_success(test_name);
}

/*
 * Rows stop at the top of the world where columns wrap around it.
 *
 * A cylinder and a sheet: panning east forever arrives back where it started and panning north
 * does not. A span that wrapped rows would ask a pack for a tile below the bottom of the world
 * while drawing the top of it, which is a picture of the wrong hemisphere in the sky.
 */
MESH_TEST_CASE(map_tile_span_clamps_rows_at_the_top_of_the_world, unit) {
    struct mesh_map_viewport viewport;
    /* The display limit, which is as far north as a map can look. */
    mesh_map_viewport_init(&viewport, 850511288, 0, 1U);
    mesh_map_viewport_resize(&viewport, MAP_TEST_WIDTH, MAP_TEST_HEIGHT);

    struct mesh_map_tile_span span;
    MESH_TEST_FAIL_IF(!mesh_map_viewport_tiles(&viewport, &span),
                      "the top of the world covers tiles");
    MESH_TEST_FAIL_IF(span.y0 != 0, "the first row is the world's own first row");
    MESH_TEST_FAIL_IF(span.rows > 2, "and there are no rows above it to ask for");
    /* Half the panel is above the world, so the first row starts partway down it - the one case
       where the origin is positive rather than a fraction of a tile behind the corner. */
    MESH_TEST_FAIL_IF(span.origin_y <= 0, "the world's edge is below the top of the box");

    struct mesh_map_tile_key key;
    MESH_TEST_FAIL_IF(!mesh_map_tile_span_key(&span, 0, span.rows - 1, &key),
                      "the last row names a tile");
    MESH_TEST_FAIL_IF(key.y >= 2U, "inside the two rows zoom 1 has");
    record_success(test_name);
}

/* A viewport nothing has measured yet answers nothing, which is the ordinary first frame: the
   nav opens the map before a backend has laid a body out. */
MESH_TEST_CASE(map_tile_span_needs_a_box, unit) {
    struct mesh_map_viewport viewport;
    mesh_map_viewport_init(&viewport, MAP_TEST_LATITUDE, MAP_TEST_LONGITUDE, 12U);

    struct mesh_map_tile_span span;
    MESH_TEST_FAIL_IF(mesh_map_viewport_tiles(&viewport, &span), "no box, no tiles");
    MESH_TEST_FAIL_IF(span.columns != 0 || span.rows != 0, "and the span is zeroed");

    struct mesh_map_tile_key key;
    MESH_TEST_FAIL_IF(mesh_map_tile_span_key(&span, 0, 0, &key), "which names no tile");
    record_success(test_name);
}

/*
 * At zoom 0 the world is one tile and the panel is four of them wide, so the same tile is asked
 * for several times over.
 *
 * Not a bug and not something to clamp away: a world narrower than the panel repeats, which is
 * what every map does at its far zoom-out, and clamping the columns would leave the sides of the
 * panel empty instead. It costs one decode and several blits, because the keys are equal.
 */
MESH_TEST_CASE(map_tile_span_repeats_a_world_narrower_than_the_panel, unit) {
    struct mesh_map_viewport viewport;
    mesh_map_viewport_init(&viewport, 0, 0, 0U);
    mesh_map_viewport_resize(&viewport, MAP_TEST_WIDTH, MAP_TEST_HEIGHT);

    struct mesh_map_tile_span span;
    MESH_TEST_FAIL_IF(!mesh_map_viewport_tiles(&viewport, &span), "zoom 0 covers tiles");
    MESH_TEST_FAIL_IF(span.columns < 4, "the panel is several worlds wide");
    MESH_TEST_FAIL_IF(span.rows != 1, "and the world has exactly one row");
    for (int32_t column = 0; column < span.columns; ++column) {
        struct mesh_map_tile_key key;
        MESH_TEST_FAIL_IF(!mesh_map_tile_span_key(&span, column, 0, &key),
                          "every column names a tile");
        MESH_TEST_FAIL_IF(key.x != 0U || key.y != 0U, "and all of them are the only tile there is");
    }
    record_success(test_name);
}
