#define _POSIX_C_SOURCE 200809L

/*
 * The off-screen framebuffer renderer.
 *
 * There is no /dev/fb0 in CI or in the dev container, which is exactly why this exists: it is
 * the only way the fb backend's output is exercised anywhere but on a Brick. The cases below
 * check the contract the encoders downstream rely on - geometry, pixel order, and that a
 * snapshot actually reaches the page - rather than pinning pixels, which would fail on every
 * legitimate change to the UI.
 */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/ui/backends/fb_capture.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store.h"
#include "mesh/ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The ground `capture` draws on, which fb_render_snapshot() clears to before anything else.
 *
 * Taken from the capture rather than spelled out, and rather than assumed to be the default
 * theme's: mesh_ui_capture_open() honours MESHCLIENT_THEME exactly as the device backend does,
 * so a developer running `MESHCLIENT_THEME=light make test` would otherwise see this file count
 * an entire correct frame as drawn pixels and fail. A palette change is not a test change
 * either; the themes themselves are covered in ui_theme.c.
 */
static bool pixel_is_background(const struct mesh_ui_capture *capture, const uint8_t *pixel) {
    const struct mesh_ui_rgb bg =
        mesh_ui_theme_color(mesh_ui_capture_theme(capture), MESH_UI_COLOR_BG);
    /* 32 bpp with every bitfield zero, which is what the capture fabricates: B,G,R,X. */
    return pixel[0] == bg.b && pixel[1] == bg.g && pixel[2] == bg.r;
}

/* Whether the page still holds what calloc() left, i.e. nothing has been drawn into it. */
static bool page_is_zeroed(const uint8_t *pixels, uint32_t width, uint32_t height, size_t stride) {
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        for (size_t i = 0; i < (size_t)width * 4U; ++i) {
            if (row[i] != 0U) {
                return false;
            }
        }
    }
    return true;
}

static size_t count_drawn(const struct mesh_ui_capture *capture, const uint8_t *pixels,
                          uint32_t width, uint32_t height, size_t stride) {
    size_t drawn = 0U;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0U; x < width; ++x) {
            if (!pixel_is_background(capture, row + (size_t)x * 4U)) {
                drawn++;
            }
        }
    }
    return drawn;
}

MESH_TEST_CASE(ui_capture_renders_a_snapshot, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_store_shutdown(&store), "no snapshot to render");

    struct mesh_ui_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, 4) != 0,
        mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
    MESH_TEST_FAIL_IF_CLEANUP(pixels == NULL || width != MESH_UI_CAPTURE_WIDTH ||
                                  height != MESH_UI_CAPTURE_HEIGHT ||
                                  stride != (size_t)MESH_UI_CAPTURE_WIDTH * 4U,
                              mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "capture geometry is wrong");

    /* A fresh page is zeroed. Said in bytes rather than as "no pixel is the background
       colour", because a theme whose ground is pure black makes those two opposites - the
       assertion is that nothing has been drawn yet, not that the ground is a particular hue. */
    MESH_TEST_FAIL_IF_CLEANUP(!page_is_zeroed(pixels, width, height, stride),
                              mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "an unrendered page is not blank");

    mesh_ui_capture_render(capture, &snapshot);
    const size_t drawn = count_drawn(capture, pixels, width, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(
        drawn == 0U || drawn > (size_t)width * (size_t)height / 2U, mesh_ui_capture_close(capture);
        mesh_ui_store_shutdown(&store), "the rendered frame is blank, or is not mostly background");

    mesh_ui_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* Whether a pixel is exactly the colour a role names. The capture fabricates 32 bpp with every
   bitfield zero, which is B,G,R,X - the same order pixel_is_background() reads. */
static bool pixel_is_role(const struct mesh_ui_capture *capture, const uint8_t *pixel,
                          enum mesh_ui_color role) {
    const struct mesh_ui_rgb want = mesh_ui_theme_color(mesh_ui_capture_theme(capture), role);
    return pixel[0] == want.b && pixel[1] == want.g && pixel[2] == want.r;
}

/* The widest run of `role` on any scanline, as a fraction of the width, in percent. A card's
   padding band is an unbroken run of its fill from edge to edge and its border is an unbroken
   run of the rule colour, so both come out near 100; a glyph in that colour comes out at a few. */
static unsigned widest_row_run(const struct mesh_ui_capture *capture, const uint8_t *pixels,
                               uint32_t width, uint32_t height, size_t stride,
                               enum mesh_ui_color role) {
    unsigned best = 0U;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        unsigned run = 0U;
        for (uint32_t x = 0U; x < width; ++x) {
            run = pixel_is_role(capture, row + (size_t)x * 4U, role) ? run + 1U : 0U;
            const unsigned pct = (unsigned)((uint64_t)run * 100U / width);
            if (pct > best) {
                best = pct;
            }
        }
    }
    return best;
}

/*
 * The Status screen is drawn as cards, not as text on the bare ground.
 *
 * Pinning pixels would fail on every legitimate change to that screen, so this asks the two
 * things a card is: a filled panel spanning most of the width, and a hairline of the rule colour
 * doing the same. Neither appears on a screen of plain rows - which is what Status was - so the
 * case fails if the cards are lost, and passes whatever the rows inside them come to say.
 *
 * Both are checked because either alone can be there without a card: a selected list row is a
 * fill, and the strip under the tabs is a rule. Only a card is both, one inside the other.
 */
MESH_TEST_CASE(ui_capture_draws_the_status_cards, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    /* Walked rather than assigned, and asserted on arrival: the tab order is the nav's to
       change, and a test that set nav.screen by hand would keep passing while the key that gets
       a user there stopped working. */
    struct mesh_ui_action action;
    while (store.nav.screen != MESH_UI_SCREEN_STATUS) {
        const enum mesh_ui_screen before = store.nav.screen;
        memset(&action, 0, sizeof action);
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
        MESH_TEST_FAIL_IF_CLEANUP(store.nav.screen == before, mesh_ui_store_shutdown(&store),
                                  "Right stopped moving before the Status tab");
    }

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_store_shutdown(&store), "no snapshot to render");

    struct mesh_ui_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, 4) != 0,
        mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
    mesh_ui_capture_render(capture, &snapshot);

    const unsigned fill =
        widest_row_run(capture, pixels, width, height, stride, MESH_UI_COLOR_SURFACE);
    MESH_TEST_FAIL_IF_CLEANUP(fill < 80U, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the Status screen draws no card fill across the body");

    /* OUTLINE, not RULE: a card's edge is a container's boundary rather than a separator, and
       the two parted company when a second container wanted an edge of its own. */
    const unsigned edge =
        widest_row_run(capture, pixels, width, height, stride, MESH_UI_COLOR_OUTLINE);
    MESH_TEST_FAIL_IF_CLEANUP(edge < 80U, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the Status screen draws no card edge across the body");

    mesh_ui_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * Two screens must not render identically. This is the check that would have caught a capture
 * tool that rendered the same snapshot over and over - a clip of one frame repeated looks
 * plausible right up until you notice nothing moves.
 */
/*
 * The conversation list is drawn as two-line items with slots, not as rows of text.
 *
 * Same approach as the cards above - the structure, never the pixels - and the two things
 * asked for are the two halves of what a slot *is*, so the case fails if either the leading or
 * the trailing one is lost while the words survive:
 *
 *   - a filled shape in the accent, several cells across. A leading disc and a trailing unread
 *     capsule are both that; a glyph drawn in the accent is at most a stroke wide, and so is
 *     the bar down a selected row's edge. Only a slot puts an unbroken band of accent on a
 *     scanline.
 *   - a hairline of the rule colour spanning most of the width: the divider between one item
 *     and the next, which nothing else on this screen draws.
 *
 * Both were the conversation cell's own geometry once and are the shared item's now, which is
 * exactly why they are worth pinning: the next component to want a slot will be built on the
 * same code, and a regression here would otherwise only show up on a device.
 */
MESH_TEST_CASE(ui_capture_draws_the_conversation_items, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    /* Messages is where the nav opens, and the list is its first level - asserted rather than
       assumed, so this fails loudly rather than silently checking some other screen. */
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.screen != MESH_UI_SCREEN_MESSAGES || store.nav.thread_open,
                              mesh_ui_store_shutdown(&store),
                              "the nav does not open on the conversation list");

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_store_shutdown(&store), "no snapshot to render");

    struct mesh_ui_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, 4) != 0,
        mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
    mesh_ui_capture_render(capture, &snapshot);

    const unsigned shape =
        widest_row_run(capture, pixels, width, height, stride, MESH_UI_COLOR_ACCENT);
    MESH_TEST_FAIL_IF_CLEANUP(shape < 2U, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the conversation list draws no filled accent slot");

    const unsigned divider =
        widest_row_run(capture, pixels, width, height, stride, MESH_UI_COLOR_RULE);
    MESH_TEST_FAIL_IF_CLEANUP(divider < 50U, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the conversation list draws no divider between its items");

    mesh_ui_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

MESH_TEST_CASE(ui_capture_follows_the_nav, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, 320U, 240U, 2) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
    const size_t page_bytes = stride * (size_t)height;
    uint8_t *first = malloc(page_bytes);
    MESH_TEST_FAIL_IF_CLEANUP(first == NULL, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "out of memory");

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    mesh_ui_capture_render(capture, &snapshot);
    memcpy(first, pixels, page_bytes);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    MESH_TEST_FAIL_IF_CLEANUP(
        store.nav.screen != MESH_UI_SCREEN_NODES, free(first); mesh_ui_capture_close(capture);
        mesh_ui_store_shutdown(&store), "Right did not move to the Nodes tab");
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    mesh_ui_capture_render(capture, &snapshot);

    MESH_TEST_FAIL_IF_CLEANUP(
        memcmp(first, pixels, page_bytes) == 0, free(first); mesh_ui_capture_close(capture);
        mesh_ui_store_shutdown(&store), "two different screens rendered identically");

    free(first);
    mesh_ui_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* The PPM is what scripts/frames.py parses, so its header and its byte order are a contract. */
MESH_TEST_CASE(ui_capture_writes_a_ppm, unit) {
    struct mesh_ui_capture *capture = NULL;
    MESH_TEST_FAIL_IF(mesh_ui_capture_open(&capture, 64U, 32U, 2) != 0, "capture open failed");

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    snprintf(snapshot.transport_status, sizeof snapshot.transport_status, "%s", "running");
    mesh_ui_capture_render(capture, &snapshot);

    char path[] = "/tmp/meshclient-capture-XXXXXX";
    const int fd = mkstemp(path);
    MESH_TEST_FAIL_IF_CLEANUP(fd < 0, mesh_ui_capture_close(capture), "cannot make a temp file");
    close(fd);

    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_write_ppm(capture, path) != 0, unlink(path);
                              mesh_ui_capture_close(capture), "writing the PPM failed");

    FILE *file = fopen(path, "rb");
    MESH_TEST_FAIL_IF_CLEANUP(file == NULL, unlink(path);
                              mesh_ui_capture_close(capture), "cannot reopen the PPM");

    char header[16];
    memset(header, 0, sizeof header);
    const size_t header_read = fread(header, 1U, 15U, file);
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fclose(file);
    unlink(path);

    MESH_TEST_FAIL_IF_CLEANUP(header_read < 15U || strncmp(header, "P6\n64 32\n255\n", 13) != 0,
                              mesh_ui_capture_close(capture), "unexpected PPM header");
    /* "P6\n64 32\n255\n" is 13 bytes, then three bytes a pixel with no padding. */
    MESH_TEST_FAIL_IF_CLEANUP(size != 13L + 64L * 32L * 3L, mesh_ui_capture_close(capture),
                              "the PPM is not header plus one RGB triple per pixel");

    mesh_ui_capture_close(capture);
    record_success(test_name);
}
