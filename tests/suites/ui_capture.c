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
#include "support/map_fixture.h"
#include "support/ui_fixture.h"

#include "mesh/core/message.h"
#include "mesh/core/updater.h"
#include "mesh/i18n/strings.h"
#include "mesh/map/viewport.h"
#include "mesh/ui/backends/fb_capture.h"
#include "mesh/ui/font.h"
#include "mesh/ui/history.h"
#include "mesh/ui/map.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"
#include "mesh/ui/theme.h"
#include "mesh/utils/time.h"

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

/* How many wide bands of `role` there are down the frame - a band being a run of scanlines,
   so a card's two-pixel edge counts once. Three cards on the Status screen is six: a top and a
   bottom each, whichever ink they are drawn in. */
static unsigned count_edge_bands(const struct mesh_ui_capture *capture, const uint8_t *pixels,
                                 uint32_t width, uint32_t height, size_t stride) {
    unsigned bands = 0U;
    bool inside = false;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        bool wide = false;
        unsigned outline = 0U;
        unsigned ring = 0U;
        for (uint32_t x = 0U; x < width; ++x) {
            const uint8_t *pixel = row + (size_t)x * 4U;
            outline = pixel_is_role(capture, pixel, MESH_UI_COLOR_OUTLINE) ? outline + 1U : 0U;
            ring = pixel_is_role(capture, pixel, MESH_UI_COLOR_PRIMARY) ? ring + 1U : 0U;
            if ((uint64_t)outline * 100U / width >= 80U || (uint64_t)ring * 100U / width >= 80U) {
                wide = true;
                break;
            }
        }
        if (wide && !inside) {
            bands += 1U;
        }
        inside = wide;
    }
    return bands;
}

/*
 * The Status screen keeps its last card when the card above it overflows.
 *
 * The failure this pins is invisible to the compiler and nearly invisible on the panel, which
 * is why it shipped: a column of cards is drawn top down and each takes what it wants, so the
 * *last* card pays for everything above it - and paying means fb_draw_card() refusing it
 * outright. On the Status screen that card is the Radio card, and it carries the `refresh`
 * verb. mesh_ui_status_actions() offers that verb from the link state alone, with no idea what
 * was drawn, so the cursor kept walking onto a button that was not on the frame - which is the
 * failure "a card that can end up with no rows must not be given a verb" reached from the
 * layout side rather than the row-count side.
 *
 * The Mesh card is made as tall as it ever gets: a LocalStats report puts the counter rows and
 * the composition up, and two airtime readings put the trend up, which is two more body rows.
 * That is not a contrived state - it is what a Brick shows a few minutes after connecting.
 *
 * Counted in card *edges* rather than in rows, because that is the thing a dropped card takes
 * with it: three cards is six bands, and a Radio card refused for want of room is four.
 */
MESH_TEST_CASE(ui_capture_status_keeps_the_last_card_when_the_one_above_overflows, unit) {
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

    /* The radio's own report, which is what raises every counter row on the Mesh card and the
       composition under them. The figures are the demo scene's, so this and the picture a
       reviewer looks at are the same mesh. */
    snapshot.settings.stats.valid = true;
    snapshot.settings.stats.uptime_seconds = 806400U;
    snapshot.settings.stats.channel_utilization = 11.5F;
    snapshot.settings.stats.air_util_tx = 3.2F;
    snapshot.settings.stats.num_packets_tx = 1462U;
    snapshot.settings.stats.num_packets_rx = 5871U;
    snapshot.settings.stats.num_packets_rx_bad = 12U;
    snapshot.settings.stats.num_rx_dupe = 431U;
    snapshot.settings.stats.num_tx_relay = 268U;
    snapshot.settings.stats.num_tx_dropped = 3U;
    snapshot.settings.stats.num_online_nodes = 9U;
    snapshot.settings.stats.num_total_nodes = 42U;

    /* And two readings of it, which is what puts the trend up - the two rows that were already
       costing the Radio card its place before the composition existed. */
    mesh_ui_history_reset(&snapshot.history);
    mesh_ui_history_note_airtime(&snapshot.history, 1000U, 115, 32);
    mesh_ui_history_note_airtime(&snapshot.history, 400000U, 580, 190);

    struct mesh_ui_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, 4) != 0,
        mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
    mesh_ui_capture_render(capture, &snapshot);

    const unsigned bands = count_edge_bands(capture, pixels, width, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(bands < 6U, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "a card was squeezed off the Status screen by the one above it");

    mesh_ui_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
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
 * The lowest scanline on which two frames differ, or `height` when they are identical.
 *
 * The two chrome containers this file cares about are told apart by exactly this number: one is
 * required to change nothing below the navigation bar and the other is required to move the
 * whole body, so "where does the difference stop" is the assertion in both cases, with the
 * comparison in opposite directions.
 */
static uint32_t last_differing_row(const uint8_t *a, const uint8_t *b, uint32_t width,
                                   uint32_t height, size_t stride) {
    uint32_t last = height;
    for (uint32_t y = 0U; y < height; ++y) {
        if (memcmp(a + (size_t)y * stride, b + (size_t)y * stride, (size_t)width * 4U) != 0) {
            last = y;
        }
    }
    return last;
}

/* A copy of the page as it stands, so a second render can be compared against it. */
static uint8_t *snapshot_page(const uint8_t *pixels, uint32_t height, size_t stride) {
    uint8_t *copy = malloc((size_t)height * stride);
    if (copy != NULL) {
        memcpy(copy, pixels, (size_t)height * stride);
    }
    return copy;
}

/*
 * The screen progress bar costs no row, and the banner costs the rows it takes.
 *
 * These are one case because they are one rule read from both ends. An indicator that changes
 * the layout is an indicator that moves what it is pointing at - which is why a request going
 * out must not reflow the list it was sent from, and it is the same correction the card's focus
 * ring needed. A banner is the deliberate exception: it is content about the client, it is
 * meant to cost rows, and a banner that did not shorten the body would be drawing over it.
 *
 * Both are checked by where two frames stop differing rather than by a pixel anywhere, because
 * that is the only form of the assertion that survives a legitimate change to either drawing.
 */
MESH_TEST_CASE(ui_capture_progress_costs_no_row_and_the_banner_costs_rows, unit) {
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

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no snapshot to render");
    mesh_ui_capture_render(capture, &snapshot);
    uint8_t *quiet = snapshot_page(pixels, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(quiet == NULL, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "out of memory");

    /* An admin read on its way back: work outstanding, and nothing else about the frame
       changed. */
    struct mesh_ui_settings settings = store.settings;
    settings.admin_busy = true;
    mesh_ui_store_set_settings(&store, &settings);
    mesh_ui_store_request_refresh(&store);
    memset(&snapshot, 0, sizeof snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot), free(quiet);
                              mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no busy snapshot");
    mesh_ui_capture_render(capture, &snapshot);

    const uint32_t bar_last = last_differing_row(quiet, pixels, width, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(bar_last == height, free(quiet); mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "work in flight drew nothing at all");
    /* An eighth of the panel is far more room than the tab strip and its rule take, and far
       less than the first body row reaches. A bar that consumed rows would push the whole list
       down and put this at the bottom of the frame. */
    MESH_TEST_FAIL_IF_CLEANUP(bar_last >= height / 8U, free(quiet); mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the progress bar moved the body, so a request reflows the list");

    /* And the other end of the rule. An installed release is a banner, a banner is content, and
       content moves the list under it. */
    settings.admin_busy = false;
    settings.client.update_state = (uint8_t)MESH_UPDATE_READY;
    snprintf(settings.client.update_latest, sizeof settings.client.update_latest, "%s", "9.9.9");
    mesh_ui_store_set_settings(&store, &settings);
    mesh_ui_store_request_refresh(&store);
    memset(&snapshot, 0, sizeof snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot), free(quiet);
                              mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no banner snapshot");
    mesh_ui_capture_render(capture, &snapshot);

    const unsigned container =
        widest_row_run(capture, pixels, width, height, stride, MESH_UI_COLOR_SUCCESS_CONTAINER);
    MESH_TEST_FAIL_IF_CLEANUP(container < 80U, free(quiet); mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no banner container on the frame");
    const uint32_t banner_last = last_differing_row(quiet, pixels, width, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(
        banner_last <= height / 2U, free(quiet); mesh_ui_capture_close(capture);
        mesh_ui_store_shutdown(&store), "the banner drew over the body instead of shortening it");

    free(quiet);
    mesh_ui_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
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

/* The topmost and bottom-most rows with anything drawn on them, which on a full frame are
   inside the navigation bar and inside the status line under the keycaps. Found rather than
   stated, so a test about chrome not moving does not carry its own copy of the layout. */
static uint32_t topmost_drawn_row(const struct mesh_ui_capture *capture, const uint8_t *pixels,
                                  uint32_t width, uint32_t height, size_t stride) {
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0U; x < width; ++x) {
            if (!pixel_is_background(capture, row + (size_t)x * 4U)) {
                return y;
            }
        }
    }
    return height;
}

static uint32_t bottommost_drawn_row(const struct mesh_ui_capture *capture, const uint8_t *pixels,
                                     uint32_t width, uint32_t height, size_t stride) {
    for (uint32_t y = height; y > 0U; --y) {
        const uint8_t *row = pixels + (size_t)(y - 1U) * stride;
        for (uint32_t x = 0U; x < width; ++x) {
            if (!pixel_is_background(capture, row + (size_t)x * 4U)) {
                return y - 1U;
            }
        }
    }
    return height;
}

/*
 * The leftmost and rightmost drawn column of `page`, counted only over the rows on which it
 * differs from `other`.
 *
 * The band a transition is confined to, read off the two frames rather than recomputed from a
 * layout this file does not have - and the restriction matters: the action bar starts at the
 * body margin on every frame, so a leftmost column taken over the whole page would report the
 * keycaps rather than the screen that is moving.
 */
static void band_extents(const struct mesh_ui_capture *capture, const uint8_t *page,
                         const uint8_t *other, uint32_t width, uint32_t height, size_t stride,
                         uint32_t *out_left, uint32_t *out_right) {
    uint32_t left = width;
    uint32_t right = 0U;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = page + (size_t)y * stride;
        if (memcmp(row, other + (size_t)y * stride, (size_t)width * 4U) == 0) {
            continue;
        }
        for (uint32_t x = 0U; x < width; ++x) {
            if (pixel_is_background(capture, row + (size_t)x * 4U)) {
                continue;
            }
            if (x < left) {
                left = x;
            }
            if (x > right) {
                right = x;
            }
        }
    }
    *out_left = left;
    *out_right = right;
}

/*
 * A screen arriving travels, and only the screen travels.
 *
 * Two claims, and the second is the one that is easy to get wrong. A frame is a function of a
 * snapshot, so the obvious way to animate a move is to offset the whole frame - which slides
 * the tab strip and the keycaps along with the body and says the entire application has been
 * replaced, when what changed is one level of one tab. So the assertions here are: the body is
 * somewhere else on the frame the press lands and back in place once it settles, in the
 * direction the move went; and the topmost and bottom-most drawn rows of the frame - which are
 * the navigation bar and the line under the keycaps - are untouched throughout.
 *
 * Which way round is checked from opposite ends for a reason. A thread is left-anchored, so it
 * announces a rightwards displacement by starting further from the left edge; the conversation
 * list's rows run the width of the panel, so its leftmost column is 0 either way and what moves
 * is where they stop. Measuring the same end for both would pass on a transition that never
 * moved at all.
 */
MESH_TEST_CASE(ui_capture_slides_a_screen_in_and_settles, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    /* Off the all-traffic row, onto a conversation with a transcript in it. */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);

    /*
     * And in and out of it once before anything is measured, which is setup rather than part of
     * the claim.
     *
     * What this case reads is the *band the body moved in*, taken as the columns on which two
     * frames differ - so it needs the two frames to differ by the move and by nothing else. The
     * first visit to a conversation marks it read, which empties its row's badge and drops the
     * count on the Messages tab, and the tab strip has ink from one edge of the panel to the
     * other: a nav bar that legitimately differs between the two captures puts the strip's own
     * extents into the answer and drowns the travel. Reading it first settles the unread state,
     * and every frame below is then taken against the same one.
     */
    struct mesh_ui_snapshot settling;
    memset(&settling, 0, sizeof settling);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    (void)mesh_ui_store_consume_updates(&store, &settling);
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    (void)mesh_ui_store_consume_updates(&store, &settling);

    struct mesh_ui_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, 4) != 0,
        mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no snapshot to render");
    /* The first frame adopts the place it is looking at rather than arriving at it. */
    render_until_still(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_animating(capture), mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the first frame drawn animated something");
    uint8_t *list_settled = snapshot_page(pixels, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(list_settled == NULL, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "out of memory");

    const uint32_t chrome_top = topmost_drawn_row(capture, pixels, width, height, stride);
    const uint32_t chrome_bottom = bottommost_drawn_row(capture, pixels, width, height, stride);

    /* ---- a level deeper: the thread comes in from the right --------------------------- */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.thread_open, free(list_settled);
                              mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "A did not open the thread");
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    mesh_ui_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(
        !mesh_ui_capture_animating(capture), free(list_settled); mesh_ui_capture_close(capture);
        mesh_ui_store_shutdown(&store), "the thread appeared in place instead of arriving");
    uint8_t *arriving = snapshot_page(pixels, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(arriving == NULL, free(list_settled); mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "out of memory");

    render_until_still(capture, &snapshot);

    uint32_t arriving_left = 0U;
    uint32_t arriving_right = 0U;
    uint32_t settled_left = 0U;
    uint32_t settled_right = 0U;
    band_extents(capture, arriving, pixels, width, height, stride, &arriving_left, &arriving_right);
    band_extents(capture, pixels, arriving, width, height, stride, &settled_left, &settled_right);
    const bool travelled_right = arriving_left > settled_left + width / 8U;
    const bool chrome_held =
        memcmp(arriving + (size_t)chrome_top * stride, pixels + (size_t)chrome_top * stride,
               (size_t)width * 4U) == 0 &&
        memcmp(arriving + (size_t)chrome_bottom * stride, pixels + (size_t)chrome_bottom * stride,
               (size_t)width * 4U) == 0;
    free(arriving);
    MESH_TEST_FAIL_IF_CLEANUP(!chrome_held, free(list_settled); mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the navigation bar or the status line travelled with the screen");
    MESH_TEST_FAIL_IF_CLEANUP(!travelled_right, free(list_settled); mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the thread did not arrive from the right");

    /* ---- and back out: the list comes in from the left --------------------------------- */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_B, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.thread_open, free(list_settled);
                              mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "B did not leave the thread");
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    mesh_ui_capture_render(capture, &snapshot);
    uint8_t *returning = snapshot_page(pixels, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(returning == NULL, free(list_settled); mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "out of memory");

    band_extents(capture, returning, list_settled, width, height, stride, &arriving_left,
                 &arriving_right);
    band_extents(capture, list_settled, returning, width, height, stride, &settled_left,
                 &settled_right);
    const bool travelled_left = arriving_right + width / 8U < settled_right;
    free(returning);
    free(list_settled);
    MESH_TEST_FAIL_IF_CLEANUP(!travelled_left, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the conversation list did not come back from the left");

    /* And it does come to rest where it started, which is the half a moving frame cannot say. */
    render_until_still(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_animating(capture), mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "the move never finished");

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
/*
 * A frame is drawn against a clock the caller can pin.
 *
 * Everything time-shaped on the panel - a message's "18:47", a node's "3m", the separator that
 * says "Yesterday" - used to come from time(NULL) inside the renderer, which made a rendered
 * frame a function of when it was rendered. That is invisible on a device and fatal for the
 * screenshots in .github/resources: regenerating them an hour later rewrote the clock column,
 * and either side of midnight moved the day separators. mesh_time_wall_set_fixed() is the seam
 * that fixes it, and this is the contract it has to keep - the same pin draws the same bytes,
 * and a different pin draws different ones, which is what proves the renderer reads it at all.
 */
MESH_TEST_CASE(ui_capture_draws_against_the_pinned_clock, unit) {
    /* Fixed rather than derived from now: a case about a pinned clock must not depend on one. */
    const uint32_t base = 1767200000U; /* 2025-12-31 in UTC, and any zone's version of it */

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    /* The fixture leaves rx_time at zero, which draws no clock and no age at all - so the two
       renderings below would be identical for the wrong reason. */
    struct mesh_ui_message_list messages;
    memset(&messages, 0, sizeof messages);
    messages.count = 1U;
    messages.entries[0].packet_id = 21U;
    messages.entries[0].peer = 0x2000U;
    messages.entries[0].broadcast = true;
    messages.entries[0].direction = MESH_MESSAGE_INBOUND;
    messages.entries[0].rx_time = base;
    snprintf(messages.entries[0].peer_name, sizeof messages.entries[0].peer_name, "%s", "ALFA");
    snprintf(messages.entries[0].text, sizeof messages.entries[0].text, "%s", "hello all");
    mesh_ui_store_set_messages(&store, &messages);

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
    const size_t page = (size_t)height * stride;
    uint8_t *early = pixels != NULL ? malloc(page) : NULL;
    uint8_t *later = pixels != NULL ? malloc(page) : NULL;
    MESH_TEST_FAIL_IF_CLEANUP(pixels == NULL || early == NULL || later == NULL, free(early);
                              free(later); mesh_ui_capture_close(capture);
                              mesh_time_wall_set_fixed(0U);
                              mesh_ui_store_shutdown(&store), "no page to compare");

    /* Ten minutes after the message, then two hours after it: "10m" against "2h". */
    mesh_time_wall_set_fixed(base + 600U);
    mesh_ui_capture_render(capture, &snapshot);
    memcpy(early, pixels, page);

    mesh_time_wall_set_fixed(base + 7200U);
    mesh_ui_capture_render(capture, &snapshot);
    memcpy(later, pixels, page);

    MESH_TEST_FAIL_IF_CLEANUP(memcmp(early, later, page) == 0, free(early); free(later);
                              mesh_ui_capture_close(capture); mesh_time_wall_set_fixed(0U);
                              mesh_ui_store_shutdown(&store), "the frame ignored the pinned clock");

    /* And back: the same pin has to draw the same bytes, or a checked-in screenshot still
       churns however carefully the scene pins its clock. */
    mesh_time_wall_set_fixed(base + 600U);
    mesh_ui_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(memcmp(early, pixels, page) != 0, free(early); free(later);
                              mesh_ui_capture_close(capture); mesh_time_wall_set_fixed(0U);
                              mesh_ui_store_shutdown(&store),
                              "the same pinned clock drew a different frame");

    free(early);
    free(later);
    mesh_ui_capture_close(capture);
    mesh_time_wall_set_fixed(0U);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

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

                (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
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

            (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
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

/*
 * A bubble draws inside itself, whatever it is asked to carry.
 *
 * This is the failure the trailing run was made typed for. The clock, the delivery state, the
 * padlock and the reaction chips used to be concatenated into one string by the screen and
 * right-aligned inside the bubble by the renderer; when that string came out wider than the
 * bubble could ever be - a failure reason is a sentence, and "no public key for that node" is
 * twenty-seven cells against a bubble that holds twenty-five at the largest scale - the measure
 * clamped the *box* to its maximum and the draw placed the *string* by its own width, and the
 * difference came out of the left edge. A whole line of a message painted on bare background,
 * outside the bubble it belonged to.
 *
 * So the assertion is containment rather than a layout: find the bubble by its own fill, and
 * require every drawn pixel on the rows it covers to be between that fill's edges. It is
 * checked at every scale and in every theme, because the width at which the run stops fitting
 * is a function of both, and it was invisible on the one panel this ships on until it was not.
 */
MESH_TEST_CASE(ui_capture_bubble_contains_its_own_ink, unit) {
    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    const char *failure = NULL;
    if (snapshot == NULL) {
        record_failure(test_name, "snapshot allocation failed");
        return;
    }

    /* One conversation with one message in it, so the only bubble on screen is the one being
       measured and its separator is the only other thing drawn. */
    snapshot->nav.screen = MESH_UI_SCREEN_MESSAGES;
    snapshot->nav.thread_open = true;
    snapshot->nav.target_node = 0x8F21B005U;
    struct mesh_ui_message *message = &snapshot->messages.entries[0];
    message->packet_id = 1U;
    message->peer = snapshot->nav.target_node;
    message->rx_time = 1788000000U;
    message->direction = MESH_MESSAGE_OUTBOUND;
    message->pki_encrypted = true;
    snprintf(message->peer_name, sizeof message->peer_name, "BRVO");
    snprintf(message->text, sizeof message->text, "Meet at the creek");

    /* Four reactions on it as well, which is the other half of what made the run long: the
       chip run and the reason were both in the same string. */
    for (uint32_t i = 0; i < 4U; ++i) {
        struct mesh_ui_message *reaction = &snapshot->messages.entries[1U + i];
        reaction->packet_id = 2U + i;
        reaction->peer = snapshot->nav.target_node;
        reaction->rx_time = message->rx_time;
        reaction->is_reaction = true;
        reaction->reply_id = message->packet_id;
        snprintf(reaction->text, sizeof reaction->text, "%s",
                 (const char *[]){"\U0001F44D", "\U0001F602", "\U0001F389", "❤"}[i]);
    }
    snapshot->messages.count = 5U;

    /* Every delivery state, and under the cursor as well as at rest - the selected fill is a
       different colour and the accent bar is laid outside it. */
    static const uint8_t acks[] = {MESH_MESSAGE_ACK_NONE, MESH_MESSAGE_ACK_PENDING,
                                   MESH_MESSAGE_ACK_DELIVERED, MESH_MESSAGE_ACK_FAILED};
    /* The longest reason the catalog carries, which is what a bubble has least room for. */
    static const uint8_t kPkiUnknownPubkey = 35U;

    struct mesh_ui_capture *capture = NULL;
    for (size_t t = 0; t < mesh_ui_theme_count() && failure == NULL; ++t) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(t);
        for (int scale = MESH_UI_SCALE_MIN; scale <= MESH_UI_SCALE_MAX && failure == NULL;
             ++scale) {
            for (size_t a = 0; a < sizeof acks / sizeof acks[0] && failure == NULL; ++a) {
                /* Cursor 0 is on the only bubble there is; anything past it is the same
                   transcript at rest. Both, because the cursor changes the fill and lays an
                   accent bar outside it. */
                for (uint32_t cursor = 0U; cursor < 2U && failure == NULL; ++cursor) {
                    const bool selected = (cursor == 0U);
                    message->ack = acks[a];
                    message->ack_error =
                        acks[a] == MESH_MESSAGE_ACK_FAILED ? kPkiUnknownPubkey : 0U;
                    snapshot->nav.cursor[MESH_UI_SCREEN_MESSAGES] = cursor;

                    if (mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH,
                                             MESH_UI_CAPTURE_HEIGHT, scale) != 0) {
                        failure = "capture open failed";
                        break;
                    }
                    mesh_ui_capture_set_theme(capture, theme);
                    mesh_ui_capture_set_scale(capture, scale);
                    uint32_t width = 0U, height = 0U;
                    size_t stride = 0U;
                    const uint8_t *pixels =
                        mesh_ui_capture_pixels(capture, &width, &height, &stride);
                    mesh_ui_capture_render(capture, snapshot);

                    /* The bubble's own fill, asked of the theme the same way the renderer asks:
                       ours in the secondary container, or the error container once it failed,
                       with the cursor's state layer over whichever it is. */
                    const struct mesh_ui_paint paint = mesh_ui_theme_paint(
                        theme,
                        acks[a] == MESH_MESSAGE_ACK_FAILED ? MESH_UI_FAMILY_ERROR
                                                           : MESH_UI_FAMILY_SECONDARY,
                        MESH_UI_SLOT_CONTAINER,
                        selected ? MESH_UI_STATE_SELECTED : MESH_UI_STATE_REST);
                    const uint32_t fill = rgb_key(paint.fill);
                    const uint32_t bg = rgb_key(mesh_ui_theme_color(theme, MESH_UI_COLOR_BG));

                    size_t escaped = 0U;
                    uint32_t rows = 0U;
                    for (uint32_t y = 0; y < height; ++y) {
                        const uint8_t *row = pixels + (size_t)y * stride;
                        uint32_t left = width, right = 0U;
                        for (uint32_t x = 0; x < width; ++x) {
                            if (pixel_key(row + (size_t)x * 4U) != fill) {
                                continue;
                            }
                            if (x < left) {
                                left = x;
                            }
                            right = x;
                        }
                        /* A row the bubble does not cover, or covers only in a corner's
                           stepping - neither says anything about containment. */
                        if (left >= right || right - left < (uint32_t)(4 * scale)) {
                            continue;
                        }
                        rows += 1U;
                        /* The cursor's accent is laid under the fill and shows on the outer
                           edge only, by exactly one scale - so that much either side is the
                           bubble too, not something that escaped it. */
                        const uint32_t bar = selected ? (uint32_t)scale : 0U;
                        left = left > bar ? left - bar : 0U;
                        right += bar;
                        for (uint32_t x = 0; x < width; ++x) {
                            if (x >= left && x <= right) {
                                continue;
                            }
                            if (pixel_key(row + (size_t)x * 4U) != bg) {
                                ++escaped;
                            }
                        }
                    }
                    mesh_ui_capture_close(capture);
                    capture = NULL;

                    if (rows == 0U) {
                        failure = "the transcript drew no bubble to measure";
                    } else if (escaped > 0U) {
                        failure = "a bubble drew part of itself outside its own fill";
                    }
                }
            }
        }
    }

    if (capture != NULL) {
        mesh_ui_capture_close(capture);
    }
    free(snapshot);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
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
        /* A move between tabs, which since step 13 is also a screen transition: the body is
           redrawn at an offset for the length of one, off a snapshot that does not change while
           it runs. Whatever that declares as damage has to be enough for the clipped
           composition below to still match the full one. */
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

/*
 * The screen progress bar under the partial-composition clip.
 *
 * fb_animation_clip_matches_full_composition covers the two animated things a body can hold; the
 * bar is the first one that lives in the *chrome*, above `body_y`, and it runs on a snapshot that
 * is not changing - which is precisely the case the clip is entered on. It declares its damage
 * through fb_draw_meter(), because it is one, so this is the check that the reuse is enough:
 * clipped and unclipped composition have to agree on every frame of the loop.
 */
MESH_TEST_CASE(fb_progress_clip_matches_full_composition, unit) {
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

    /* A radio attached with an admin read outstanding: mesh_ui_chrome_busy() is true and nothing
       else about the frame moves, so every frame after the first is a candidate for the clip. */
    snapshot->nav.screen = MESH_UI_SCREEN_NODES;
    snapshot->device_count = 1U;
    snapshot->devices[0].connected = true;
    snapshot->handshake_valid = true;
    snapshot->handshake.config_complete = true;
    snapshot->settings.admin_busy = true;

    for (unsigned frame = 0U; frame < 40U; ++frame) {
        for (unsigned i = 0U; i < 2U; ++i) {
            fb_state_set_now(&state[i], 1000U + frame * 16U);
            fb_render_snapshot(&state[i], snapshot);
        }
        if (state[0].clip_active) {
            clipped++;
        }
        if (memcmp(state[0].fb_ptr, state[1].fb_ptr, state[0].fb_size) != 0) {
            failure = "the clipped frame lost the progress bar, or what it travelled over";
            goto cleanup;
        }
    }
    if (clipped == 0U) {
        failure = "the bar declared no damage, so the comparison never exercised the clip";
    }
cleanup:
    for (unsigned i = 0U; i < 2U; ++i) {
        fb_glyph_cache_free(&state[i]);
        fb_thread_cache_free(&state[i]);
        fb_render_cache_free(&state[i]);
        free(state[i].fb_ptr);
    }
    free(snapshot);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * A slider says where the value is, and nothing under it moves when it says something else.
 *
 * Two frames of Settings > Display with the screen timeout at the bottom of its scale and then
 * at the top. Two things have to hold and the second is the one this component could get wrong
 * in a way nothing on a single frame would reveal.
 *
 * The control must change, because a track that draws the same for the shortest timeout this
 * field offers and the longest is a decoration rather than a reading. And every row *below* it
 * must be pixel-identical, which is what says the row's second step was reserved from the field
 * rather than from the value: a height that depended on what the radio last reported would
 * reflow the whole section under the cursor every time somebody pressed Right, and the reflow
 * would be invisible in any screenshot taken one value at a time.
 *
 * The row's own bar takes the width the words had rather than the width left after them - that
 * is what a second step is *for* - so unlike the segmented button there is no label column to
 * hold still here. What holds still is everything the row is not.
 *
 * Across every theme and every scale, because both are measurements and the narrow end of the
 * scale range is where a measurement stops fitting.
 */
MESH_TEST_CASE(ui_capture_slider_places_the_value, unit) {
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
                settings.has_display = true;
                /* The two ends of the scale this field states: 15 seconds and an hour. Its 0 is
                   "the firmware decides" and stands outside the track, so neither end is it. */
                settings.screen_on_secs = pass == 0U ? 15U : 3600U;
                mesh_ui_store_set_settings(&store, &settings);

                (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
                if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_DISPLAY)) {
                    failure = "could not open Settings > Display";
                } else {
                    frames[pass] = capture_frame(&store, theme, scale, &width, &height, &stride);
                    if (frames[pass] == NULL) {
                        failure = "capture failed";
                    }
                }
                mesh_ui_store_shutdown(&store);
            }

            if (failure == NULL) {
                /* Screen on is the section's first row, so everything further down is rows this
                   value has nothing to do with. */
                const uint32_t body_top = height / 8U;
                const uint32_t body_bottom = height - height / 8U;
                /* A third of the way down clears the row itself at every scale that ships and
                   still leaves four of this section's rows below it - which is what has to be
                   identical, and what would not be if the row's height moved. */
                const uint32_t below = body_top + (body_bottom - body_top) / 3U;
                const size_t row_changed =
                    differing_in(frames[0], frames[1], stride, 0U, width - 1U, body_top, below);
                const size_t under_changed =
                    differing_in(frames[0], frames[1], stride, 0U, width - 1U, below, body_bottom);
                if (row_changed == 0U) {
                    failure = "the screen timeout draws identically at both ends of its scale - "
                              "the track is reporting nothing";
                } else if (under_changed != 0U) {
                    failure = "changing a value moved the rows below it, so a slider row's height "
                              "is coming from the value rather than from the field";
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
 * A value that is a word rather than a quantity draws no handle at all - and still costs the row
 * the same step.
 *
 * "Default" and LoRa's "max" are not the bottom of their scales, and the whole of the first bug
 * this pins is that they render identically to the bottom if the control clamps them into range.
 * So the two frames below - the firmware's own default, and the shortest timeout this field
 * actually offers - have to *differ*. Under the clamp they are the same frame, which is §2.11's
 * rule caught in the act: a picture cannot be wrong quietly.
 *
 * And this is the pair that pins the other half of it. These are the only two values in the
 * client whose rows a value-dependent height would draw at different heights - one places and
 * one does not - so if the second step were reserved from the value rather than from the field,
 * everything below this row would sit one step further up in one of these frames. Pressing Right
 * off "default" would then reflow the section under the cursor, which no screenshot taken one
 * value at a time can show.
 */
MESH_TEST_CASE(ui_capture_slider_refuses_a_word, unit) {
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
            settings.has_display = true;
            settings.screen_on_secs = pass == 0U ? 0U : 15U;
            mesh_ui_store_set_settings(&store, &settings);

            (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
            if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_DISPLAY)) {
                failure = "could not open Settings > Display";
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
            const uint32_t below = body_top + (body_bottom - body_top) / 3U;
            /*
             * The *label column*, which is the one part of this row where the two frames can only
             * differ because of the control.
             *
             * The words to the right of it differ whatever happens - one row says "default" and
             * the other says "15s" - so a comparison across the whole width is satisfied by the
             * value column alone and never looks at the track at all. Inside the label column the
             * words are identical, and what is left is the left end of the bar: a handle sitting
             * at the bottom of the scale under the clamp, and nothing at all when the value is
             * one the track cannot place.
             */
            const uint32_t label_right = settings_label_right(theme, width, 4);
            const size_t row_changed =
                differing_in(frames[0], frames[1], stride, theme->metrics.margin,
                             label_right < width ? label_right : width, body_top, below);
            const size_t under_changed =
                differing_in(frames[0], frames[1], stride, 0U, width - 1U, below, body_bottom);
            if (row_changed == 0U) {
                failure = "a screen timeout the firmware picks puts a handle exactly where the "
                          "shortest one this client offers does - the track is claiming a value "
                          "nobody reported";
            } else if (under_changed != 0U) {
                failure = "a value the track cannot place changed the height of its row, so the "
                          "rows below it move when somebody steps off \"default\"";
            }
        }
        free(frames[0]);
        free(frames[1]);
    }

    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The most runs of `gap` any single scanline has strictly *inside* runs of `ink`: marks cut out
 * of a filled bar, which is what a slider's stops and a meter's band boundaries both are.
 *
 * Per row rather than summed, because the number on one row is what carries the claim. A filled
 * slider's own handle is the same ink as its fill with a gap before it, so every correct frame
 * and every broken one has at least one - and only a frame whose stops survived has several.
 *
 * No geometry, deliberately. Where the bar is depends on the theme's metrics, the glyph scale
 * and how many rows the app bar took, and a test that worked that out would be a second opinion
 * about the layout rather than a reading of the frame.
 */
static size_t gaps_inside_fill(const uint8_t *frame, size_t stride, uint32_t width, uint32_t y0,
                               uint32_t y1, uint32_t ink, uint32_t gap) {
    size_t most = 0U;
    for (uint32_t y = y0; y <= y1; ++y) {
        const uint8_t *row = frame + (size_t)y * stride;
        bool seen_ink = false;
        uint32_t run = 0U;
        size_t gaps = 0U;
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t key = pixel_key(row + (size_t)x * 4U);
            if (key == ink) {
                /* A gap only counts once it is closed by more of the fill, which is what
                   distinguishes a notch from the end of the bar. */
                if (seen_ink && run > 0U) {
                    ++gaps;
                }
                seen_ink = true;
                run = 0U;
            } else if (key == gap && seen_ink) {
                ++run;
            } else {
                seen_ink = false;
                run = 0U;
            }
        }
        if (gaps > most) {
            most = gaps;
        }
    }
    return most;
}

/*
 * A track that offers choices still shows them once the fill has passed them.
 *
 * The stops are notches cut out of the track in the ground colour, which is what lets them read
 * the same over the filled half and the empty one and need no ink of their own - and the whole
 * of that arrangement depends on the notches being drawn *after* both halves are down. Drawn
 * before the fill they are gaps the fill closes: the stops behind the handle vanish one by one
 * as the value climbs, and at the top of the scale a field offering ten choices shows none of
 * them. That is the state this renders - screen-on at its longest, so the fill is the whole
 * track - and the frame has to carry marks inside it.
 */
MESH_TEST_CASE(ui_capture_slider_stops_survive_the_fill, unit) {
    const char *failure = NULL;

    for (size_t t = 0; t < mesh_ui_theme_count() && failure == NULL; ++t) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(t);
        struct mesh_ui_store store;
        if (mesh_ui_store_init(&store) != 0) {
            failure = "store init failed";
            break;
        }
        mesh_test_nav_populate(&store);
        struct mesh_ui_settings settings = store.settings;
        settings.loaded = true;
        settings.has_display = true;
        settings.screen_on_secs = 3600U; /* the top of this field's scale */
        mesh_ui_store_set_settings(&store, &settings);

        (void)mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS);
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        uint8_t *frame = NULL;
        if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_DISPLAY)) {
            failure = "could not open Settings > Display";
        } else {
            frame = capture_frame(&store, theme, 4, &width, &height, &stride);
            if (frame == NULL) {
                failure = "capture failed";
            }
        }
        if (failure == NULL) {
            const uint32_t ink = rgb_key(mesh_ui_theme_tone(theme, MESH_UI_TONE_PRIMARY));
            const uint32_t ground = rgb_key(mesh_ui_theme_color(theme, MESH_UI_COLOR_BG));
            const uint32_t body_top = height / 8U;
            const uint32_t body_bottom = height - height / 8U;
            /* Screen-on offers nine stops, so a filled track carries seven interior marks plus
               the handle's own gap. More than the handle alone is the whole of the claim. */
            if (gaps_inside_fill(frame, stride, width, body_top, body_bottom, ink, ground) < 3U) {
                failure = "a full slider draws as one unbroken bar - the stops it offers are "
                          "being painted over by the fill instead of cut out of it";
            }
        }
        free(frame);
        mesh_ui_store_shutdown(&store);
    }

    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The map never puts ink on the chrome around it.
 *
 * The map is the one screen whose content is placed at coordinates rather than laid out in rows,
 * so the containment a list gets for free has to be arranged here: the nav's viewport and this
 * backend's body box are the same box, resized on every frame, and `visible` is what gates the
 * draw. If those drift apart a marker lands on the tab strip or through the keycaps, which reads
 * as a rendering fault rather than as a bug with a cause - and it is invisible in a screenshot
 * of any view that happens not to have a marker near an edge.
 *
 * So the marker under test is swept north and south, from the middle of the body to well past
 * both of its edges, and the top and bottom bands of the panel - which are chrome at this glyph
 * scale, and nothing else - are compared against the same frame drawn with the marker having no
 * position at all. A waypoint counts towards the app bar's "of" whether or not it has
 * coordinates, so those bands are identical by construction and any difference in them is ink
 * that escaped the map.
 *
 * Vertically rather than horizontally, because that is where the room is: the body is inset from
 * the panel by a margin at the sides and by the whole of two bars top and bottom, so a marker a
 * little past the left edge is clipped by the framebuffer itself while one a little past the top
 * lands squarely on the tab strip.
 *
 * One glyph scale, and it is the device's own. The bands below have to be chrome and not body,
 * which is a fact about how tall the bars are drawn - so the test states the scale it is true at
 * rather than sweeping scales and quietly widening the bands until it passes at all of them.
 */
MESH_TEST_CASE(ui_capture_map_keeps_its_ink_off_the_chrome, unit) {
    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    const char *failure = NULL;
    if (snapshot == NULL) {
        record_failure(test_name, "snapshot allocation failed");
        return;
    }

    /* One node at the centre, so the frame is a map with something on it, and one place whose
       position is what the sweep moves. */
    snapshot->nav.screen = MESH_UI_SCREEN_NODES;
    snapshot->nav.map_open = true;
    snapshot->handshake_valid = true;
    snapshot->handshake.has_my_info = true;
    snapshot->handshake.my_info.node_num = 0x1000U;
    snapshot->handshake.node_count = 1U;
    snapshot->handshake.nodes[0].node_id = 0x1000U;
    snapshot->handshake.nodes[0].in_nodedb = true;
    snprintf(snapshot->handshake.nodes[0].short_name,
             sizeof snapshot->handshake.nodes[0].short_name, "ME");
    snapshot->handshake.nodes[0].position.valid = true;
    snapshot->handshake.nodes[0].position.latitude_i = 476180000;
    snapshot->handshake.nodes[0].position.longitude_i = -1223320000;

    /*
     * A second node is the marker under test, and it carries a rounded position on purpose.
     *
     * A node rather than a waypoint because a waypoint has no `precision_bits` - a place is a
     * point somebody chose, not one a receiver solved - and the footprint that rounding draws is
     * the biggest thing the map places: a bare marker is a disc a few pixels across, so the band
     * of positions where it straddles an edge is a handful of rows and a sweep can stride over
     * it. 19 bits is about 45 metres, which at this zoom is a disc wide enough that any step of
     * the sweep landing near an edge spills visibly over it.
     */
    snapshot->handshake.node_count = 2U;
    snapshot->handshake.nodes[1].node_id = 0x2000U;
    snapshot->handshake.nodes[1].in_nodedb = true;
    snprintf(snapshot->handshake.nodes[1].short_name,
             sizeof snapshot->handshake.nodes[1].short_name, "ALFA");
    snapshot->handshake.nodes[1].position.longitude_i = -1223320000;
    snapshot->handshake.nodes[1].position.precision_bits = 19U;

    const int scale = 4; /* what the Brick draws at, and what the bands below are true for */
    const uint8_t zoom = 16U;
    mesh_map_viewport_init(&snapshot->nav.map_viewport, 476180000, -1223320000, zoom);

    /*
     * The chrome above and below the map: the tab strip and the app bar at the top, the keycaps
     * and the status line at the bottom. Both bands stop short of where the body actually starts
     * at this scale, so a legitimately drawn marker never reaches them.
     *
     * The top band is the one that matters. An early version of this test used a band of a couple
     * of dozen pixels at the very edge of the panel, which is chrome at any scale and therefore
     * safe - and useless: the spill this is about is a marker whose *centre* is inside the body
     * near the top edge and whose footprint or label reaches back over the app bar, which is
     * nowhere near the panel's edge. A band that does not include the app bar cannot see it.
     */
    const uint32_t top_band = 96U;
    const uint32_t bottom_band = 56U;

    /*
     * And only the middle of each band, because two things in the chrome legitimately change
     * between the two passes: the app bar's badge counts the markers on the panel, and it sits
     * against the right edge, while the title sits against the left. The marker is swept down the
     * centre line, so anything that escapes the map lands between them - which is exactly the
     * column range compared here.
     */
    const uint32_t column_from = MESH_UI_CAPTURE_WIDTH / 3U;
    const uint32_t column_to = (MESH_UI_CAPTURE_WIDTH * 2U) / 3U;

    struct mesh_ui_capture *capture = NULL;
    uint8_t *reference = NULL;
    /*
     * About a dozen pixels a step at this zoom and this latitude - a degree of latitude is 111 km
     * and a pixel is a metre and a half - so fifty steps either way walks the marker from the
     * middle of the body out past the top and the bottom of the panel.
     *
     * The step size is the part that has to be right, and two earlier versions of this test got
     * it wrong while passing against deliberately broken code. A marker only straddles an edge
     * over a narrow band of positions, so a step larger than the drawn shape jumps straight over
     * the case the test exists for. Smaller than the footprint above, therefore, and not
     * "somewhere around the right order of magnitude".
     */
    for (int step = -50; step <= 50 && failure == NULL; ++step) {
        snapshot->handshake.nodes[1].position.latitude_i = 476180000 + step * 1700;

        for (int pass = 0; pass < 2 && failure == NULL; ++pass) {
            /* With a position and then without one. A node counts towards the app bar's "of"
               either way, so the chrome does not move for that reason. */
            snapshot->handshake.nodes[1].position.valid = (pass == 1);

            if (mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT,
                                     scale) != 0) {
                failure = "capture open failed";
                break;
            }
            mesh_ui_capture_set_theme(capture, mesh_ui_theme_at(0));
            mesh_ui_capture_set_scale(capture, scale);
            uint32_t width = 0U, height = 0U;
            size_t stride = 0U;
            const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
            mesh_ui_capture_render(capture, snapshot);

            if (pass == 0) {
                free(reference);
                reference = malloc((size_t)height * stride);
                if (reference == NULL) {
                    failure = "frame allocation failed";
                } else {
                    memcpy(reference, pixels, (size_t)height * stride);
                }
            } else if (height > bottom_band) {
                for (uint32_t y = 0; y < height && failure == NULL; ++y) {
                    if (y >= top_band && y < height - bottom_band) {
                        continue; /* the body, where a marker is entitled to be */
                    }
                    const size_t offset = (size_t)y * stride + (size_t)column_from * 4U;
                    const size_t span = (size_t)(column_to - column_from) * 4U;
                    if (memcmp(reference + offset, pixels + offset, span) != 0) {
                        failure = "a marker put ink on the chrome around the map";
                    }
                }
            }
            mesh_ui_capture_close(capture);
            capture = NULL;
        }
    }

    if (capture != NULL) {
        mesh_ui_capture_close(capture);
    }
    free(reference);
    free(snapshot);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * How many pixels of a frame are one of a fixture pack's tile colours.
 *
 * A fixture tile is one flat colour and no theme draws it, so this counts the basemap and
 * nothing else - which is what lets a case say "the first frame drew one tile" without knowing
 * where the tile grid happened to land in the body.
 */
static size_t count_pack_tiles(const uint8_t *pixels, uint32_t width, uint32_t height,
                               size_t stride, size_t tiles, unsigned shade) {
    size_t found = 0U;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0U; x < width; ++x) {
            const uint8_t *pixel = row + (size_t)x * 4U;
            for (size_t tile = 0U; tile < tiles; ++tile) {
                uint8_t rgb[3];
                mesh_test_map_tile_colour(tile, shade, rgb);
                /* The capture's page is B, G, R, X - see pixel_is_background(). */
                if (pixel[0] == rgb[2] && pixel[1] == rgb[1] && pixel[2] == rgb[0]) {
                    ++found;
                    break;
                }
            }
        }
    }
    return found;
}

static size_t count_fixture_tiles(const uint8_t *pixels, uint32_t width, uint32_t height,
                                  size_t stride, size_t tiles) {
    return count_pack_tiles(pixels, width, height, stride, tiles, 0U);
}

/*
 * The basemap: the tiles a pack holds, drawn under the markers and inside the map's own body.
 *
 * Three things are checked here and each one is a bug that has a way of being invisible.
 *
 * **The tiles reach the panel at all**, which is the fill loop, the cache and the decoder end to
 * end - a fixture pack of solid colours, so a frame can be counted for pixels no theme and no
 * marker draws.
 *
 * **They stay inside the map's body.** A blit is by a long way the widest thing this screen
 * places - a tile is 256 pixels square where a marker is a dozen - so a blit that clipped for
 * itself instead of going through the one function every fill in this backend goes through
 * would paint over the app bar, and would do it on any view whose tile grid happens not to line
 * up with the body. Checked the way the marker case above is: the chrome either side of the map
 * is compared against the same frame drawn with no pack at all, and the two must be identical.
 *
 * **One tile arrives per frame.** That is the property the whole design rests on - a cold tile
 * is 2-5 ms on the Brick's card and a view stands on about twenty of them, so a frame that drew
 * them all would be a tenth of a second in which nothing else is serviced. A first frame that
 * covered the body would pass a test that only looked for tiles, which is why the count after
 * one frame is compared against the count once it has settled.
 */
MESH_TEST_CASE(ui_capture_map_draws_a_basemap_one_tile_at_a_time, unit) {
    /* Where the demo roster stands, at a zoom whose tiles are a few hundred metres across. */
    const int32_t latitude_i = 476180000;
    const int32_t longitude_i = -1223320000;
    const uint8_t zoom = 15U;
    const int scale = 4;

    struct mesh_map_tile_key keys[35];
    const size_t tiles = mesh_test_map_keys_around(latitude_i, longitude_i, zoom, 7, 5, keys,
                                                   sizeof keys / sizeof keys[0]);
    if (tiles == 0U) {
        record_failure(test_name, "the fixture covers no tiles");
        return;
    }
    struct mesh_test_map_pack pack;
    const int wrote = mesh_test_map_pack_write(&pack, keys, tiles, 0U, "Fixture", "No copyright");
    if (wrote < 0) {
        record_failure(test_name, "the fixture pack could not be written");
        return;
    }

    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    uint8_t *bare = NULL;
    const char *failure = NULL;
    if (snapshot == NULL) {
        mesh_test_map_pack_remove(&pack);
        record_failure(test_name, "snapshot allocation failed");
        return;
    }
    snapshot->nav.screen = MESH_UI_SCREEN_NODES;
    snapshot->nav.map_open = true;
    snapshot->handshake_valid = true;
    snapshot->handshake.has_my_info = true;
    snapshot->handshake.my_info.node_num = 0x1000U;
    snapshot->handshake.node_count = 1U;
    snapshot->handshake.nodes[0].node_id = 0x1000U;
    snapshot->handshake.nodes[0].in_nodedb = true;
    snprintf(snapshot->handshake.nodes[0].short_name,
             sizeof snapshot->handshake.nodes[0].short_name, "ME");
    snapshot->handshake.nodes[0].position.valid = true;
    snapshot->handshake.nodes[0].position.latitude_i = latitude_i;
    snapshot->handshake.nodes[0].position.longitude_i = longitude_i;
    mesh_map_viewport_init(&snapshot->nav.map_viewport, latitude_i, longitude_i, zoom);

    /* The bands the marker case established: chrome at this scale and nothing else. */
    const uint32_t top_band = 96U;
    const uint32_t bottom_band = 56U;

    struct mesh_ui_capture *capture = NULL;
    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;

    /* The same frame with no pack open, which is what the chrome is compared against. */
    if (mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, scale) != 0) {
        failure = "capture open failed";
    } else {
        mesh_ui_capture_set_theme(capture, mesh_ui_theme_at(0));
        mesh_ui_capture_set_scale(capture, scale);
        const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
        mesh_ui_capture_render(capture, snapshot);
        bare = malloc((size_t)height * stride);
        if (bare == NULL || pixels == NULL) {
            failure = "frame allocation failed";
        } else {
            memcpy(bare, pixels, (size_t)height * stride);
        }
        if (mesh_ui_capture_animating(capture)) {
            failure = "a map with no pack asks for another frame";
        }
        mesh_ui_capture_close(capture);
        capture = NULL;
    }

    size_t after_one = 0U;
    size_t settled = 0U;
    unsigned frames = 0U;
    if (failure == NULL &&
        mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, scale) != 0) {
        failure = "capture open failed";
    } else if (failure == NULL) {
        mesh_ui_capture_set_theme(capture, mesh_ui_theme_at(0));
        mesh_ui_capture_set_scale(capture, scale);
        if (mesh_ui_capture_open_map_pack(capture, pack.path) != 0) {
            failure = "the capture would not open the fixture pack";
        }
    }

    if (failure == NULL) {
        const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
        mesh_ui_capture_render(capture, snapshot);
        after_one = count_fixture_tiles(pixels, width, height, stride, tiles);

        /* Settled: frames until it stops asking for more, with a ceiling well above the tiles a
           view can stand on so a loop that never stopped is a failure rather than a hang. */
        for (frames = 1U; frames < 200U && mesh_ui_capture_animating(capture); ++frames) {
            mesh_ui_capture_advance(capture, 33U);
            mesh_ui_capture_render(capture, snapshot);
        }
        settled = count_fixture_tiles(pixels, width, height, stride, tiles);

        if (mesh_ui_capture_animating(capture)) {
            failure = "the map never stopped asking for another frame";
        } else if (settled == 0U) {
            failure = "no tile reached the panel";
        } else if (after_one == 0U) {
            failure = "the first frame drew no tile at all";
        } else if (after_one * 2U > settled) {
            failure = "the first frame drew more than its one tile";
        } else if (frames < 4U) {
            failure = "the view filled in fewer frames than it has tiles";
        }
    }

    if (failure == NULL && height > bottom_band) {
        const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
        for (uint32_t y = 0U; y < height && failure == NULL; ++y) {
            if (y >= top_band && y < height - bottom_band) {
                continue; /* the body, where a tile is entitled to be */
            }
            if (memcmp(bare + (size_t)y * stride, pixels + (size_t)y * stride, stride) != 0) {
                failure = "a tile put ink on the chrome around the map";
            }
        }
    }

    if (capture != NULL) {
        mesh_ui_capture_close(capture);
    }
    mesh_test_map_pack_remove(&pack);
    free(bare);
    free(snapshot);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * Walking off the map stops it asking for frames.
 *
 * The flag that keeps the repaint timer running while a view fills says what the *last frame*
 * wanted, and a reader who opens the map and leaves before it has filled leaves it set. Latched,
 * that is thirty frames a second of a screen with no map on it for as long as the client runs -
 * on a handheld, the battery, and invisible from the outside because every one of those frames
 * is correct. So it is cleared at the top of every frame and only a frame that draws the map
 * sets it again.
 */
MESH_TEST_CASE(ui_capture_map_stops_asking_once_it_is_left, unit) {
    const int32_t latitude_i = 476180000;
    const int32_t longitude_i = -1223320000;
    const uint8_t zoom = 15U;
    const int scale = 4;

    struct mesh_map_tile_key keys[35];
    const size_t tiles = mesh_test_map_keys_around(latitude_i, longitude_i, zoom, 7, 5, keys,
                                                   sizeof keys / sizeof keys[0]);
    struct mesh_test_map_pack pack;
    if (tiles == 0U ||
        mesh_test_map_pack_write(&pack, keys, tiles, 0U, "Fixture", "No copyright") < 0) {
        record_failure(test_name, "the fixture pack could not be written");
        return;
    }

    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    struct mesh_ui_capture *capture = NULL;
    const char *failure = NULL;
    if (snapshot == NULL) {
        failure = "snapshot allocation failed";
    } else {
        snapshot->nav.screen = MESH_UI_SCREEN_NODES;
        snapshot->nav.map_open = true;
        snapshot->handshake_valid = true;
        mesh_map_viewport_init(&snapshot->nav.map_viewport, latitude_i, longitude_i, zoom);
    }
    if (failure == NULL &&
        mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, scale) != 0) {
        failure = "capture open failed";
    }
    if (failure == NULL && mesh_ui_capture_open_map_pack(capture, pack.path) != 0) {
        failure = "the capture would not open the fixture pack";
    }
    if (failure == NULL) {
        mesh_ui_capture_render(capture, snapshot);
        if (!mesh_ui_capture_animating(capture)) {
            failure = "a view with tiles still to fetch asked for no further frame";
        }
    }
    if (failure == NULL) {
        /*
         * Off the map, with the fill unfinished.
         *
         * Settling rather than checking the very next frame, because leaving a screen is a
         * transition and a transition is entitled to its frames. What is being checked is that
         * the asking *ends*: the slide is a few hundred milliseconds and the tiles left behind
         * would be for ever.
         */
        snapshot->nav.map_open = false;
        snapshot->nav.screen = MESH_UI_SCREEN_SETTINGS;
        unsigned frames = 0U;
        for (; frames < 60U && mesh_ui_capture_animating(capture); ++frames) {
            mesh_ui_capture_advance(capture, 33U);
            mesh_ui_capture_render(capture, snapshot);
        }
        if (mesh_ui_capture_animating(capture)) {
            failure = "a screen with no map on it went on asking for frames";
        }
    }

    if (capture != NULL) {
        mesh_ui_capture_close(capture);
    }
    mesh_test_map_pack_remove(&pack);
    free(snapshot);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * Opening a second pack forgets the first one's pixels.
 *
 * A tile key is three numbers about the world and none about the file it came out of, so two
 * packs of the same place hold different pictures at the same key. A cache carried across the
 * swap draws the old pack's streets under the new pack's attribution - and every pixel of it is
 * a real tile in the right place, so there is nothing on the frame that looks wrong. That is why
 * it is checked here rather than left to the eye: two fixtures over the same keys in different
 * colours, and the frame after the swap must hold none of the first one's.
 */
MESH_TEST_CASE(ui_capture_map_forgets_the_pack_it_swapped_out, unit) {
    const int32_t latitude_i = 476180000;
    const int32_t longitude_i = -1223320000;
    const uint8_t zoom = 15U;
    const int scale = 4;

    struct mesh_map_tile_key keys[35];
    const size_t tiles = mesh_test_map_keys_around(latitude_i, longitude_i, zoom, 7, 5, keys,
                                                   sizeof keys / sizeof keys[0]);
    struct mesh_test_map_pack first;
    struct mesh_test_map_pack second;
    if (tiles == 0U ||
        mesh_test_map_pack_write(&first, keys, tiles, 0U, "One", "No copyright") < 0) {
        record_failure(test_name, "the first fixture pack could not be written");
        return;
    }
    if (mesh_test_map_pack_write(&second, keys, tiles, 7U, "Two", "No copyright") < 0) {
        mesh_test_map_pack_remove(&first);
        record_failure(test_name, "the second fixture pack could not be written");
        return;
    }

    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    const char *failure = NULL;
    struct mesh_ui_capture *capture = NULL;
    if (snapshot == NULL) {
        failure = "snapshot allocation failed";
    } else {
        snapshot->nav.screen = MESH_UI_SCREEN_NODES;
        snapshot->nav.map_open = true;
        snapshot->handshake_valid = true;
        mesh_map_viewport_init(&snapshot->nav.map_viewport, latitude_i, longitude_i, zoom);
    }

    if (failure == NULL &&
        mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, scale) != 0) {
        failure = "capture open failed";
    }
    if (failure == NULL) {
        mesh_ui_capture_set_theme(capture, mesh_ui_theme_at(0));
        mesh_ui_capture_set_scale(capture, scale);
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);

        if (mesh_ui_capture_open_map_pack(capture, first.path) != 0) {
            failure = "the capture would not open the first pack";
        } else {
            for (unsigned frame = 0U; frame < 200U; ++frame) {
                mesh_ui_capture_render(capture, snapshot);
                if (!mesh_ui_capture_animating(capture)) {
                    break;
                }
                mesh_ui_capture_advance(capture, 33U);
            }
            if (count_pack_tiles(pixels, width, height, stride, tiles, 0U) == 0U) {
                failure = "the first pack drew nothing to forget";
            }
        }

        if (failure == NULL && mesh_ui_capture_open_map_pack(capture, second.path) != 0) {
            failure = "the capture would not open the second pack";
        }
        if (failure == NULL) {
            for (unsigned frame = 0U; frame < 200U; ++frame) {
                mesh_ui_capture_render(capture, snapshot);
                if (!mesh_ui_capture_animating(capture)) {
                    break;
                }
                mesh_ui_capture_advance(capture, 33U);
            }
            if (count_pack_tiles(pixels, width, height, stride, tiles, 7U) == 0U) {
                failure = "the second pack did not reach the panel";
            } else if (count_pack_tiles(pixels, width, height, stride, tiles, 0U) != 0U) {
                failure = "the first pack's tiles survived the swap";
            }
        }
    }

    if (capture != NULL) {
        mesh_ui_capture_close(capture);
    }
    mesh_test_map_pack_remove(&first);
    mesh_test_map_pack_remove(&second);
    free(snapshot);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * A chart's line stays inside the plot, at both ends of its domain.
 *
 * The arithmetic that places a reading is a subtraction from the bottom of the box - a height on
 * a panel whose origin is its top corner - and the way it goes wrong is at the ends: a reading at
 * the top of its domain drawn without room for its own stroke paints half of that stroke above
 * the plot, which on this screen is the app bar. It is the sparkline's `travel` one component
 * along, with a pen twice as thick, and a chart's pen is thick enough for the spill to be
 * several pixels rather than one.
 *
 * Checked the map's way: the same frame with the readings at the bottom of the domain and at the
 * top of it, compared outside the body. The chrome cannot move between the two - the title says
 * Airtime either way and the keycaps are the same three - so anything that differs there is ink
 * that escaped.
 */
MESH_TEST_CASE(ui_capture_chart_keeps_its_ink_off_the_chrome, unit) {
    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    const char *failure = NULL;
    if (snapshot == NULL) {
        record_failure(test_name, "snapshot allocation failed");
        return;
    }

    snapshot->nav.screen = MESH_UI_SCREEN_STATUS;
    snapshot->nav.trend_open = true;
    snapshot->handshake_valid = true;

    const int scale = 4; /* what the Brick draws at, and what the bands below are true for */
    const uint32_t top_band = 96U;
    const uint32_t bottom_band = 56U;

    struct mesh_ui_capture *capture = NULL;
    uint8_t *reference = NULL;
    size_t frame_bytes = 0U;
    bool body_moved = false;

    for (int pass = 0; pass < 2 && failure == NULL; ++pass) {
        /* Flat on the floor of the domain, then flat against its ceiling. Six readings a minute
           apart, so the line crosses the whole plot rather than sitting in a corner of it. */
        const int32_t level = pass == 0 ? 0 : 1000;
        mesh_ui_history_reset(&snapshot->history);
        for (uint32_t i = 0U; i < 6U; ++i) {
            mesh_ui_history_note_airtime(&snapshot->history, 60000U * (i + 1U), level, level);
        }

        if (mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, scale) !=
            0) {
            failure = "capture open failed";
            break;
        }
        mesh_ui_capture_set_theme(capture, mesh_ui_theme_at(0));
        mesh_ui_capture_set_scale(capture, scale);
        uint32_t width = 0U, height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
        mesh_ui_capture_render(capture, snapshot);

        if (pass == 0) {
            frame_bytes = (size_t)height * stride;
            free(reference);
            reference = malloc(frame_bytes);
            if (reference == NULL) {
                failure = "frame allocation failed";
            } else {
                memcpy(reference, pixels, frame_bytes);
            }
        } else if (height > top_band + bottom_band) {
            for (uint32_t y = 0; y < height && failure == NULL; ++y) {
                const size_t offset = (size_t)y * stride;
                if (y >= top_band && y < height - bottom_band) {
                    /* The body, where the line is entitled to be - and where it had better have
                       moved, or the comparison above is comparing two identical frames and would
                       pass against a chart that drew nothing at all. */
                    body_moved = body_moved || memcmp(reference + offset, pixels + offset,
                                                      (size_t)width * 4U) != 0;
                    continue;
                }
                if (memcmp(reference + offset, pixels + offset, (size_t)width * 4U) != 0) {
                    failure = "a reading at the top of the domain put ink on the chrome";
                }
            }
        }
        mesh_ui_capture_close(capture);
        capture = NULL;
    }

    if (failure == NULL && !body_moved) {
        failure = "the two readings drew the same picture, so nothing was tested";
    }

    if (capture != NULL) {
        mesh_ui_capture_close(capture);
    }
    free(reference);
    free(snapshot);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

/*
 * The navigation bar's own badge: the one thing on the frame that speaks for a screen the user
 * is not looking at.
 *
 * Asked as "is there a filled capsule up in the tab strip", because that is the whole claim -
 * before this, an arriving message was invisible from every tab but Messages, and the strip is
 * chrome that every screen draws. The band is the strip alone: muting also puts a bell on the
 * conversation rows, and those are body and would answer the wrong question.
 *
 * Both directions are checked, and the second is the one worth having: a badge that appears is
 * easy, and a badge that never goes away is the failure that makes the whole thing worthless.
 */
MESH_TEST_CASE(ui_capture_nav_bar_badges_unread_messages, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    /* Standing on another tab, which is the case the badge exists for. */
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    while (store.nav.screen != MESH_UI_SCREEN_NODES) {
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }

    struct mesh_ui_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, 4) != 0,
        mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);

    /* Wide enough that a glyph stroke cannot produce it - the capsule's top scanline runs its
       whole width - and narrow enough for a single figure at the smallest scale a theme picks. */
    const unsigned capsule = 20U;
    /* The tab strip and nothing below it. It is the first thing the frame draws and it is one
       chrome line tall, so an eighth of the panel is generous and still well clear of the body. */
    const uint32_t strip = height / 8U;

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    mesh_ui_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(topmost_row_run(capture, pixels, width, height, stride,
                                              MESH_UI_COLOR_PRIMARY, capsule) >= strip,
                              mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "unread messages should badge the Messages tab from another tab");

    /* Muting every conversation empties the total, and the badge goes with it - which is the
       press's whole promise, and the reason the total is what the strip reads. */
    (void)mesh_ui_store_set_conversation_mute(&store, (uint8_t)MESH_UI_CONVERSATION_CHANNEL,
                                              MESH_MESSAGE_BROADCAST_ADDR, 0U, true);
    (void)mesh_ui_store_set_conversation_mute(&store, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x3000U,
                                              0U, true);
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_nav_unread_total(&store) != 0U, mesh_ui_capture_close(capture);
        mesh_ui_store_shutdown(&store), "the fixture's two conversations should both now be muted");
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    mesh_ui_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(topmost_row_run(capture, pixels, width, height, stride,
                                              MESH_UI_COLOR_PRIMARY, capsule) < strip,
                              mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "a muted mesh should leave the tab strip unbadged");

    mesh_ui_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * The node detail is drawn as a column of cards, and the cursor does not eat their sides.
 *
 * Two assertions, and the second is the one worth having.
 *
 * The first is the Status screen's, one tab over: a card is a filled panel with a hairline
 * round it, and a screen of plain rows on the bare ground is neither - so this fails if the
 * grouping is lost and passes whatever the rows inside the cards come to say. Pinning pixels
 * would fail on every legitimate change to this screen, which is not what is being protected.
 *
 * The second is the failure the widening was for. A card's box and a row's cursor fill were the
 * same rectangle, both measured from the row gutter, so the highlight landed exactly on the
 * hairline and painted it out for the length of one row: the card appeared to lose its sides
 * wherever the cursor stood, and *only* there. That is invisible in a still of a resting screen
 * and invisible in a count of how much card fill is on the panel - it is one row of one card,
 * and the row it happens on is the one the reader is looking at. So the check is that on the
 * scanlines the cursor fill covers there is still an edge pixel outside it on both sides, which
 * is the whole of what "a card contains the widest thing standing in it" means.
 *
 * At every scale, because the gutter, the hairline and the corner radius all come off the glyph
 * scale and they do not all come off it at the same rate.
 */
/*
 * A group's heading is drawn after the cards, so anything of it that lands on a card's edge
 * paints through the edge - and the scale it happens at is the one nobody renders.
 *
 * At MESH_UI_SCALE_MIN the type scale clamps the label onto the body, because a label cannot be
 * rasterised below the registry's smallest size. A heading's cell is then exactly as tall as a
 * row's, the step holds one line gap of air, and an inset taken out of that leaves the cell a
 * pixel longer than the gap it is centred in - so its last row is the next card's top edge and
 * every heading with a descender in it draws through the hairline.
 *
 * What this asks is that a card's edge is only ever card: a scanline that is mostly outline
 * carries nothing but the card's own colours, the cursor's fill included - whether the highlight
 * may stand *on* an edge is the question cursor_stays_inside_its_card() asks, and answering it
 * twice in two ways is how the two come to disagree. The scroll rail is outside the margin and
 * outside this, for the reason it is outside that one.
 */
static const char *card_edges_carry_no_ink(const struct mesh_ui_capture *capture,
                                           const uint8_t *pixels, uint32_t width, uint32_t height,
                                           size_t stride, int scale) {
    /* The rail's gutter is half a margin, and the margin does not scale - see fb_list_rail(). */
    const uint32_t right = width > 16U ? width - 16U : width;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        uint32_t edge = 0U;
        for (uint32_t x = 0U; x < right; ++x) {
            if (pixel_is_role(capture, row + (size_t)x * 4U, MESH_UI_COLOR_OUTLINE)) {
                edge++;
            }
        }
        /* A card runs nearly the whole panel, so its edge rows are the only ones that can be
           mostly outline. Nothing else on the frame draws a rule in that role. */
        if (edge * 5U < right * 3U) {
            continue;
        }
        for (uint32_t x = 0U; x < right; ++x) {
            const uint8_t *px = row + (size_t)x * 4U;
            if (pixel_is_role(capture, px, MESH_UI_COLOR_OUTLINE) ||
                pixel_is_role(capture, px, MESH_UI_COLOR_SURFACE) ||
                pixel_is_role(capture, px, MESH_UI_COLOR_SURFACE_SEL) ||
                pixel_is_role(capture, px, MESH_UI_COLOR_BG)) {
                continue;
            }
            (void)scale;
            return "a group's heading drew through the edge of the card under it";
        }
    }
    return NULL;
}

/*
 * Whether the scroll rail is beside the cards rather than on them.
 *
 * This is the bug the rail's gutter exists to prevent, and it is one the two card cases above
 * cannot see: both stop scanning short of the rail's strip, precisely so that a control drawn
 * out there is not mistaken for ink on a card. So the question has to be asked from the other
 * end - of the space *between* the two.
 *
 * It went wrong by arithmetic rather than by anybody moving the rail. The rail was placed in the
 * half-margin left over beside a row's fill, which was right until a card started spending its
 * hairline outward into that same half-margin to keep the cursor's highlight off its sides. The
 * two then met exactly: the card's outline ended on one pixel and the rail's track began on the
 * next, so on the node detail - the one screen that is a column of cards - the rail read as part
 * of the card's edge rather than as a control standing beside it.
 *
 * Nothing about that is visible in a count of card fill, in a widest-run measurement, or in a
 * still at the scale a screenshot is looked at. What it is, is a missing gap, so that is what is
 * measured: on every scanline carrying card ink, whatever is drawn to the right of the card is
 * required to have clear ground between it and the card. The rail is not named - it is asked for
 * as "the next thing on this row", which is also what keeps this honest if anything else ever
 * moves into that strip.
 *
 * `saw_rail` is the other half, and it is an out-parameter rather than a failure here because a
 * list that fits its window draws no rail at all - which is the design, and which the node
 * detail really does do at the smallest glyph scale on a node that has not reported much. So
 * "something was drawn beside the cards" is reported up to the caller, which has the whole
 * ladder of scales to ask it of and can require that at least one of them scrolled.
 */
static const char *rail_clears_the_cards(const struct mesh_ui_capture *capture,
                                         const uint8_t *pixels, uint32_t width, uint32_t height,
                                         size_t stride, bool *saw_rail) {
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        /* The card's own outer edge: its hairline, or its fill on the ends the window cut. The
           cursor's own fill is inside both and so can never be the rightmost of the three. */
        int card_right = -1;
        for (uint32_t x = 0U; x < width; ++x) {
            const uint8_t *px = row + (size_t)x * 4U;
            if (pixel_is_role(capture, px, MESH_UI_COLOR_OUTLINE) ||
                pixel_is_role(capture, px, MESH_UI_COLOR_SURFACE)) {
                card_right = (int)x;
            }
        }
        /* A row with no card on it - a group's heading, the chrome above and below the body -
           has nothing for the rail to be too close to. */
        if (card_right < 0) {
            continue;
        }
        for (uint32_t x = (uint32_t)card_right + 1U; x < width; ++x) {
            if (pixel_is_background(capture, row + (size_t)x * 4U)) {
                continue;
            }
            /*
             * Two pixels of clear ground, not one.
             *
             * Flush is how it actually broke, but a single pixel between two filled shapes is
             * not a gap the eye reads as one - it reads as a seam in the card's own edge, which
             * is the same complaint. Two is also the floor rather than the figure: the shipped
             * layout leaves the rail centred in fb_rail_gutter()'s strip, which is several
             * pixels either side at every scale and every panel this is drawn at, so a frame
             * that comes back with two has already lost the gutter and kept only the rounding.
             */
            if (x < (uint32_t)card_right + 3U) {
                return "the scroll rail is drawn against the card beside it";
            }
            if (saw_rail != NULL) {
                *saw_rail = true;
            }
            break;
        }
    }
    return NULL;
}

/*
 * Whether the cursor's fill is standing inside a card's edge on every scanline it covers.
 *
 * Asked of the rows where the fill is at its widest, because the fill is a rounded shape and its
 * first and last rows are inset by the corner radius - an edge pixel beside those is the card's
 * corner rather than its side, which is a different question and a weaker one.
 *
 * The *contiguous* run, and its own two ends, because the cursor fill is not the only thing on a
 * scanline wearing that colour: the scroll rail's track is the same ink, four pixels wide and out
 * in the gutter past everything the card contains. Taken as the first and last pixel of the
 * colour anywhere on the row, the rail moves the right-hand end outside the card and every
 * scrolling list fails a check about its cursor. Returns the failure, or NULL.
 */
static const char *cursor_stays_inside_its_card(const struct mesh_ui_capture *capture,
                                                const uint8_t *pixels, uint32_t width,
                                                uint32_t height, size_t stride) {
    uint32_t widest = 0U;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        uint32_t run = 0U;
        for (uint32_t x = 0U; x < width; ++x) {
            run = pixel_is_role(capture, row + (size_t)x * 4U, MESH_UI_COLOR_SURFACE_SEL) ? run + 1U
                                                                                          : 0U;
            if (run > widest) {
                widest = run;
            }
        }
    }
    if (widest == 0U) {
        return "no cursor fill on the node detail to check the card edge against";
    }
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        uint32_t run = 0U;
        uint32_t first = width;
        uint32_t last = width;
        for (uint32_t x = 0U; x < width; ++x) {
            if (pixel_is_role(capture, row + (size_t)x * 4U, MESH_UI_COLOR_SURFACE_SEL)) {
                run++;
                if (run == widest) {
                    first = x + 1U - run;
                    last = x;
                }
            } else {
                run = 0U;
            }
        }
        if (last == width) {
            continue;
        }
        bool left = false;
        bool right = false;
        for (uint32_t x = 0U; x < first; ++x) {
            left = left || pixel_is_role(capture, row + (size_t)x * 4U, MESH_UI_COLOR_OUTLINE);
        }
        for (uint32_t x = last + 1U; x < width; ++x) {
            right = right || pixel_is_role(capture, row + (size_t)x * 4U, MESH_UI_COLOR_OUTLINE);
        }
        if (!left || !right) {
            return "the cursor fill painted out the card's edge on its own row";
        }
    }
    return NULL;
}

MESH_TEST_CASE(ui_capture_node_detail_cards_survive_the_cursor, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    /* Walked rather than assigned, the Status case's rule: a test that set the nav by hand would
       keep passing while the presses that get a user here stopped working. */
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    while (store.nav.screen != MESH_UI_SCREEN_NODES) {
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* past the map row */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* a node that is not us */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.node_detail_open, mesh_ui_store_shutdown(&store),
                              "A should open the node detail");

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_store_shutdown(&store), "no snapshot to render");

    const char *failure = NULL;
    /* Whether any of the scales below put a rail beside the cards. Asked once over the whole
       ladder rather than per scale: bigger glyphs fit fewer rows, so a detail that fits its
       window at the smallest scale cannot at the largest, and a rail that stopped being drawn
       at all is what this catches. */
    bool rail_seen = false;
    for (int scale = MESH_UI_SCALE_MIN; scale <= MESH_UI_SCALE_MAX && failure == NULL; ++scale) {
        struct mesh_ui_capture *capture = NULL;
        if (mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, scale) !=
            0) {
            failure = "capture open failed";
            break;
        }
        mesh_ui_capture_set_scale(capture, scale);

        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
        mesh_ui_capture_render(capture, &snapshot);

        const unsigned fill =
            widest_row_run(capture, pixels, width, height, stride, MESH_UI_COLOR_SURFACE);
        const unsigned edge =
            widest_row_run(capture, pixels, width, height, stride, MESH_UI_COLOR_OUTLINE);
        if (fill < 80U) {
            failure = "the node detail draws no card fill across the body";
        } else if (edge < 80U) {
            failure = "the node detail draws no card edge across the body";
        }

        if (failure == NULL) {
            failure = cursor_stays_inside_its_card(capture, pixels, width, height, stride);
        }
        if (failure == NULL) {
            failure = card_edges_carry_no_ink(capture, pixels, width, height, stride, scale);
        }
        if (failure == NULL) {
            const char *broke =
                rail_clears_the_cards(capture, pixels, width, height, stride, &rail_seen);
            if (broke != NULL) {
                static char detail[160];
                snprintf(detail, sizeof detail, "%s (at glyph scale %d)", broke, scale);
                failure = detail;
            }
        }

        mesh_ui_capture_close(capture);
    }

    /*
     * And the same question of every row of a group rather than only of the one the screen opens
     * on, because the two rows that can get it wrong are the *first* and the *last* of a card.
     * Those are the rows the card's corners are curving through, and a card drawn to a rounder
     * shape than the highlight is still turning where the highlight has reached its full width -
     * so the cursor's ends stand outside the card, on precisely the rows a reader walks through
     * to get from one group to the next. The screen's first row is in the middle of its card and
     * saw none of it.
     *
     * Walked with the key rather than by setting the cursor, the same rule as above, and far
     * enough to cross several groups - the detail's first dozen rows are the actions, the
     * identity and the signal, which is three cards and both ends of two of them.
     */
    for (unsigned step = 0U; step < 16U && failure == NULL; ++step) {
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
        mesh_ui_store_request_refresh(&store);
        if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
            failure = "no snapshot after walking the node detail";
            break;
        }
        struct mesh_ui_capture *capture = NULL;
        if (mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, 0) != 0) {
            failure = "capture open failed";
            break;
        }
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
        mesh_ui_capture_render(capture, &snapshot);
        const char *broke = cursor_stays_inside_its_card(capture, pixels, width, height, stride);
        if (broke == NULL) {
            broke = rail_clears_the_cards(capture, pixels, width, height, stride, &rail_seen);
        }
        if (broke != NULL) {
            /* Which press it was, because "somewhere in the first sixteen rows" is the half of
               this failure that costs the time - the rows differ by what the node reported. */
            static char detail[160];
            snprintf(detail, sizeof detail, "%s (%u rows into the detail, cursor %u)", broke,
                     step + 1U, snapshot.nav.cursor[MESH_UI_SCREEN_NODES]);
            failure = detail;
        }
        mesh_ui_capture_close(capture);
    }

    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    MESH_TEST_FAIL_IF(!rail_seen, "no scroll rail beside the node detail at any glyph scale");
    record_success(test_name);
}

/*
 * Scanlines on which a label's ink stands entirely to the left of a value's.
 *
 * The question a two-tier fact row answers in pixels. A row that draws its label column and its
 * value in one ink puts one colour across the whole line, so no scanline has the quiet ink at
 * all; a row that draws them as two pieces puts TEXT_DIM in the label column and TEXT after it,
 * in that order, on every scanline the glyphs' cores reach.
 *
 * Ordered rather than merely both-present, because both-present is satisfied by a screen that
 * happens to carry a dim word somewhere to the right of an ordinary one - a trailing age beside
 * a name, which is most of the lists in this client. The label column is what is being pinned,
 * and what makes it the label column is that it comes first.
 *
 * Glyph cores only: a glyph carries coverage, so its edges are blends of the ink and the ground
 * and match no role exactly. That is the whole of why this counts scanlines rather than pixels -
 * a run of them is a row of text, and one is a stray antialiased hit.
 */
static unsigned two_tier_rows(const struct mesh_ui_capture *capture, const uint8_t *pixels,
                              uint32_t width, uint32_t height, size_t stride) {
    unsigned rows = 0U;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        bool dim = false;
        bool text = false;
        bool text_before_dim = false;
        for (uint32_t x = 0U; x < width; ++x) {
            const uint8_t *pixel = row + (size_t)x * 4U;
            if (pixel_is_role(capture, pixel, MESH_UI_COLOR_TEXT_DIM)) {
                dim = true;
                text_before_dim = text_before_dim || text;
            } else if (pixel_is_role(capture, pixel, MESH_UI_COLOR_TEXT)) {
                text = true;
            }
        }
        if (dim && text && !text_before_dim) {
            rows++;
        }
    }
    return rows;
}

/*
 * A node's facts are two tiers, not one.
 *
 * The whole of this screen is label-and-value rows, and they were composed into a single string
 * and drawn in a single ink - so the question and the answer were typographically identical and
 * a card of them read as a block of text with no way into it. That is the bubble's trailing run
 * one component over: pieces pasted together cannot take two inks, and the fix is the same one,
 * which is to draw them as pieces.
 *
 * Asked in the ink rather than in the layout, because the layout was never wrong: the columns
 * lined up before this and they line up now. What changed is that the label recedes to the quiet
 * tier and the value keeps the row's own, so a reader scanning a hundred and twenty rows for the
 * answers is not reading the questions as well.
 *
 * Across the scale range, because the label column is a cell count and the row it has to fit in
 * is a measurement - a fit that collapses at the largest glyphs would take the value column with
 * it, and this screen is the one that runs out of width first.
 */
MESH_TEST_CASE(ui_capture_node_detail_states_its_labels_quietly, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    /* Walked rather than assigned, the rest of this suite's rule: a test that set the nav by
       hand would keep passing while the presses that get a user here stopped working. */
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    while (store.nav.screen != MESH_UI_SCREEN_NODES) {
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_RIGHT, &action);
    }
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* past the map row */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action); /* a node that is not us */
    (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.node_detail_open, mesh_ui_store_shutdown(&store),
                              "A should open the node detail");
    /* Down past the verbs, which are plain rows and say nothing about a label column. Far
       enough that the window is showing facts whichever groups this node turns out to have. */
    for (unsigned step = 0U; step < 12U; ++step) {
        (void)mesh_ui_store_handle_key(&store, MESH_UI_KEY_DOWN, &action);
    }

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_store_shutdown(&store), "no snapshot to render");

    const char *failure = NULL;
    static char detail[160];
    for (int scale = MESH_UI_SCALE_MIN; scale <= MESH_UI_SCALE_MAX && failure == NULL; ++scale) {
        struct mesh_ui_capture *capture = NULL;
        if (mesh_ui_capture_open(&capture, MESH_UI_CAPTURE_WIDTH, MESH_UI_CAPTURE_HEIGHT, scale) !=
            0) {
            failure = "capture open failed";
            break;
        }
        mesh_ui_capture_set_scale(capture, scale);
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
        mesh_ui_capture_render(capture, &snapshot);

        /* More than one, so a single antialiased hit cannot pass for a row: the smallest glyph
           this ships draws its cores over several scanlines, and the screen is a column of
           these rows rather than one of them. */
        const unsigned rows = two_tier_rows(capture, pixels, width, height, stride);
        if (rows < 2U) {
            snprintf(detail, sizeof detail,
                     "the node detail draws its labels in the value's own ink (%u two-tier "
                     "scanlines at glyph scale %d)",
                     rows, scale);
            failure = detail;
        }
        mesh_ui_capture_close(capture);
    }

    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
