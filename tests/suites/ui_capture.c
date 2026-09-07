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
#include "mesh/ui/settings.h"
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
        widest_row_run(capture, pixels, width, height, stride, MESH_UI_COLOR_PRIMARY);
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

/*
 * The topmost scanline carrying an unbroken run of `role` at least `min_run` pixels wide, or
 * `height` when there is none. What says *where* a container is rather than whether it exists,
 * which is the only way to ask whether something slid.
 */
static uint32_t topmost_row_run(const struct mesh_ui_capture *capture, const uint8_t *pixels,
                                uint32_t width, uint32_t height, size_t stride,
                                enum mesh_ui_color role, unsigned min_run) {
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        unsigned run = 0U;
        for (uint32_t x = 0U; x < width; ++x) {
            run = pixel_is_role(capture, row + (size_t)x * 4U, role) ? run + 1U : 0U;
            if (run >= min_run) {
                return y;
            }
        }
    }
    return height;
}

/* Draws frames until nothing is moving, exactly as the event loop's repaint timer does. The
   guard is against a widget that never settles - a bug, but not one that should hang a test. */
static void render_until_still(struct mesh_ui_capture *capture,
                               const struct mesh_ui_snapshot *snapshot) {
    mesh_ui_capture_render(capture, snapshot);
    for (unsigned i = 0U; i < 60U && mesh_ui_capture_animating(capture); ++i) {
        mesh_ui_capture_advance(capture, 33U);
        mesh_ui_capture_render(capture, snapshot);
    }
}

/*
 * The transient notice is a container that slides, not a line of accent text in the footer.
 *
 * Four things, and the middle two are the ones a still could not show:
 *
 *   - nothing of the inverted surface is on a frame with no notice up. That role has exactly
 *     one user, so its absence and its presence are the whole test for "is there a snackbar".
 *   - it is below its resting place on the frame it is raised on, and higher once the
 *     animation has run out. That is the arrival, and it is the reason the notice moved out of
 *     the footer at all.
 *   - it comes to rest over the body rather than in the chrome: below the middle of the panel,
 *     and spanning enough pixels to be a container rather than a glyph.
 *   - it is gone again once the nav has dropped it *and* the slide back out has finished -
 *     which is the half that needs the backend to keep the words after the store has forgotten
 *     them, and would fail if it did not.
 */
MESH_TEST_CASE(ui_capture_slides_the_snackbar_in_and_out, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, 4) != 0,
        mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);

    /* Wide enough that only a fill can produce it: a glyph at this scale is a few pixels of
       stroke and the panel is 1024 across. */
    const unsigned container_run = width / 8U;

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no snapshot to render");
    render_until_still(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(topmost_row_run(capture, pixels, width, height, stride,
                                              MESH_UI_COLOR_SURFACE_INVERSE,
                                              container_run) < height,
                              mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "a frame with no notice up still draws the snackbar's surface");

    mesh_ui_store_set_toast(&store, 1000U, "Sent to BRVO");
    MESH_TEST_FAIL_IF_CLEANUP(
        !mesh_ui_store_consume_updates(&store, &snapshot), mesh_ui_capture_close(capture);
        mesh_ui_store_shutdown(&store), "raising a notice published no snapshot");

    mesh_ui_capture_render(capture, &snapshot);
    const uint32_t arriving = topmost_row_run(capture, pixels, width, height, stride,
                                              MESH_UI_COLOR_SURFACE_INVERSE, container_run);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_capture_animating(capture), mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the notice appeared in place instead of sliding in");

    render_until_still(capture, &snapshot);
    const uint32_t resting = topmost_row_run(capture, pixels, width, height, stride,
                                             MESH_UI_COLOR_SURFACE_INVERSE, container_run);
    MESH_TEST_FAIL_IF_CLEANUP(resting >= height, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the notice never drew a container of the inverted surface");
    MESH_TEST_FAIL_IF_CLEANUP(resting >= arriving, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the notice did not travel upwards into its resting place");
    MESH_TEST_FAIL_IF_CLEANUP(
        resting < height / 2U, mesh_ui_capture_close(capture);
        mesh_ui_store_shutdown(&store),
        "the notice came to rest somewhere other than the bottom of the body");

    /* Past the four seconds the nav gives it. The store forgets the words here; the backend has
       to keep them long enough to draw the way out. */
    mesh_ui_store_tick(&store, 9000U);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.toast[0] != '\0', mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "the notice did not expire");
    MESH_TEST_FAIL_IF_CLEANUP(
        !mesh_ui_store_consume_updates(&store, &snapshot), mesh_ui_capture_close(capture);
        mesh_ui_store_shutdown(&store), "an expired notice published no snapshot");

    mesh_ui_capture_render(capture, &snapshot);
    const uint32_t leaving = topmost_row_run(capture, pixels, width, height, stride,
                                             MESH_UI_COLOR_SURFACE_INVERSE, container_run);
    MESH_TEST_FAIL_IF_CLEANUP(leaving >= height, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the notice vanished on expiry instead of sliding out");

    render_until_still(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(
        topmost_row_run(capture, pixels, width, height, stride, MESH_UI_COLOR_SURFACE_INVERSE,
                        container_run) < height,
        mesh_ui_capture_close(capture);
        mesh_ui_store_shutdown(&store), "the notice is still on the panel after sliding out");

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

/* Long runs of one colour on a single scanline, ignoring the two grounds a dialog is built
   from - the body behind it and the panel itself. A filled control is such a run; a glyph, a
   hairline outline and an antialiased icon edge are all far shorter. */
static uint32_t pixel_key(const uint8_t *pixel) {
    /* 32 bpp with every bitfield zero, which is what the capture fabricates: B,G,R,X. */
    return (uint32_t)pixel[0] | ((uint32_t)pixel[1] << 8) | ((uint32_t)pixel[2] << 16);
}

static uint32_t rgb_key(struct mesh_ui_rgb rgb) {
    return (uint32_t)rgb.b | ((uint32_t)rgb.g << 8) | ((uint32_t)rgb.r << 16);
}

/* Rows the dialog's raised panel covers, found by its fill rather than by re-deriving the
   layout here - a test that computed the panel's geometry itself would agree with a broken
   renderer. */
static void panel_rows(const uint8_t *pixels, uint32_t width, uint32_t height, size_t stride,
                       const struct mesh_ui_theme *theme, uint32_t *top, uint32_t *bottom) {
    const uint32_t panel = rgb_key(mesh_ui_theme_color(theme, MESH_UI_COLOR_SURFACE_HIGH));
    *top = height;
    *bottom = 0U;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        uint32_t run = 0U;
        for (uint32_t x = 0; x < width; ++x) {
            run = (pixel_key(row + (size_t)x * 4U) == panel) ? run + 1U : 0U;
            if (run < 40U) {
                continue;
            }
            if (y < *top) {
                *top = y;
            }
            *bottom = y;
            break;
        }
    }
}

/* Pixels that differ between two frames inside a rectangle. */
static size_t differing_in(const uint8_t *a, const uint8_t *b, size_t stride, uint32_t x0,
                           uint32_t x1, uint32_t y0, uint32_t y1) {
    size_t differing = 0U;
    for (uint32_t y = y0; y <= y1; ++y) {
        for (uint32_t x = x0; x < x1; ++x) {
            const size_t i = (size_t)y * stride + (size_t)x * 4U;
            if (pixel_key(a + i) != pixel_key(b + i)) {
                ++differing;
            }
        }
    }
    return differing;
}

/*
 * Moving the cursor visibly changes the accept button, on every theme.
 *
 * This is the one thing a dialog has to get right and the one thing a palette can quietly take
 * away. The obvious design gives the accept a standing tonal fill so it reads as the proposed
 * answer and lets the cursor promote it to the full accent, and it looks correct on three of
 * the four themes. On the high-contrast one it is unusable: that palette deliberately collapses
 * ACCENT, ACCENT_CONTAINER and SURFACE_ACTIVE onto a single yellow, because a theme built for
 * legibility has no "held back" version of its one accent. The accept then renders *identically*
 * whether or not it is selected, and which answer A would press cannot be told at all - on the
 * theme chosen by the people least able to guess.
 *
 * Two weaker assertions were tried first and both passed on the broken design, which is why the
 * shape of this one matters. "The two frames differ" passes because the *cancel* button still
 * changes. "Only one control is filled" cannot be counted from scanlines at all: a button's own
 * label splits its fill into four separate runs of the same colour, so a single filled button
 * already looks like four.
 *
 * So the assertion is narrowed to the accept button's own corner of the panel, across the two
 * cursor positions. That region has to change, whatever the palette collapses.
 */
MESH_TEST_CASE(ui_capture_dialog_marks_the_selected_answer, unit) {
    const char *failure = NULL;
    struct mesh_ui_capture *capture = NULL;
    uint8_t *frames[2] = {NULL, NULL};

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    for (size_t t = 0; t < mesh_ui_theme_count() && failure == NULL; ++t) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(t);
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;

        for (unsigned cursor = 0U; cursor < 2U && failure == NULL; ++cursor) {
            store.nav.confirm_open = true;
            store.nav.confirm_cursor = (uint8_t)cursor;
            store.nav.screen = MESH_UI_SCREEN_SETTINGS;

            struct mesh_ui_snapshot snapshot;
            memset(&snapshot, 0, sizeof snapshot);
            mesh_ui_store_request_refresh(&store);
            if (!mesh_ui_store_consume_updates(&store, &snapshot) || !snapshot.nav.confirm_open) {
                failure = "no snapshot carrying the confirm overlay";
                break;
            }
            if (mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, 4) !=
                0) {
                failure = "capture open failed";
                break;
            }
            mesh_ui_capture_set_theme(capture, theme);
            const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
            mesh_ui_capture_render(capture, &snapshot);

            frames[cursor] = malloc(stride * (size_t)height);
            if (frames[cursor] == NULL) {
                failure = "out of memory";
            } else {
                memcpy(frames[cursor], pixels, stride * (size_t)height);
            }
            mesh_ui_capture_close(capture);
            capture = NULL;
        }

        if (failure == NULL) {
            uint32_t top = 0U;
            uint32_t bottom = 0U;
            panel_rows(frames[0], width, height, stride, theme, &top, &bottom);
            if (top >= bottom) {
                failure = "the dialog drew no raised panel";
            } else {
                /* The action row is the foot of the panel, and the accept is the button against
                   its trailing edge - so this rectangle is inside that button on every theme
                   without the test having to know how wide its label is. */
                const uint32_t band = bottom - (bottom - top) / 6U;
                const uint32_t x0 = (width * 3U) / 4U;
                const uint32_t x1 = (width * 93U) / 100U;
                const size_t changed =
                    differing_in(frames[0], frames[1], stride, x0, x1, band, bottom);
                if (changed < 200U) {
                    failure = "the accept button looks the same selected and not - which answer "
                              "is chosen cannot be told on this theme";
                }
            }
        }

        free(frames[0]);
        free(frames[1]);
        frames[0] = NULL;
        frames[1] = NULL;
    }

    free(frames[0]);
    free(frames[1]);
    if (capture != NULL) {
        mesh_ui_capture_close(capture);
    }
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A dialog's answers stay inside its panel, whatever the labels say and whatever the scale.
 *
 * The action row is laid out from the panel's trailing edge backwards, and the arithmetic is
 * happy to produce a negative x: at the largest glyph scale "Reset the node database" wants more
 * than the whole panel on its own, and beside Cancel it put the cancel button off the left-hand
 * edge of the *screen*. On a destructive confirmation, where the answer that disappeared is the
 * safe one.
 *
 * So the row stacks when the pair will not fit, and each label is fitted to the panel so a
 * single button cannot overhang it either. What that has to mean, and what is asserted here, is
 * that nothing the dialog draws lands outside the panel it belongs to.
 */
MESH_TEST_CASE(ui_capture_dialog_actions_stay_inside_the_panel, unit) {
    const char *failure = NULL;
    struct mesh_ui_capture *capture = NULL;

    /* Every action whose confirmation this screen can raise, so the longest label in the
       catalog is covered rather than assumed - and the two factory resets with it. */
    static const enum mesh_ui_settings_action actions[] = {
        MESH_UI_SETTINGS_ACTION_NONE,
        MESH_UI_SETTINGS_ACTION_REBOOT,
        MESH_UI_SETTINGS_ACTION_SHUTDOWN,
        MESH_UI_SETTINGS_ACTION_RESET_NODEDB,
        MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES,
        MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES,
        MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG,
        MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE,
    };

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    /* The largest multiplier the UI accepts, which is where the row overflowed. */
    for (size_t a = 0; a < sizeof actions / sizeof actions[0] && failure == NULL; ++a) {
        for (unsigned cursor = 0U; cursor < 2U && failure == NULL; ++cursor) {
            store.nav.confirm_open = true;
            store.nav.confirm_cursor = (uint8_t)cursor;
            store.nav.confirm_action = (uint8_t)actions[a];
            store.nav.screen = MESH_UI_SCREEN_SETTINGS;

            struct mesh_ui_snapshot snapshot;
            memset(&snapshot, 0, sizeof snapshot);
            mesh_ui_store_request_refresh(&store);
            if (!mesh_ui_store_consume_updates(&store, &snapshot) || !snapshot.nav.confirm_open) {
                failure = "no snapshot carrying the confirm overlay";
                break;
            }
            if (mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT,
                                     MESH_UI_SCALE_MAX) != 0) {
                failure = "capture open failed";
                break;
            }

            uint32_t width = 0U;
            uint32_t height = 0U;
            size_t stride = 0U;
            const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
            mesh_ui_capture_render(capture, &snapshot);

            const struct mesh_ui_theme *theme = mesh_ui_capture_theme(capture);
            const uint32_t bg = rgb_key(mesh_ui_theme_color(theme, MESH_UI_COLOR_BG));
            const uint32_t panel_fill =
                rgb_key(mesh_ui_theme_color(theme, MESH_UI_COLOR_SURFACE_HIGH));

            uint32_t top = 0U;
            uint32_t bottom = 0U;
            panel_rows(pixels, width, height, stride, theme, &top, &bottom);
            if (top >= bottom) {
                failure = "the dialog drew no raised panel";
                mesh_ui_capture_close(capture);
                capture = NULL;
                break;
            }

            /* The panel's left edge, taken from the widest of its rows so a rounded corner is
               not mistaken for the inset. */
            uint32_t left = width;
            for (uint32_t y = top; y <= bottom; ++y) {
                const uint8_t *row = pixels + (size_t)y * stride;
                for (uint32_t x = 0; x < left; ++x) {
                    if (pixel_key(row + (size_t)x * 4U) == panel_fill) {
                        left = x;
                        break;
                    }
                }
            }

            /* Anything drawn to the left of the panel, on the rows the panel covers, escaped
               it - the clipped cancel button landed exactly here. The panel's own edge is laid
               down just outside its fill and is not an escape. */
            const uint32_t outline = rgb_key(mesh_ui_theme_color(theme, MESH_UI_COLOR_OUTLINE));
            size_t escaped = 0U;
            for (uint32_t y = top; y <= bottom && left > 0U; ++y) {
                const uint8_t *row = pixels + (size_t)y * stride;
                for (uint32_t x = 0; x < left; ++x) {
                    const uint32_t key = pixel_key(row + (size_t)x * 4U);
                    if (key != bg && key != outline) {
                        ++escaped;
                    }
                }
            }
            mesh_ui_capture_close(capture);
            capture = NULL;

            if (escaped > 0U) {
                failure = "a dialog control is drawn outside its own panel";
            }
        }
    }

    if (capture != NULL) {
        mesh_ui_capture_close(capture);
    }
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
