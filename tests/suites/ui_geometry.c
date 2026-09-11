#define _POSIX_C_SOURCE 200809L

/*
 * The frame at panel sizes that are not the Brick's.
 *
 * Every other rendering test in this tree draws at 1024x768, which is the one panel this pak
 * ships for - so the properties the layout has because it *measures* rather than because it was
 * tuned have never been separated from the properties it has because 1024x768 is roomy. The two
 * come apart on a different screen, and they come apart silently: a tab that no longer fits is
 * not a crash, a card refused for want of room is not a warning, and an action bar pushed off
 * the bottom still renders a perfectly good frame with nothing at the bottom of it.
 *
 * These are invariants rather than golden images on purpose. Pinning pixels at four geometries
 * would be four times the maintenance for every legitimate UI change, and would fail for the
 * wrong reason every time; what is worth holding is the handful of things that must be true of
 * *any* frame on *any* panel. The renderer already degrades - the tab strip drops its labels by
 * measured room, the action bar drops entries from the end, a field's label column halves on a
 * narrow body - so what these check is that the degradation is reached rather than overrun.
 *
 * The matrix is deliberately not a list of devices. It is the Brick's panel, two shapes wider
 * and shorter than it, and one appreciably smaller - enough to move every ladder in the
 * renderer without claiming this client has been run on any of them.
 */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/ui/backends/fb_capture.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "mesh/ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct geometry {
    uint32_t width;
    uint32_t height;
    const char *what;
};

/*
 * The panels these are drawn at.
 *
 * 1024x768 is the Brick and is here so a failure can be read as "this is about the geometry"
 * rather than "this is about the frame"; the rest are shapes, not products. 1280x800 is wider
 * and taller, 1280x720 wider and shorter - which is the combination that catches something
 * reserved out of the height and spent on the width - and 640x480 is small enough to put every
 * ladder in the renderer at its bottom step at once.
 */
static const struct geometry k_geometries[] = {
    {1024U, 768U, "the Brick's panel"},
    {1280U, 800U, "wider and taller"},
    {1280U, 720U, "wider and shorter"},
    {640U, 480U, "small"},
};

/*
 * The grounds: every colour a frame can be filled *with*, as opposed to written in.
 *
 * Ink is defined as what is left, and that works because there is no anti-aliasing here - a
 * fill is exactly its palette entry, and the only pixels that are not are glyphs and icons,
 * which are resampled coverage and so land between their ink and whatever they were drawn on.
 * That is what makes "is there text here?" answerable from the page without knowing anything
 * about the layout.
 */
static const enum mesh_ui_color k_grounds[] = {
    MESH_UI_COLOR_BG,
    MESH_UI_COLOR_SURFACE_LOW,
    MESH_UI_COLOR_SURFACE,
    MESH_UI_COLOR_SURFACE_HIGH,
    MESH_UI_COLOR_SURFACE_SEL,
    MESH_UI_COLOR_SURFACE_ACTIVE,
    MESH_UI_COLOR_SURFACE_INVERSE,
};

static bool pixel_is_ground(const struct mesh_ui_theme *theme, const uint8_t *pixel) {
    for (size_t i = 0; i < sizeof k_grounds / sizeof k_grounds[0]; ++i) {
        const struct mesh_ui_rgb ground = mesh_ui_theme_color(theme, k_grounds[i]);
        /* 32 bpp, B,G,R,X in memory - what the capture fabricates and what fb0 holds. */
        if (pixel[0] == ground.b && pixel[1] == ground.g && pixel[2] == ground.r) {
            return true;
        }
    }
    return false;
}

/* Whether anything is written - as opposed to filled - inside this band of rows. */
static bool band_has_ink(const struct mesh_ui_theme *theme, const uint8_t *pixels, uint32_t width,
                         size_t stride, uint32_t from_row, uint32_t to_row) {
    for (uint32_t y = from_row; y < to_row; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0U; x < width; ++x) {
            if (!pixel_is_ground(theme, row + (size_t)x * 4U)) {
                return true;
            }
        }
    }
    return false;
}

/* The same question of a column, which is what an overhanging label lands in. */
static bool column_has_ink(const struct mesh_ui_theme *theme, const uint8_t *pixels,
                           uint32_t height, size_t stride, uint32_t column) {
    for (uint32_t y = 0U; y < height; ++y) {
        if (!pixel_is_ground(theme, pixels + (size_t)y * stride + (size_t)column * 4U)) {
            return true;
        }
    }
    return false;
}

/* One populated snapshot, parked on `screen`. Returns false when the store had nothing to say. */
static bool snapshot_for_screen(struct mesh_ui_store *store, enum mesh_ui_screen screen,
                                struct mesh_ui_snapshot *out) {
    store->nav.screen = screen;
    memset(out, 0, sizeof *out);
    mesh_ui_store_request_refresh(store);
    return mesh_ui_store_consume_updates(store, out);
}

/*
 * Every tab, on every panel: the chrome is drawn and nothing is written into the outermost
 * column on either side.
 *
 * The two halves are the two ways a panel this client has not seen goes wrong. A frame that
 * lost its chrome is one where the body took room the bars needed - the reader can no longer
 * tell which tab they are on or what the buttons do, and the frame is otherwise perfect. A
 * frame with ink in its first or last column is the opposite: something was laid out against a
 * width it did not fit in, and what the reader sees is a word with its first letter shaved off.
 * Neither is visible in a screenshot unless somebody thinks to take one at that size.
 *
 * A column rather than a margin because the margin is a theme's to choose and this is not: the
 * renderer insets everything it writes, so ink in the edge column means the inset was computed
 * and then overrun.
 */
MESH_TEST_CASE(ui_geometry_every_screen_keeps_its_chrome, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    const char *failure = NULL;
    char detail[192];

    for (size_t g = 0; g < sizeof k_geometries / sizeof k_geometries[0] && failure == NULL; ++g) {
        const struct geometry *geometry = &k_geometries[g];

        for (unsigned screen = 0U; screen < (unsigned)MESH_UI_SCREEN_COUNT && failure == NULL;
             ++screen) {
            struct mesh_ui_snapshot snapshot;
            if (!snapshot_for_screen(&store, (enum mesh_ui_screen)screen, &snapshot)) {
                failure = "no snapshot to render";
                break;
            }

            struct mesh_ui_capture *capture = NULL;
            if (mesh_ui_capture_open(&capture, geometry->width, geometry->height, 0) != 0) {
                failure = "capture open failed";
                break;
            }
            mesh_ui_capture_render(capture, &snapshot);

            uint32_t width = 0U;
            uint32_t height = 0U;
            size_t stride = 0U;
            const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
            const struct mesh_ui_theme *theme = mesh_ui_capture_theme(capture);

            /*
             * An eighth of the panel at each end, which is more than either bar takes at any
             * scale and less than the body ever starts at - so the three bands partition the
             * frame into the navigation bar, the body and the action bar.
             *
             * The middle one is measured from the bar rather than from the middle of the panel,
             * which was the first thing tried and is wrong: a screen with a short list - the
             * Waypoints tab with one row on it - legitimately has nothing below the first
             * quarter, and a band starting there reads a correct frame as an empty body.
             */
            const uint32_t band = height / 8U;
            if (!band_has_ink(theme, pixels, width, stride, 0U, band)) {
                snprintf(detail, sizeof detail, "%s (%ux%u): screen %u drew no navigation bar",
                         geometry->what, width, height, screen);
                failure = detail;
            } else if (!band_has_ink(theme, pixels, width, stride, height - band, height)) {
                snprintf(detail, sizeof detail, "%s (%ux%u): screen %u drew no action bar",
                         geometry->what, width, height, screen);
                failure = detail;
            } else if (!band_has_ink(theme, pixels, width, stride, band, height - band)) {
                snprintf(detail, sizeof detail, "%s (%ux%u): screen %u drew an empty body",
                         geometry->what, width, height, screen);
                failure = detail;
            } else if (column_has_ink(theme, pixels, height, stride, 0U)) {
                snprintf(detail, sizeof detail, "%s (%ux%u): screen %u wrote into column 0",
                         geometry->what, width, height, screen);
                failure = detail;
            } else if (column_has_ink(theme, pixels, height, stride, width - 1U)) {
                snprintf(detail, sizeof detail, "%s (%ux%u): screen %u wrote into the last column",
                         geometry->what, width, height, screen);
                failure = detail;
            }

            mesh_ui_capture_close(capture);
        }
    }

    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The overlays, on every panel.
 *
 * They are here as a case of their own because they are the densest thing this client draws and
 * the only thing that lays out from the *trailing* edge backwards. That arithmetic is happy to
 * produce a negative x, and it did: at the largest glyph scale a confirmation's two answers put
 * the cancel button off the left-hand edge of the screen - on a destructive dialog, where the
 * answer that vanished was the safe one. A narrow panel is the same overflow reached from the
 * other direction, and the fix for the first (stack when the pair will not fit, fit each label
 * to the panel) is what has to keep holding here.
 *
 * The keyboard goes with it for the opposite reason: it is a fixed grid of keys rather than a
 * row that elides, so it is the layout here with the least room to give.
 */
MESH_TEST_CASE(ui_geometry_overlays_stay_inside_the_panel, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    static const char *const k_overlays[] = {"the confirmation", "the keyboard"};
    const char *failure = NULL;
    char detail[192];

    for (size_t g = 0; g < sizeof k_geometries / sizeof k_geometries[0] && failure == NULL; ++g) {
        const struct geometry *geometry = &k_geometries[g];

        for (size_t overlay = 0U;
             overlay < sizeof k_overlays / sizeof k_overlays[0] && failure == NULL; ++overlay) {
            store.nav.screen = overlay == 0U ? MESH_UI_SCREEN_SETTINGS : MESH_UI_SCREEN_MESSAGES;
            store.nav.confirm_open = overlay == 0U;
            store.nav.confirm_cursor = 0U;
            store.nav.confirm_action = (uint8_t)MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE;
            store.nav.compose_open = overlay == 1U;
            store.nav.keyboard_open = overlay == 1U;

            struct mesh_ui_snapshot snapshot;
            memset(&snapshot, 0, sizeof snapshot);
            mesh_ui_store_request_refresh(&store);
            if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
                failure = "no snapshot to render";
                break;
            }

            struct mesh_ui_capture *capture = NULL;
            if (mesh_ui_capture_open(&capture, geometry->width, geometry->height, 0) != 0) {
                failure = "capture open failed";
                break;
            }

            uint32_t width = 0U;
            uint32_t height = 0U;
            size_t stride = 0U;
            const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
            const struct mesh_ui_theme *theme = mesh_ui_capture_theme(capture);
            const size_t page_bytes = stride * (size_t)height;

            mesh_ui_capture_render(capture, &snapshot);

            /*
             * The screen underneath, so the overlay can be shown to be on the frame.
             *
             * Without it the case is vacuous in the one way that matters: a nav flag that
             * stopped opening the overlay would leave every assertion below passing over a
             * plain screen, and the thing they exist to check would simply not be drawn.
             *
             * It is a capture of its own rather than a second render into this one, and that is
             * not fastidiousness. A backend remembers the route it last drew, so a second render
             * of a different place starts the screen transition - and mid-slide the body really
             * is over the panel's edge, which is the whole point of a slide. What is asserted
             * here is the settled frame, so each is somebody's first.
             */
            struct mesh_ui_snapshot plain = snapshot;
            plain.nav.confirm_open = false;
            plain.nav.compose_open = false;
            plain.nav.keyboard_open = false;

            struct mesh_ui_capture *beneath = NULL;
            if (mesh_ui_capture_open(&beneath, geometry->width, geometry->height, 0) != 0) {
                failure = "capture open failed";
                mesh_ui_capture_close(capture);
                break;
            }
            mesh_ui_capture_render(beneath, &plain);
            const bool overlay_drew =
                memcmp(mesh_ui_capture_pixels(beneath, NULL, NULL, NULL), pixels, page_bytes) != 0;
            mesh_ui_capture_close(beneath);

            if (!overlay_drew) {
                snprintf(detail, sizeof detail, "%s (%ux%u): %s never reached the frame",
                         geometry->what, width, height, k_overlays[overlay]);
                failure = detail;
            } else if (column_has_ink(theme, pixels, height, stride, 0U)) {
                snprintf(detail, sizeof detail, "%s (%ux%u): %s wrote into column 0",
                         geometry->what, width, height, k_overlays[overlay]);
                failure = detail;
            } else if (column_has_ink(theme, pixels, height, stride, width - 1U)) {
                snprintf(detail, sizeof detail, "%s (%ux%u): %s wrote into the last column",
                         geometry->what, width, height, k_overlays[overlay]);
                failure = detail;
            }

            mesh_ui_capture_close(capture);
        }
    }

    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
