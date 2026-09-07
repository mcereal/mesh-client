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

#include "mesh/core/message.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/backends/fb_capture.h"
#include "mesh/ui/font.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "mesh/ui/theme.h"

#include "../../src/ui/backends/fb_internal.h"
#include "../../src/ui/backends/fb_widgets.h"

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

/* The lowest scanline carrying a run of `role` across most of the width, or `height` when
   there is none. A card's top and bottom edges are the only thing on these screens that puts an
   unbroken band of the outline or the accent across the panel. */
static uint32_t last_wide_run_y(const struct mesh_ui_capture *capture, const uint8_t *pixels,
                                uint32_t width, uint32_t height, size_t stride,
                                enum mesh_ui_color role) {
    uint32_t last = height;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        unsigned run = 0U;
        for (uint32_t x = 0U; x < width; ++x) {
            run = pixel_is_role(capture, row + (size_t)x * 4U, role) ? run + 1U : 0U;
            if ((uint64_t)run * 100U / width >= 80U) {
                last = y;
                break;
            }
        }
    }
    return last;
}

/* The bottom edge of the lowest card on the frame, whichever ink it is drawn in - a focused
   card's edge is the accent and every other card's is the outline. */
static uint32_t last_card_edge_y(const struct mesh_ui_capture *capture, const uint8_t *pixels,
                                 uint32_t width, uint32_t height, size_t stride) {
    const uint32_t outline =
        last_wide_run_y(capture, pixels, width, height, stride, MESH_UI_COLOR_OUTLINE);
    const uint32_t ring =
        last_wide_run_y(capture, pixels, width, height, stride, MESH_UI_COLOR_PRIMARY);
    if (outline == height) {
        return ring;
    }
    if (ring == height) {
        return outline;
    }
    return outline > ring ? outline : ring;
}

/*
 * The three card variants, and the ring that says which card the next press acts on.
 *
 * Same approach as the case above - structure, never pixels. What is asked for is that the
 * Status column is drawn at more than one weight: a card is one of three surface tiers, and a
 * screen that lost the variant would draw all three in MESH_UI_COLOR_SURFACE and leave the
 * raised tier nowhere on the frame. SURFACE_HIGH spanning most of the width is the Link card
 * and nothing else on this screen; the fixture's radio has said nothing about itself, so its
 * Radio card is the outlined one and carries no fill of its own at all.
 *
 * The ring is the second half. A card holding the selected verb draws its edge in the primary
 * rather than in the outline, so a full-width run of MESH_UI_COLOR_PRIMARY appears in the body
 * and appears nowhere else: the navigation bar's active chip is the primary *container*, and
 * every other use of the base colour here is a glyph, which is at most a stroke wide.
 */
MESH_TEST_CASE(ui_capture_draws_the_card_variants, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

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

    const unsigned raised =
        widest_row_run(capture, pixels, width, height, stride, MESH_UI_COLOR_SURFACE_HIGH);
    MESH_TEST_FAIL_IF_CLEANUP(raised < 80U, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "no card is drawn on the raised tier, so the variant is lost");

    const unsigned ring =
        widest_row_run(capture, pixels, width, height, stride, MESH_UI_COLOR_PRIMARY);
    MESH_TEST_FAIL_IF_CLEANUP(ring < 80U, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the focused card draws no ring, so nothing says what A acts on");

    /*
     * Where the lowest card ends, which must not depend on where the cursor is.
     *
     * The ring is a thicker edge, and an earlier draft grew the card's *layout* edge to draw
     * it - so selecting a card made it taller, pushed every card under it down the panel and
     * could change which rows were clipped, all because the cursor arrived. The ring is painted
     * inward into the padding now, and this is what says so.
     */
    const uint32_t bottom_before = last_card_edge_y(capture, pixels, width, height, stride);

    /* And it moves. The cursor steps to the Radio card's verb, which is a different card, so
       the ring has to end up on a different scanline - a ring painted at a fixed place would
       pass every check above and still be wrong. */
    unsigned first_ring_y = height;
    for (uint32_t y = 0U; y < height && first_ring_y == height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        unsigned run = 0U;
        for (uint32_t x = 0U; x < width; ++x) {
            run =
                pixel_is_role(capture, row + (size_t)x * 4U, MESH_UI_COLOR_PRIMARY) ? run + 1U : 0U;
            if ((uint64_t)run * 100U / width >= 80U) {
                first_ring_y = y;
                break;
            }
        }
    }

    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no second snapshot");
    mesh_ui_capture_render(capture, &snapshot);

    unsigned moved_ring_y = height;
    for (uint32_t y = 0U; y < height && moved_ring_y == height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        unsigned run = 0U;
        for (uint32_t x = 0U; x < width; ++x) {
            run =
                pixel_is_role(capture, row + (size_t)x * 4U, MESH_UI_COLOR_PRIMARY) ? run + 1U : 0U;
            if ((uint64_t)run * 100U / width >= 80U) {
                moved_ring_y = y;
                break;
            }
        }
    }
    MESH_TEST_FAIL_IF_CLEANUP(
        moved_ring_y == height || moved_ring_y == first_ring_y, mesh_ui_capture_close(capture);
        mesh_ui_store_shutdown(&store), "Down did not move the ring onto the next card's verb");

    const uint32_t bottom_after = last_card_edge_y(capture, pixels, width, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(bottom_after != bottom_before, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "moving the cursor resized a card and shifted the column");

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

/*
 * The top app bar's trailing slot: a fact about the *screen*, in the one place a title could
 * not carry one.
 *
 * "3 unsaved" used to be " (unsaved)" glued onto the end of the breadcrumb with a %s, where it
 * was neither countable nor a capsule - so the thing worth pinning is that it is a filled shape
 * in the chrome rather than more words in the title, and that it is not there when there is
 * nothing to report. Position rather than colour alone: a badge that drifted into the body
 * would still be the right colour.
 */
MESH_TEST_CASE(ui_capture_app_bar_badges_unsaved_edits, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    while (store.nav.screen != MESH_UI_SCREEN_SETTINGS) {
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_settings_open(&store, MESH_UI_SETTINGS_DISPLAY),
                              mesh_ui_store_shutdown(&store), "could not open a settings section");

    struct mesh_ui_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, 4) != 0,
        mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);

    /* Wide enough that a warning-toned glyph cannot pass for a capsule, narrow enough that the
       shortest badge ("1 unsaved" at the smallest glyph scale a theme picks) still clears it. */
    const unsigned capsule = 60U;
    /* The chrome: the navigation bar and the app bar under it. The body starts well below. */
    const uint32_t chrome = height / 6U;

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    mesh_ui_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(
        topmost_row_run(capture, pixels, width, height, stride, MESH_UI_COLOR_WARNING, capsule) <
            chrome,
        mesh_ui_capture_close(capture);
        mesh_ui_store_shutdown(&store),
        "a section with nothing pending should carry no badge in its app bar");

    store.nav.settings_edit_count = 3U;
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    mesh_ui_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(
        topmost_row_run(capture, pixels, width, height, stride, MESH_UI_COLOR_WARNING, capsule) >=
            chrome,
        mesh_ui_capture_close(capture);
        mesh_ui_store_shutdown(&store),
        "pending edits should draw a filled badge in the app bar's trailing slot");

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

/*
 * ---- step 10's two selection components ----------------------------------------------------
 *
 * Both are asserted the same way, and it is the way the dialog case above is: render the same
 * screen twice with one thing changed, and require the pixels in the region that should have
 * answered to differ. A control that says which of a set is chosen has exactly one job, and a
 * palette or a layout can take it away silently - which is the failure these exist for.
 */

/*
 * Where a settings row's label column ends, in pixels: the room the row has promised to its own
 * words before any trailing slot gets a say.
 *
 * Derived from the theme's own tokens rather than guessed at, because that is what the renderer
 * measures it from - the narrow case included, where a panel too tight for the stated column
 * gives the label half the line instead. A test that assumed the stated width would pass at the
 * scales where it is right and accuse the renderer at the two where it is not.
 */
static uint32_t settings_label_right(const struct mesh_ui_theme *theme, uint32_t width, int scale) {
    const struct mesh_ui_font *font = mesh_ui_font_by_id(theme->font_id);
    const int advance = mesh_ui_font_advance(font, scale);
    const int margin = (int)theme->metrics.margin;
    const int usable = (int)width - 2 * margin;
    const size_t cols = usable > 0 ? (size_t)(usable / advance) : 1U;
    const size_t label_cols =
        cols < theme->metrics.narrow_cols ? cols / 2U : theme->metrics.field_label_cols;
    return (uint32_t)(margin + (int)label_cols * advance);
}

/* Renders `store` as it stands into a fresh capture at `scale`, and hands back a copy of the
   page. The caller frees it. */
static uint8_t *capture_frame(struct mesh_ui_store *store, const struct mesh_ui_theme *theme,
                              int scale, uint32_t *out_w, uint32_t *out_h, size_t *out_stride) {
    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(store);
    if (!mesh_ui_store_consume_updates(store, &snapshot)) {
        return NULL;
    }
    struct mesh_ui_capture *capture = NULL;
    if (mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, scale) != 0) {
        return NULL;
    }
    mesh_ui_capture_set_theme(capture, theme);
    /* After the theme, not before: a theme carries a scale of its own and adopting one unpins
       whatever was asked for at open. This is the same order the scene scripts' `theme` and
       `scale` lines are read in. */
    mesh_ui_capture_set_scale(capture, scale);
    const uint8_t *pixels = mesh_ui_capture_pixels(capture, out_w, out_h, out_stride);
    mesh_ui_capture_render(capture, &snapshot);
    uint8_t *frame = malloc(*out_stride * (size_t)*out_h);
    if (frame != NULL) {
        memcpy(frame, pixels, *out_stride * (size_t)*out_h);
    }
    mesh_ui_capture_close(capture);
    return frame;
}

/*
 * A short enum's row says *which* of its choices is set, and says it without touching the label.
 *
 * Two frames of Settings > Bluetooth with the pairing mode moved from the first of its three
 * values to the last. Two things have to hold and the second is the one that is easy to lose:
 * the value column must change, because a control that shows the set and not the choice is
 * worse than the word it replaced - and the *label* column must not, because the segmented
 * button is the first trailing slot wide enough to reach it, and a slot fitted against the
 * whole line rather than against the room actually free lands on the row's own words.
 *
 * Across the scale range as well as across the themes: the fit is a measurement, and the point
 * at which the segments stop fitting is precisely where a bad measurement stops being visible
 * on the one panel this ships on.
 */
MESH_TEST_CASE(ui_capture_segmented_marks_the_chosen_value, unit) {
    const char *failure = NULL;

    for (size_t t = 0; t < mesh_ui_theme_count() && failure == NULL; ++t) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(t);
        for (int scale = MESH_UI_SCALE_MIN; scale <= MESH_UI_SCALE_MAX && failure == NULL;
             ++scale) {
            uint8_t *frames[2] = {NULL, NULL};
            uint32_t width = 0U;
            uint32_t height = 0U;
            size_t stride = 0U;

            for (unsigned pass = 0U; pass < 2U && failure == NULL; ++pass) {
                struct mesh_ui_store store;
                if (mesh_ui_store_init(&store) != 0) {
                    failure = "store init failed";
                    break;
                }
                mesh_test_nav_populate(&store);
                struct mesh_ui_settings settings = store.settings;
                settings.loaded = true;
                settings.has_bluetooth = true;
                settings.bluetooth_enabled = true;
                /* 0 and 2 of the three: the ends of the set, so the fill has moved the whole
                   width of the control and no theme can pass by accident. */
                settings.pairing_mode = pass == 0U ? 0U : 2U;
                mesh_ui_store_set_settings(&store, &settings);

                struct mesh_ui_action action;
                for (int i = 0; i < 4; ++i) {
                    mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
                }
                if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_BLUETOOTH)) {
                    failure = "could not open Settings > Bluetooth";
                } else {
                    frames[pass] = capture_frame(&store, theme, scale, &width, &height, &stride);
                    if (frames[pass] == NULL) {
                        failure = "capture failed";
                    }
                }
                mesh_ui_store_shutdown(&store);
            }

            if (failure == NULL) {
                /*
                 * The pairing row is the second body row of the section, and the body starts
                 * under the app bar - so rather than deriving where that is, the whole body is
                 * swept in two columns. Nothing else on this screen moved between the frames.
                 */
                const uint32_t body_top = height / 8U;
                const uint32_t body_bottom = height - height / 8U;
                const uint32_t label_right = settings_label_right(theme, width, scale);
                const size_t label_changed =
                    differing_in(frames[0], frames[1], stride, theme->metrics.margin,
                                 label_right < width ? label_right : width, body_top, body_bottom);
                const size_t value_changed = differing_in(frames[0], frames[1], stride,
                                                          label_right < width ? label_right : 0U,
                                                          width - 1U, body_top, body_bottom);
                if (value_changed == 0U) {
                    failure = "the pairing row draws the same for two different values - which "
                              "of the set is chosen cannot be told";
                } else if (label_changed != 0U) {
                    failure = "changing a row's value redrew its label column, so the trailing "
                              "slot is being laid over the row's own words";
                }
            }
            free(frames[0]);
            free(frames[1]);
        }
    }

    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A choice the set does not contain is drawn as words, not as the first segment lit.
 *
 * The radio can report an enum value outside what this build knows - a newer firmware's, or a
 * corrupt one - and the settings item keeps it and formats it as "Unknown". A segmented button
 * that clamped it into range would answer that by lighting `Random PIN`, which states a
 * configuration nobody knows. So the two frames below, one with a value in the set and one with
 * a value outside it, have to *differ*: under the clamp they render identically, which is the
 * whole of the bug.
 */
MESH_TEST_CASE(ui_capture_segmented_refuses_an_unknown_value, unit) {
    const char *failure = NULL;

    for (size_t t = 0; t < mesh_ui_theme_count() && failure == NULL; ++t) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(t);
        uint8_t *frames[2] = {NULL, NULL};
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;

        for (unsigned pass = 0U; pass < 2U && failure == NULL; ++pass) {
            struct mesh_ui_store store;
            if (mesh_ui_store_init(&store) != 0) {
                failure = "store init failed";
                break;
            }
            mesh_test_nav_populate(&store);
            struct mesh_ui_settings settings = store.settings;
            settings.loaded = true;
            settings.has_bluetooth = true;
            settings.bluetooth_enabled = true;
            /* The first of the three, then one past the last of them. */
            settings.pairing_mode = pass == 0U ? 0U : 9U;
            mesh_ui_store_set_settings(&store, &settings);

            struct mesh_ui_action action;
            for (int i = 0; i < 4; ++i) {
                mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
            }
            if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_BLUETOOTH)) {
                failure = "could not open Settings > Bluetooth";
            } else {
                frames[pass] = capture_frame(&store, theme, 4, &width, &height, &stride);
                if (frames[pass] == NULL) {
                    failure = "capture failed";
                }
            }
            mesh_ui_store_shutdown(&store);
        }

        if (failure == NULL) {
            const uint32_t body_top = height / 8U;
            const uint32_t body_bottom = height - height / 8U;
            if (differing_in(frames[0], frames[1], stride, 0U, width - 1U, body_top, body_bottom) ==
                0U) {
                failure = "a pairing mode outside the set draws exactly as the first one does - "
                          "the control is claiming a configuration the radio never reported";
            }
        }
        free(frames[0]);
        free(frames[1]);
    }

    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The picker marks the current target, and marks it in the trailing slot rather than in the disc.
 *
 * Two frames with the target moved from the first row to the second. What has to change is the
 * trailing column of both rows - the mark left one and arrived at the other - and what has to
 * stay put is the leading column, because the disc carries the node's identity and the whole
 * reason this became a radio is that a stated accent fill was overwriting it.
 */
MESH_TEST_CASE(ui_capture_picker_marks_the_current_target, unit) {
    const char *failure = NULL;

    for (size_t t = 0; t < mesh_ui_theme_count() && failure == NULL; ++t) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(t);
        uint8_t *frames[2] = {NULL, NULL};
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;

        for (unsigned pass = 0U; pass < 2U && failure == NULL; ++pass) {
            struct mesh_ui_store store;
            if (mesh_ui_store_init(&store) != 0) {
                failure = "store init failed";
                break;
            }
            mesh_test_nav_populate(&store);
            store.nav.screen = MESH_UI_SCREEN_MESSAGES;
            store.nav.picker_open = true;
            store.nav.picker_cursor = 0U;
            /* The first two rows of the picker are the two channels the fixture carries, so
               the target moves one row without the list itself changing. */
            store.nav.target_node = MESH_MESSAGE_BROADCAST_ADDR;
            store.nav.target_channel = (uint8_t)pass;
            frames[pass] = capture_frame(&store, theme, 4, &width, &height, &stride);
            if (frames[pass] == NULL) {
                failure = "capture failed";
            }
            mesh_ui_store_shutdown(&store);
        }

        if (failure == NULL) {
            const uint32_t body_top = height / 8U;
            const uint32_t body_bottom = height / 2U;
            /* The trailing column: the last tenth of the panel, which is where every trailing
               slot in this UI ends up whatever the row is. */
            const size_t trailing_changed =
                differing_in(frames[0], frames[1], stride, (width * 9U) / 10U, width - 1U, body_top,
                             body_bottom);
            /* The leading column: the discs, and the one thing that must be the same in both. */
            const size_t leading_changed =
                differing_in(frames[0], frames[1], stride, 0U, width / 12U, body_top, body_bottom);
            if (trailing_changed < 20U) {
                failure = "moving the target did not move the mark in the picker's trailing "
                          "column - which row is chosen cannot be told on this theme";
            } else if (leading_changed != 0U) {
                failure = "moving the target redrew a row's avatar, so identity and selection "
                          "are being said in the same slot again";
            }
        }
        free(frames[0]);
        free(frames[1]);
    }

    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* These device-memory boundaries are independent of the renderer's theme and content. */

MESH_TEST_CASE(fb_damage_preserves_mirror_and_padding, unit) {
    uint8_t mapping[80];
    uint8_t previous[32] = {0};
    uint8_t frame[32];
    memset(mapping, 0xA5, sizeof mapping);
    memset(frame, 0x31, sizeof frame);
    struct mesh_ui_backend_fb_state state = {0};
    state.fb_ptr = mapping;
    state.fb_size = sizeof mapping;
    state.line_bytes = 16U;
    state.bytes_per_pixel = 4U;
    state.var.xres = 3U; /* a padded row */
    state.var.yres = 2U;
    state.var.yres_virtual = 5U;
    MESH_TEST_FAIL_IF(fb_copy_damage(&state, frame, previous, true) != 64U,
                      "first frame must initialize both pages");
    MESH_TEST_FAIL_IF(memcmp(mapping, frame, 32U) != 0 || memcmp(mapping + 32U, frame, 32U) != 0,
                      "display pages must match the rendered frame");
    MESH_TEST_FAIL_IF(fb_copy_damage(&state, frame, previous, false) != 0U,
                      "an unchanged frame must not write display memory");
    frame[19] ^= 1U;
    MESH_TEST_FAIL_IF(fb_copy_damage(&state, frame, previous, false) != 8U,
                      "one changed pixel must write only one pixel per page");
    MESH_TEST_FAIL_IF(memcmp(mapping, frame, 32U) != 0 || memcmp(mapping + 32U, frame, 32U) != 0,
                      "partial updates must preserve both complete pages");
    for (size_t i = 64U; i < sizeof mapping; ++i) {
        MESH_TEST_FAIL_IF(mapping[i] != 0xA5, "updates must not touch extra virtual pages");
    }
    state.var.yres_virtual = 2U;
    frame[0] ^= 1U;
    MESH_TEST_FAIL_IF(fb_copy_damage(&state, frame, previous, false) != 4U,
                      "a single-page display must receive one copy");
    record_success(test_name);
}

MESH_TEST_CASE(fb_glyph_cache_matches_uncached_colors_and_scales, unit) {
    uint8_t cached_pixels[256U * 128U * 4U];
    uint8_t reference[sizeof cached_pixels];
    struct mesh_ui_backend_fb_state state = {0};
    state.fb_size = sizeof cached_pixels;
    state.var.xres = 256U;
    state.var.yres = 128U;
    state.var.bits_per_pixel = 32U;
    state.fix.line_length = 256U * 4U;
    state.bytes_per_pixel = 4U;
    const char *failure = NULL;
    fb_state_set_theme(&state, mesh_ui_theme_default(), 4);
    struct fb_glyph_cache *cache = state.glyph_cache;
    for (int scale = 2; scale <= 6 && failure == NULL; ++scale) {
        for (unsigned pass = 0; pass < 3U; ++pass) {
            const struct mesh_ui_rgb ink = {(uint8_t)(pass * 91U), 170U, 250U};
            const struct mesh_ui_rgb ground = {30U, (uint8_t)(pass * 71U), 10U};
            state.fb_ptr = cached_pixels;
            state.glyph_cache = cache;
            fb_clear(&state, ground);
            fb_draw_text(&state, -3, 10, "Ab éñ!?", scale, ink, ground);
            state.fb_ptr = reference;
            state.glyph_cache = NULL;
            fb_clear(&state, ground);
            fb_draw_text(&state, -3, 10, "Ab éñ!?", scale, ink, ground);
            if (memcmp(reference, cached_pixels, sizeof reference) != 0) {
                failure = "cached glyphs must match uncached output after color and scale changes";
                break;
            }
        }
    }
    state.glyph_cache = cache;
    fb_glyph_cache_free(&state);
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(fb_transcript_cache_matches_reference_after_mutations, unit) {
    struct mesh_ui_capture *cached = NULL, *reference = NULL;
    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    const char *failure = NULL;
    if (snapshot == NULL || mesh_ui_capture_open(&cached, 1024U, 768U, 4) != 0 ||
        mesh_ui_capture_open(&reference, 1024U, 768U, 4) != 0) {
        failure = "capture allocation failed";
        goto cleanup;
    }
    mesh_ui_capture_set_reference(reference, true);
    snapshot->nav.screen = MESH_UI_SCREEN_MESSAGES;
    snapshot->nav.thread_open = true;
    snapshot->nav.inbox = true;
    snapshot->messages.count = MESH_UI_MAX_MESSAGES;
    for (uint32_t i = 0; i < MESH_UI_MAX_MESSAGES; ++i) {
        struct mesh_ui_message *message = &snapshot->messages.entries[i];
        message->packet_id = i + 1U;
        message->peer = 2U;
        message->broadcast = true;
        message->rx_time = 1788000000U + i * 1000U;
        snprintf(message->peer_name, sizeof message->peer_name, "ALFA");
        snprintf(message->text, sizeof message->text,
                 "Message %u with enough text to wrap into several lines on a narrow screen.", i);
    }
    for (unsigned pass = 0; pass < 12U; ++pass) {
        snapshot->nav.cursor[MESH_UI_SCREEN_MESSAGES] = pass % 2U == 0 ? 20U : 63U;
        if (pass == 2U)
            snprintf(snapshot->messages.entries[20].text,
                     sizeof snapshot->messages.entries[20].text, "edited");
        if (pass == 3U)
            snprintf(snapshot->messages.entries[63].peer_name,
                     sizeof snapshot->messages.entries[63].peer_name, "RENAMED");
        if (pass == 4U) {
            snapshot->messages.entries[62].is_reaction = true;
            snapshot->messages.entries[62].reply_id = 64U;
            snprintf(snapshot->messages.entries[62].text,
                     sizeof snapshot->messages.entries[62].text, "!");
        }
        if (pass == 5U) {
            snapshot->messages.entries[63].direction = MESH_MESSAGE_OUTBOUND;
            snapshot->messages.entries[63].ack = MESH_MESSAGE_ACK_FAILED;
        }
        if (pass == 6U)
            mesh_i18n_set_locale("es");
        if (pass == 7U) {
            mesh_ui_capture_set_scale(cached, 3);
            mesh_ui_capture_set_scale(reference, 3);
        }
        if (pass == 8U)
            snapshot->nav.inbox = false;
        if (pass == 9U)
            snapshot->nav.target_node = MESH_MESSAGE_BROADCAST_ADDR;
        if (pass == 10U)
            snapshot->messages.count = 1U;
        mesh_ui_capture_render(cached, snapshot);
        mesh_ui_capture_render(reference, snapshot);
        if (memcmp(mesh_ui_capture_pixels(cached, NULL, NULL, NULL),
                   mesh_ui_capture_pixels(reference, NULL, NULL, NULL), 1024U * 768U * 4U) != 0) {
            failure = "cached transcript differs after navigation or input mutation";
            break;
        }
    }
cleanup:
    mesh_i18n_set_locale("en");
    mesh_ui_capture_close(cached);
    mesh_ui_capture_close(reference);
    free(snapshot);
    if (failure != NULL)
        record_failure(test_name, failure);
    else
        record_success(test_name);
}

MESH_TEST_CASE(fb_animation_clip_matches_full_composition, unit) {
    struct mesh_ui_backend_fb_state state[2] = {0};
    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    const char *failure = NULL;
    unsigned clipped = 0U;
    if (snapshot == NULL) {
        record_failure(test_name, "snapshot allocation failed");
        return;
    }
    for (unsigned i = 0; i < 2U; ++i) {
        state[i].var.xres = 1024U;
        state[i].var.yres = 768U;
        state[i].var.bits_per_pixel = 32U;
        state[i].line_bytes = state[i].fix.line_length = 4096U;
        state[i].bytes_per_pixel = 4U;
        state[i].fb_size = 4096U * 768U;
        state[i].fb_ptr = calloc(1U, state[i].fb_size);
        if (state[i].fb_ptr == NULL) {
            failure = "frame allocation failed";
            goto cleanup;
        }
        fb_state_set_theme(&state[i], mesh_ui_theme_default(), 4);
    }
    state[1].partial_disabled = true;
    snapshot->nav.screen = MESH_UI_SCREEN_MESSAGES;
    for (unsigned frame = 0U; frame < 50U; ++frame) {
        if (frame == 1U || frame == 20U) {
            snprintf(snapshot->nav.toast, sizeof snapshot->nav.toast, "Saved");
            snapshot->nav.toast_until_ms = 5000U + frame;
        }
        if (frame == 15U || frame == 35U)
            snapshot->nav.toast[0] = '\0';
        if (frame == 25U)
            snapshot->nav.screen = MESH_UI_SCREEN_NODES;
        if (frame == 30U) {
            fb_state_set_theme(&state[0], mesh_ui_theme_default(), 3);
            fb_state_set_theme(&state[1], mesh_ui_theme_default(), 3);
        }
        for (unsigned i = 0U; i < 2U; ++i) {
            fb_state_set_now(&state[i], 1000U + frame * 16U);
            fb_render_snapshot(&state[i], snapshot);
            /* A second animation above the snackbar must participate in the next clip. */
            const struct fb_selection selection = {
                .id = 0x7FFFFFFEU,
                .rect = {.x = 80, .y = 200, .w = 24, .h = 24},
                .shape = FB_SELECTION_RADIO,
                .on = frame >= 5U && frame < 18U,
            };
            fb_draw_selection(&state[i], &selection);
        }
        if (state[0].clip_active)
            clipped++;
        if (memcmp(state[0].fb_ptr, state[1].fb_ptr, state[0].fb_size) != 0) {
            failure = "animation clip must restore overlapping content on arrival and dismissal";
            goto cleanup;
        }
    }
    if (clipped == 0U)
        failure = "comparison did not exercise partial drawing";
cleanup:
    for (unsigned i = 0U; i < 2U; ++i) {
        fb_glyph_cache_free(&state[i]);
        fb_thread_cache_free(&state[i]);
        fb_render_cache_free(&state[i]);
        free(state[i].fb_ptr);
    }
    free(snapshot);
    if (failure != NULL)
        record_failure(test_name, failure);
    else
        record_success(test_name);
}
