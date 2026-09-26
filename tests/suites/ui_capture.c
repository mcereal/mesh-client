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

#include "inkcell/ui/emoji.h"
#include "inkcell/ui/font.h"
#include "inkcell/ui/theme.h"
#include "inkcell/ui/widgets.h"
#include "inkwell/base/time.h"

#include "framework/mesh_test.h"
#include "support/map_fixture.h"
#include "support/ui_fixture.h"

#include "mesh/core/message.h"
#include "mesh/core/updater.h"
#include "mesh/i18n/strings.h"
#include "mesh/map/viewport.h"
#include "mesh/ui/backends/fb_capture.h"
#include "mesh/ui/focus.h"
#include "mesh/ui/history.h"
#include "mesh/ui/map.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/node_detail.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include "../../src/ui/backends/fb_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * A page for a renderer to draw into, without a device under it.
 *
 * The cases below that measure pixels rather than look at a screen build their own surface
 * instead of opening a capture, because they are about a geometry a capture would not hand them
 * - a stride with padding in it, two pages of the same scene at two widths. 32 bits a pixel with
 * no channel offsets is what both the Brick's fb0 and inkcell_capture_open() present, so what
 * comes out here is comparable with either.
 */
static struct inkcell_surface ui_capture_surface(uint8_t *pixels, uint32_t width, uint32_t height,
                                                 uint32_t stride) {
    return (struct inkcell_surface){
        .pixels = pixels,
        .size = (size_t)stride * height,
        .width = width,
        .height = height,
        .stride = stride,
        .bytes_per_pixel = 4U,
        .format = {.bits_per_pixel = 32U},
    };
}

/*
 * The ground `capture` draws on, which fb_render_snapshot() clears to before anything else.
 *
 * Taken from the capture rather than spelled out, and rather than assumed to be the default
 * theme's: mesh_ui_capture_open() honours MESHCLIENT_THEME exactly as the device backend does,
 * so a developer running `MESHCLIENT_THEME=light make test` would otherwise see this file count
 * an entire correct frame as drawn pixels and fail. A palette change is not a test change
 * either; the themes themselves are covered in ui_theme.c.
 */
static bool pixel_is_background(const struct inkcell_capture *capture, const uint8_t *pixel) {
    const struct inkcell_rgb bg =
        inkcell_theme_color(inkcell_capture_theme(capture), INKCELL_COLOR_BG);
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

static size_t count_drawn(const struct inkcell_capture *capture, const uint8_t *pixels,
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

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                                   INKCELL_CAPTURE_HEIGHT, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
    MESH_TEST_FAIL_IF_CLEANUP(pixels == NULL || width != INKCELL_CAPTURE_WIDTH ||
                                  height != INKCELL_CAPTURE_HEIGHT ||
                                  stride != (size_t)INKCELL_CAPTURE_WIDTH * 4U,
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "capture geometry is wrong");

    /* A fresh page is zeroed. Said in bytes rather than as "no pixel is the background
       colour", because a theme whose ground is pure black makes those two opposites - the
       assertion is that nothing has been drawn yet, not that the ground is a particular hue. */
    MESH_TEST_FAIL_IF_CLEANUP(!page_is_zeroed(pixels, width, height, stride),
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "an unrendered page is not blank");

    inkcell_capture_render(capture, &snapshot);
    const size_t drawn = count_drawn(capture, pixels, width, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(
        drawn == 0U || drawn > (size_t)width * (size_t)height / 2U, inkcell_capture_close(capture);
        mesh_ui_store_shutdown(&store), "the rendered frame is blank, or is not mostly background");

    inkcell_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* Whether a pixel is exactly the colour a role names. The capture fabricates 32 bpp with every
   bitfield zero, which is B,G,R,X - the same order pixel_is_background() reads. */
static bool pixel_is_role(const struct inkcell_capture *capture, const uint8_t *pixel,
                          enum inkcell_color role) {
    const struct inkcell_rgb want = inkcell_theme_color(inkcell_capture_theme(capture), role);
    return pixel[0] == want.b && pixel[1] == want.g && pixel[2] == want.r;
}

/* The widest run of `role` on any scanline, as a fraction of the content column, in percent. A
   card's padding band is an unbroken run of its fill from edge to edge of that column and its
   border is an unbroken run of the rule colour, so both come out near 100; a glyph in that
   colour comes out at a few. Takes the capture mutably only to ask it for the column - see the
   note inside. */
static unsigned widest_row_run(struct inkcell_capture *capture, const uint8_t *pixels,
                               uint32_t width, uint32_t height, size_t stride,
                               enum inkcell_color role) {
    /*
     * As a percentage of the *content column*, not of the panel.
     *
     * The two were the same number for as long as content was as wide as the surface. They stop
     * being the same the moment a surface is wider than one column of text has any use for, and
     * then the panel is the wrong denominator: a card spanning its column completely would
     * score under half and this would report that the screen had stopped drawing cards. What
     * the callers below are asking is "does this span most of the width it was given", and the
     * width a card is given is inkcell_fb_content_w().
     *
     * The run itself is still measured across the whole frame, because where the column is is
     * not this helper's business - only how wide it is.
     */
    const unsigned column = (unsigned)inkcell_fb_content_w(inkcell_capture_state(capture));
    const unsigned span = column > 0U ? column : width;
    unsigned best = 0U;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        unsigned run = 0U;
        for (uint32_t x = 0U; x < width; ++x) {
            run = pixel_is_role(capture, row + (size_t)x * 4U, role) ? run + 1U : 0U;
            const unsigned pct = (unsigned)((uint64_t)run * 100U / span);
            if (pct > best) {
                best = pct;
            }
        }
    }
    return best > 100U ? 100U : best;
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
    while (store.nav.screen != MESH_UI_SCREEN_RADIO) {
        const enum mesh_ui_screen before = store.nav.screen;
        memset(&action, 0, sizeof action);
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
        MESH_TEST_FAIL_IF_CLEANUP(store.nav.screen == before, mesh_ui_store_shutdown(&store),
                                  "the shoulder stopped moving before the Status tab");
    }

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_store_shutdown(&store), "no snapshot to render");

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                                   INKCELL_CAPTURE_HEIGHT, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
    inkcell_capture_render(capture, &snapshot);

    const unsigned fill =
        widest_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_SURFACE);
    MESH_TEST_FAIL_IF_CLEANUP(fill < 80U, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the Status screen draws no card fill across the body");

    /* OUTLINE, not RULE: a card's edge is a container's boundary rather than a separator, and
       the two parted company when a second container wanted an edge of its own. */
    const unsigned edge =
        widest_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_OUTLINE);
    MESH_TEST_FAIL_IF_CLEANUP(edge < 80U, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the Status screen draws no card edge across the body");

    inkcell_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* The lowest scanline carrying a run of `role` across most of the width, or `height` when
   there is none. A card's top and bottom edges are the only thing on these screens that puts an
   unbroken band of the outline or the accent across the panel. */
static uint32_t last_wide_run_y(const struct inkcell_capture *capture, const uint8_t *pixels,
                                uint32_t width, uint32_t height, size_t stride,
                                enum inkcell_color role) {
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
static uint32_t last_card_edge_y(const struct inkcell_capture *capture, const uint8_t *pixels,
                                 uint32_t width, uint32_t height, size_t stride) {
    const uint32_t outline =
        last_wide_run_y(capture, pixels, width, height, stride, INKCELL_COLOR_OUTLINE);
    const uint32_t ring =
        last_wide_run_y(capture, pixels, width, height, stride, INKCELL_COLOR_PRIMARY);
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
/*
 * How many separate card regions the frame has: a run down the panel of rows that are mostly one
 * card's own fill or edge, counted once per unbroken run.
 *
 * It counted *hairlines* - two per card, so three cards was six bands - and that stopped being a
 * way to find a card when a filled card stopped having one. An edge is now the outlined
 * variant's business and the focus ring's; a filled or elevated card is a fill and nothing else,
 * which is the whole point of the tonal tiers.
 *
 * So the question is asked of the fill instead, and it is the better question: what this test is
 * really pinning is that the third card is *on the panel*, and a card is on the panel when a
 * broad band of its surface is. The gaps between cards are the ground, so the runs stay
 * separate and the count is one per card.
 */
static unsigned count_card_bands(const struct inkcell_capture *capture, const uint8_t *pixels,
                                 uint32_t width, uint32_t height, size_t stride) {
    static const enum inkcell_color roles[] = {
        INKCELL_COLOR_SURFACE,
        INKCELL_COLOR_SURFACE_HIGH,
        INKCELL_COLOR_OUTLINE,
        INKCELL_COLOR_PRIMARY,
    };
    const size_t role_count = sizeof roles / sizeof roles[0];

    unsigned bands = 0U;
    bool inside = false;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        unsigned run[sizeof roles / sizeof roles[0]] = {0U};
        bool wide = false;
        for (uint32_t x = 0U; x < width && !wide; ++x) {
            const uint8_t *pixel = row + (size_t)x * 4U;
            for (size_t r = 0; r < role_count; ++r) {
                run[r] = pixel_is_role(capture, pixel, roles[r]) ? run[r] + 1U : 0U;
                if ((uint64_t)run[r] * 100U / width >= 80U) {
                    wide = true;
                    break;
                }
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
 * *last* card pays for everything above it - and paying means inkcell_fb_draw_card() refusing it
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
    while (store.nav.screen != MESH_UI_SCREEN_RADIO) {
        const enum mesh_ui_screen before = store.nav.screen;
        memset(&action, 0, sizeof action);
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
        MESH_TEST_FAIL_IF_CLEANUP(store.nav.screen == before, mesh_ui_store_shutdown(&store),
                                  "the shoulder stopped moving before the Status tab");
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

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                                   INKCELL_CAPTURE_HEIGHT, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
    inkcell_capture_render(capture, &snapshot);

    /* Three cards: Link, Mesh and Radio. One band each. */
    const unsigned bands = count_card_bands(capture, pixels, width, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(bands < 3U, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "a card was squeezed off the Status screen by the one above it");

    inkcell_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * The three card variants, and the ring that says which card the next press acts on.
 *
 * Same approach as the case above - structure, never pixels. What is asked for is that the
 * Status column is drawn at more than one weight: a card is one of three surface tiers, and a
 * screen that lost the variant would draw all three in INKCELL_COLOR_SURFACE and leave the
 * raised tier nowhere on the frame. SURFACE_HIGH spanning most of the width is the Link card
 * and nothing else on this screen; the fixture's radio has said nothing about itself, so its
 * Radio card is the outlined one and carries no fill of its own at all.
 *
 * The ring is the second half. A card holding the selected verb draws its edge in the primary
 * rather than in the outline, so a full-width run of INKCELL_COLOR_PRIMARY appears in the body
 * and appears nowhere else: the navigation bar's active chip is the primary *container*, and
 * every other use of the base colour here is a glyph, which is at most a stroke wide.
 */
MESH_TEST_CASE(ui_capture_draws_the_card_variants, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    while (store.nav.screen != MESH_UI_SCREEN_RADIO) {
        const enum mesh_ui_screen before = store.nav.screen;
        memset(&action, 0, sizeof action);
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
        MESH_TEST_FAIL_IF_CLEANUP(store.nav.screen == before, mesh_ui_store_shutdown(&store),
                                  "the shoulder stopped moving before the Status tab");
    }

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_store_shutdown(&store), "no snapshot to render");

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                                   INKCELL_CAPTURE_HEIGHT, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
    inkcell_capture_render(capture, &snapshot);

    const unsigned raised =
        widest_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_SURFACE_HIGH);
    MESH_TEST_FAIL_IF_CLEANUP(raised < 80U, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "no card is drawn on the raised tier, so the variant is lost");

    const unsigned ring =
        widest_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_PRIMARY);
    MESH_TEST_FAIL_IF_CLEANUP(ring < 80U, inkcell_capture_close(capture);
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
                pixel_is_role(capture, row + (size_t)x * 4U, INKCELL_COLOR_PRIMARY) ? run + 1U : 0U;
            if ((uint64_t)run * 100U / width >= 80U) {
                first_ring_y = y;
                break;
            }
        }
    }

    /* Two presses: the Link card carries two verbs, and the second is on the same card. */
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no second snapshot");
    inkcell_capture_render(capture, &snapshot);

    unsigned moved_ring_y = height;
    for (uint32_t y = 0U; y < height && moved_ring_y == height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        unsigned run = 0U;
        for (uint32_t x = 0U; x < width; ++x) {
            run =
                pixel_is_role(capture, row + (size_t)x * 4U, INKCELL_COLOR_PRIMARY) ? run + 1U : 0U;
            if ((uint64_t)run * 100U / width >= 80U) {
                moved_ring_y = y;
                break;
            }
        }
    }
    MESH_TEST_FAIL_IF_CLEANUP(
        moved_ring_y == height || moved_ring_y == first_ring_y, inkcell_capture_close(capture);
        mesh_ui_store_shutdown(&store), "Down did not move the ring onto the next card's verb");

    const uint32_t bottom_after = last_card_edge_y(capture, pixels, width, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(bottom_after != bottom_before, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "moving the cursor resized a card and shifted the column");

    inkcell_capture_close(capture);
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

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                                   INKCELL_CAPTURE_HEIGHT, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
    inkcell_capture_render(capture, &snapshot);

    const unsigned shape =
        widest_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_PRIMARY);
    MESH_TEST_FAIL_IF_CLEANUP(shape < 2U, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the conversation list draws no filled accent slot");

    const unsigned divider =
        widest_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_RULE);
    MESH_TEST_FAIL_IF_CLEANUP(divider < 50U, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the conversation list draws no divider between its items");

    inkcell_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * The topmost scanline carrying an unbroken run of `role` at least `min_run` pixels wide, or
 * `height` when there is none. What says *where* a container is rather than whether it exists,
 * which is the only way to ask whether something slid.
 */
static uint32_t topmost_row_run(const struct inkcell_capture *capture, const uint8_t *pixels,
                                uint32_t width, uint32_t height, size_t stride,
                                enum inkcell_color role, unsigned min_run) {
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
static void render_until_still(struct inkcell_capture *capture,
                               const struct mesh_ui_snapshot *snapshot) {
    inkcell_capture_render(capture, snapshot);
    for (unsigned i = 0U; i < 60U && inkcell_capture_animating(capture); ++i) {
        inkcell_capture_advance(capture, 33U);
        inkcell_capture_render(capture, snapshot);
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

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                                   INKCELL_CAPTURE_HEIGHT, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no snapshot to render");
    inkcell_capture_render(capture, &snapshot);
    uint8_t *quiet = snapshot_page(pixels, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(quiet == NULL, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "out of memory");

    /* An admin read on its way back: work outstanding, and nothing else about the frame
       changed. */
    struct mesh_ui_settings settings = store.settings;
    settings.admin_busy = true;
    mesh_ui_store_set_settings(&store, &settings);
    mesh_ui_store_request_refresh(&store);
    memset(&snapshot, 0, sizeof snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot), free(quiet);
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no busy snapshot");
    inkcell_capture_render(capture, &snapshot);

    const uint32_t bar_last = last_differing_row(quiet, pixels, width, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(bar_last == height, free(quiet); inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "work in flight drew nothing at all");
    /* An eighth of the panel is far more room than the tab strip and its rule take, and far
       less than the first body row reaches. A bar that consumed rows would push the whole list
       down and put this at the bottom of the frame. */
    MESH_TEST_FAIL_IF_CLEANUP(bar_last >= height / 8U, free(quiet); inkcell_capture_close(capture);
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
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no banner snapshot");
    inkcell_capture_render(capture, &snapshot);

    const unsigned container =
        widest_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_SUCCESS_CONTAINER);
    MESH_TEST_FAIL_IF_CLEANUP(container < 80U, free(quiet); inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no banner container on the frame");
    const uint32_t banner_last = last_differing_row(quiet, pixels, width, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(
        banner_last <= height / 2U, free(quiet); inkcell_capture_close(capture);
        mesh_ui_store_shutdown(&store), "the banner drew over the body instead of shortening it");

    free(quiet);
    inkcell_capture_close(capture);
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

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                                   INKCELL_CAPTURE_HEIGHT, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);

    /* Wide enough that only a fill can produce it: a glyph at this scale is a few pixels of
       stroke and the panel is 1024 across. */
    const unsigned container_run = width / 8U;

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no snapshot to render");
    render_until_still(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(topmost_row_run(capture, pixels, width, height, stride,
                                              INKCELL_COLOR_SURFACE_INVERSE,
                                              container_run) < height,
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "a frame with no notice up still draws the snackbar's surface");

    mesh_ui_store_set_toast(&store, 1000U, "Sent to BRVO");
    MESH_TEST_FAIL_IF_CLEANUP(
        !mesh_ui_store_consume_updates(&store, &snapshot), inkcell_capture_close(capture);
        mesh_ui_store_shutdown(&store), "raising a notice published no snapshot");

    inkcell_capture_render(capture, &snapshot);
    const uint32_t arriving = topmost_row_run(capture, pixels, width, height, stride,
                                              INKCELL_COLOR_SURFACE_INVERSE, container_run);
    MESH_TEST_FAIL_IF_CLEANUP(!inkcell_capture_animating(capture), inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the notice appeared in place instead of sliding in");

    render_until_still(capture, &snapshot);
    const uint32_t resting = topmost_row_run(capture, pixels, width, height, stride,
                                             INKCELL_COLOR_SURFACE_INVERSE, container_run);
    MESH_TEST_FAIL_IF_CLEANUP(resting >= height, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the notice never drew a container of the inverted surface");
    MESH_TEST_FAIL_IF_CLEANUP(resting >= arriving, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the notice did not travel upwards into its resting place");
    MESH_TEST_FAIL_IF_CLEANUP(
        resting < height / 2U, inkcell_capture_close(capture);
        mesh_ui_store_shutdown(&store),
        "the notice came to rest somewhere other than the bottom of the body");

    /* Past the four seconds the nav gives it. The store forgets the words here; the backend has
       to keep them long enough to draw the way out. */
    mesh_ui_store_tick(&store, 9000U);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.toast.text[0] != '\0', inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "the notice did not expire");
    MESH_TEST_FAIL_IF_CLEANUP(
        !mesh_ui_store_consume_updates(&store, &snapshot), inkcell_capture_close(capture);
        mesh_ui_store_shutdown(&store), "an expired notice published no snapshot");

    inkcell_capture_render(capture, &snapshot);
    const uint32_t leaving = topmost_row_run(capture, pixels, width, height, stride,
                                             INKCELL_COLOR_SURFACE_INVERSE, container_run);
    MESH_TEST_FAIL_IF_CLEANUP(leaving >= height, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the notice vanished on expiry instead of sliding out");

    render_until_still(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(
        topmost_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_SURFACE_INVERSE,
                        container_run) < height,
        inkcell_capture_close(capture);
        mesh_ui_store_shutdown(&store), "the notice is still on the panel after sliding out");

    inkcell_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/* The topmost and bottom-most rows with anything drawn on them, which on a full frame are
   inside the navigation bar and inside the status line under the keycaps. Found rather than
   stated, so a test about chrome not moving does not carry its own copy of the layout. */
static uint32_t topmost_drawn_row(const struct inkcell_capture *capture, const uint8_t *pixels,
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

static uint32_t bottommost_drawn_row(const struct inkcell_capture *capture, const uint8_t *pixels,
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
static void band_extents(const struct inkcell_capture *capture, const uint8_t *page,
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
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);

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
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    (void)mesh_ui_store_consume_updates(&store, &settling);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    (void)mesh_ui_store_consume_updates(&store, &settling);

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                                   INKCELL_CAPTURE_HEIGHT, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "no snapshot to render");
    /* The first frame adopts the place it is looking at rather than arriving at it. */
    render_until_still(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(inkcell_capture_animating(capture), inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the first frame drawn animated something");
    uint8_t *list_settled = snapshot_page(pixels, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(list_settled == NULL, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "out of memory");

    const uint32_t chrome_top = topmost_drawn_row(capture, pixels, width, height, stride);
    const uint32_t chrome_bottom = bottommost_drawn_row(capture, pixels, width, height, stride);

    /* ---- a level deeper: the thread comes in from the right --------------------------- */
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.thread_open, free(list_settled);
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "A did not open the thread");
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    inkcell_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(
        !inkcell_capture_animating(capture), free(list_settled); inkcell_capture_close(capture);
        mesh_ui_store_shutdown(&store), "the thread appeared in place instead of arriving");
    uint8_t *arriving = snapshot_page(pixels, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(arriving == NULL, free(list_settled); inkcell_capture_close(capture);
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
    MESH_TEST_FAIL_IF_CLEANUP(!chrome_held, free(list_settled); inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the navigation bar or the status line travelled with the screen");
    MESH_TEST_FAIL_IF_CLEANUP(!travelled_right, free(list_settled); inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the thread did not arrive from the right");

    /* ---- and back out: the list comes in from the left --------------------------------- */
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_B, &action);
    MESH_TEST_FAIL_IF_CLEANUP(store.nav.thread_open, free(list_settled);
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "B did not leave the thread");
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    inkcell_capture_render(capture, &snapshot);
    uint8_t *returning = snapshot_page(pixels, height, stride);
    MESH_TEST_FAIL_IF_CLEANUP(returning == NULL, free(list_settled); inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "out of memory");

    band_extents(capture, returning, list_settled, width, height, stride, &arriving_left,
                 &arriving_right);
    band_extents(capture, list_settled, returning, width, height, stride, &settled_left,
                 &settled_right);
    const bool travelled_left = arriving_right + width / 8U < settled_right;
    free(returning);
    free(list_settled);
    MESH_TEST_FAIL_IF_CLEANUP(!travelled_left, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the conversation list did not come back from the left");

    /* And it does come to rest where it started, which is the half a moving frame cannot say. */
    render_until_still(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(inkcell_capture_animating(capture), inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "the move never finished");

    inkcell_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

MESH_TEST_CASE(ui_capture_follows_the_nav, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, 320U, 240U, INKCELL_SCALE(2)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
    const size_t page_bytes = stride * (size_t)height;
    uint8_t *first = malloc(page_bytes);
    MESH_TEST_FAIL_IF_CLEANUP(first == NULL, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "out of memory");

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    inkcell_capture_render(capture, &snapshot);
    memcpy(first, pixels, page_bytes);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    MESH_TEST_FAIL_IF_CLEANUP(
        store.nav.screen != MESH_UI_SCREEN_NODES, free(first); inkcell_capture_close(capture);
        mesh_ui_store_shutdown(&store), "Right did not move to the Nodes tab");
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    inkcell_capture_render(capture, &snapshot);

    MESH_TEST_FAIL_IF_CLEANUP(
        memcmp(first, pixels, page_bytes) == 0, free(first); inkcell_capture_close(capture);
        mesh_ui_store_shutdown(&store), "two different screens rendered identically");

    free(first);
    inkcell_capture_close(capture);
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
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    }
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_settings_open(&store, MESH_UI_SETTINGS_DISPLAY),
                              mesh_ui_store_shutdown(&store), "could not open a settings section");

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                                   INKCELL_CAPTURE_HEIGHT, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);

    /* Wide enough that a warning-toned glyph cannot pass for a capsule, narrow enough that the
       shortest badge ("1 unsaved" at the smallest glyph scale a theme picks) still clears it. */
    const unsigned capsule = 60U;
    /* The chrome: the navigation bar and the app bar under it. The body starts well below. */
    const uint32_t chrome = height / 6U;

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    inkcell_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(
        topmost_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_WARNING, capsule) <
            chrome,
        inkcell_capture_close(capture);
        mesh_ui_store_shutdown(&store),
        "a section with nothing pending should carry no badge in its app bar");

    store.nav.settings_edit_count = 3U;
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    inkcell_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(
        topmost_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_WARNING, capsule) >=
            chrome,
        inkcell_capture_close(capture);
        mesh_ui_store_shutdown(&store),
        "pending edits should draw a filled badge in the app bar's trailing slot");

    inkcell_capture_close(capture);
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
 * and either side of midnight moved the day separators. inkwell_time_wall_set_fixed() is the seam
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

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                                   INKCELL_CAPTURE_HEIGHT, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
    const size_t page = (size_t)height * stride;
    uint8_t *early = pixels != NULL ? malloc(page) : NULL;
    uint8_t *later = pixels != NULL ? malloc(page) : NULL;
    MESH_TEST_FAIL_IF_CLEANUP(pixels == NULL || early == NULL || later == NULL, free(early);
                              free(later); inkcell_capture_close(capture);
                              inkwell_time_wall_set_fixed(0U);
                              mesh_ui_store_shutdown(&store), "no page to compare");

    /* Ten minutes after the message, then two hours after it: "10m" against "2h". */
    inkwell_time_wall_set_fixed(base + 600U);
    inkcell_capture_render(capture, &snapshot);
    memcpy(early, pixels, page);

    inkwell_time_wall_set_fixed(base + 7200U);
    inkcell_capture_render(capture, &snapshot);
    memcpy(later, pixels, page);

    MESH_TEST_FAIL_IF_CLEANUP(memcmp(early, later, page) == 0, free(early); free(later);
                              inkcell_capture_close(capture); inkwell_time_wall_set_fixed(0U);
                              mesh_ui_store_shutdown(&store), "the frame ignored the pinned clock");

    /* And back: the same pin has to draw the same bytes, or a checked-in screenshot still
       churns however carefully the scene pins its clock. */
    inkwell_time_wall_set_fixed(base + 600U);
    inkcell_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(memcmp(early, pixels, page) != 0, free(early); free(later);
                              inkcell_capture_close(capture); inkwell_time_wall_set_fixed(0U);
                              mesh_ui_store_shutdown(&store),
                              "the same pinned clock drew a different frame");

    free(early);
    free(later);
    inkcell_capture_close(capture);
    inkwell_time_wall_set_fixed(0U);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

MESH_TEST_CASE(ui_capture_writes_a_ppm, unit) {
    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF(mesh_ui_capture_open(&capture, 64U, 32U, INKCELL_SCALE(2)) != 0,
                      "capture open failed");

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    snprintf(snapshot.transport_status, sizeof snapshot.transport_status, "%s", "running");
    inkcell_capture_render(capture, &snapshot);

    char path[] = "/tmp/meshclient-capture-XXXXXX";
    const int fd = mkstemp(path);
    MESH_TEST_FAIL_IF_CLEANUP(fd < 0, inkcell_capture_close(capture), "cannot make a temp file");
    close(fd);

    MESH_TEST_FAIL_IF_CLEANUP(inkcell_capture_write_ppm(capture, path) != 0, unlink(path);
                              inkcell_capture_close(capture), "writing the PPM failed");

    FILE *file = fopen(path, "rb");
    MESH_TEST_FAIL_IF_CLEANUP(file == NULL, unlink(path);
                              inkcell_capture_close(capture), "cannot reopen the PPM");

    char header[16];
    memset(header, 0, sizeof header);
    const size_t header_read = fread(header, 1U, 15U, file);
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fclose(file);
    unlink(path);

    MESH_TEST_FAIL_IF_CLEANUP(header_read < 15U || strncmp(header, "P6\n64 32\n255\n", 13) != 0,
                              inkcell_capture_close(capture), "unexpected PPM header");
    /* "P6\n64 32\n255\n" is 13 bytes, then three bytes a pixel with no padding. */
    MESH_TEST_FAIL_IF_CLEANUP(size != 13L + 64L * 32L * 3L, inkcell_capture_close(capture),
                              "the PPM is not header plus one RGB triple per pixel");

    inkcell_capture_close(capture);
    record_success(test_name);
}

/* Long runs of one colour on a single scanline, ignoring the two grounds a dialog is built
   from - the body behind it and the panel itself. A filled control is such a run; a glyph, a
   hairline outline and an antialiased icon edge are all far shorter. */
static uint32_t pixel_key(const uint8_t *pixel) {
    /* 32 bpp with every bitfield zero, which is what the capture fabricates: B,G,R,X. */
    return (uint32_t)pixel[0] | ((uint32_t)pixel[1] << 8) | ((uint32_t)pixel[2] << 16);
}

static uint32_t rgb_key(struct inkcell_rgb rgb) {
    return (uint32_t)rgb.b | ((uint32_t)rgb.g << 8) | ((uint32_t)rgb.r << 16);
}

/*
 * A pixel on an anti-aliased edge between two colours.
 *
 * inkcell draws a rounded shape by blending its own colour into whatever it is over, so the
 * pixels along a curve are neither colour and every one of them fails an equality test. That is
 * what the containment cases below started reporting the day the toolkit's shapes gained
 * anti-aliasing: a dialog panel's corner, a bubble's corner and a card's edge are each a short
 * run of blends, and not one of them is something that escaped anything.
 *
 * Channel-wise between, inclusive, which is what a blend of two colours is - and what a control
 * that really did land outside its panel is not, because it is drawn in a colour the theme
 * validated as distinct from both of these.
 */
static bool pixel_between(const uint8_t *pixel, struct inkcell_rgb a, struct inkcell_rgb b) {
    const uint8_t got[3] = {pixel[2], pixel[1], pixel[0]};
    const uint8_t lo[3] = {a.r < b.r ? a.r : b.r, a.g < b.g ? a.g : b.g, a.b < b.b ? a.b : b.b};
    const uint8_t hi[3] = {a.r > b.r ? a.r : b.r, a.g > b.g ? a.g : b.g, a.b > b.b ? a.b : b.b};
    for (unsigned c = 0U; c < 3U; ++c) {
        if (got[c] < lo[c] || got[c] > hi[c]) {
            return false;
        }
    }
    return true;
}

/* Rows the dialog's raised panel covers, found by its fill rather than by re-deriving the
   layout here - a test that computed the panel's geometry itself would agree with a broken
   renderer. */
static void panel_rows(const uint8_t *pixels, uint32_t width, uint32_t height, size_t stride,
                       const struct inkcell_theme *theme, uint32_t *top, uint32_t *bottom) {
    const uint32_t panel = rgb_key(inkcell_theme_color(theme, INKCELL_COLOR_SURFACE_HIGH));
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
    struct inkcell_capture *capture = NULL;
    uint8_t *frames[2] = {NULL, NULL};

    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    for (size_t t = 0; t < inkcell_theme_count() && failure == NULL; ++t) {
        const struct inkcell_theme *theme = inkcell_theme_at(t);
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;

        for (unsigned cursor = 0U; cursor < 2U && failure == NULL; ++cursor) {
            store.nav.confirm.open = true;
            store.nav.confirm.cursor = (uint8_t)cursor;
            store.nav.screen = MESH_UI_SCREEN_SETTINGS;

            struct mesh_ui_snapshot snapshot;
            memset(&snapshot, 0, sizeof snapshot);
            mesh_ui_store_request_refresh(&store);
            if (!mesh_ui_store_consume_updates(&store, &snapshot) || !snapshot.nav.confirm.open) {
                failure = "no snapshot carrying the confirm overlay";
                break;
            }
            if (mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT,
                                     INKCELL_SCALE(4)) != 0) {
                failure = "capture open failed";
                break;
            }
            inkcell_capture_set_theme(capture, theme);
            const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
            inkcell_capture_render(capture, &snapshot);

            frames[cursor] = malloc(stride * (size_t)height);
            if (frames[cursor] == NULL) {
                failure = "out of memory";
            } else {
                memcpy(frames[cursor], pixels, stride * (size_t)height);
            }
            inkcell_capture_close(capture);
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
        inkcell_capture_close(capture);
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
    struct inkcell_capture *capture = NULL;

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
            store.nav.confirm.open = true;
            store.nav.confirm.cursor = (uint8_t)cursor;
            store.nav.confirm.subject = (uint8_t)actions[a];
            store.nav.screen = MESH_UI_SCREEN_SETTINGS;

            struct mesh_ui_snapshot snapshot;
            memset(&snapshot, 0, sizeof snapshot);
            mesh_ui_store_request_refresh(&store);
            if (!mesh_ui_store_consume_updates(&store, &snapshot) || !snapshot.nav.confirm.open) {
                failure = "no snapshot carrying the confirm overlay";
                break;
            }
            if (mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT,
                                     INKCELL_SCALE_MAX) != 0) {
                failure = "capture open failed";
                break;
            }

            uint32_t width = 0U;
            uint32_t height = 0U;
            size_t stride = 0U;
            const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
            inkcell_capture_render(capture, &snapshot);

            const struct inkcell_theme *theme = inkcell_capture_theme(capture);
            const uint32_t bg = rgb_key(inkcell_theme_color(theme, INKCELL_COLOR_BG));
            const uint32_t panel_fill =
                rgb_key(inkcell_theme_color(theme, INKCELL_COLOR_SURFACE_HIGH));

            uint32_t top = 0U;
            uint32_t bottom = 0U;
            panel_rows(pixels, width, height, stride, theme, &top, &bottom);
            if (top >= bottom) {
                failure = "the dialog drew no raised panel";
                inkcell_capture_close(capture);
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
            const struct inkcell_rgb bg_rgb = inkcell_theme_color(theme, INKCELL_COLOR_BG);
            const struct inkcell_rgb outline_rgb =
                inkcell_theme_color(theme, INKCELL_COLOR_OUTLINE);
            const uint32_t outline = rgb_key(outline_rgb);
            /*
             * And the screen under the question, drawn alone: the same snapshot with the dialog
             * put down, on the same first frame. The list behind stands in an inset section
             * whose surface and cursor capsule can reach further left than the panel at this
             * scale, and that is the screen being asked about - not a control escaping. What
             * the test is for is anything *else* out there, so a pixel that matches this frame
             * is not an escape whatever its colour.
             */
            struct mesh_ui_snapshot beneath = snapshot;
            beneath.nav.confirm.open = false;
            struct inkcell_capture *under = NULL;
            if (mesh_ui_capture_open(&under, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT,
                                     INKCELL_SCALE_MAX) != 0) {
                failure = "capture open failed";
                inkcell_capture_close(capture);
                capture = NULL;
                break;
            }
            inkcell_capture_render(under, &beneath);
            const uint8_t *behind = inkcell_capture_pixels(under, NULL, NULL, NULL);
            const struct inkcell_rgb shadow_rgb = inkcell_theme_color(theme, INKCELL_COLOR_SHADOW);
            size_t escaped = 0U;
            for (uint32_t y = top; y <= bottom && left > 0U; ++y) {
                const uint8_t *row = pixels + (size_t)y * stride;
                for (uint32_t x = 0; x < left; ++x) {
                    const uint8_t *px = row + (size_t)x * 4U;
                    const uint32_t key = pixel_key(px);
                    /* The edge as it turns through a corner is a blend of itself and the ground
                       it is over - see pixel_between(). It is still the panel's own edge. */
                    const size_t at = (size_t)y * stride + (size_t)x * 4U;
                    /* The panel's shadow falls out here too, and all a shadow can do is take
                       what was behind part of the way towards the theme's shadow colour. */
                    const struct inkcell_rgb behind_rgb = {
                        .r = behind[at + 2U], .g = behind[at + 1U], .b = behind[at]};
                    if (key != bg && key != outline && pixel_key(behind + at) != key &&
                        !pixel_between(px, bg_rgb, outline_rgb) &&
                        !pixel_between(px, behind_rgb, shadow_rgb)) {
                        ++escaped;
                    }
                }
            }
            inkcell_capture_close(under);
            inkcell_capture_close(capture);
            capture = NULL;

            if (escaped > 0U) {
                failure = "a dialog control is drawn outside its own panel";
            }
        }
    }

    if (capture != NULL) {
        inkcell_capture_close(capture);
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
/*
 * How wide the content column is on a panel this size, at this scale.
 *
 * The denominator for every "does this span most of the width" sweep below. It used to be the
 * panel's width, and that was the same number for as long as content was as wide as the
 * surface; where the column is capped and centred it is not, and a sweep measured against the
 * panel reports that a card spanning its whole column has stopped being a card.
 *
 * Asked of the toolkit rather than re-derived, for settings_label_right()'s reason.
 */
static uint32_t content_column_width(const struct inkcell_theme *theme, uint32_t width, int scale) {
    struct inkcell_capture *capture = NULL;
    if (mesh_ui_capture_open(&capture, width, INKCELL_CAPTURE_HEIGHT, scale) != 0) {
        return width;
    }
    inkcell_capture_set_theme(capture, theme);
    inkcell_capture_set_scale(capture, scale);
    const uint32_t column = (uint32_t)inkcell_fb_content_w(inkcell_capture_state(capture));
    inkcell_capture_close(capture);
    return column > 0U ? column : width;
}

static uint32_t settings_label_right(const struct inkcell_theme *theme, uint32_t width, int scale) {
    /*
     * Asked of the toolkit rather than re-derived, which it used to be: the arithmetic here was
     * `margin + label_cols * advance` over a count taken from the panel, and both halves of that
     * stopped being true when content stopped being as wide as the surface. A row's label column
     * starts at inkcell_fb_content_x() now, and how many columns it gets is
     * inkcell_fb_field_label_cols() - the renderer's own answer, over the *body's* count.
     *
     * Re-deriving it was always the weaker version. It passed for as long as the two derivations
     * happened to agree, which is exactly as long as nobody changed the layout - so the case it
     * was guarding was the one case it could not survive.
     */
    struct inkcell_capture *capture = NULL;
    if (mesh_ui_capture_open(&capture, width, INKCELL_CAPTURE_HEIGHT, scale) != 0) {
        return 0U;
    }
    inkcell_capture_set_theme(capture, theme);
    inkcell_capture_set_scale(capture, scale);

    struct inkcell_draw_state *const state = inkcell_capture_state(capture);
    const struct inkcell_fb_layout layout = inkcell_fb_layout_begin(state, true, true);
    const int advance = inkcell_fb_char_adv(state, scale);
    const size_t label_cols = inkcell_fb_field_label_cols(state, &layout, 0U);
    const uint32_t right = (uint32_t)(inkcell_fb_content_x(state) + (int)label_cols * advance);

    inkcell_capture_close(capture);
    return right;
}

/*
 * Whether the last frame capture_frame() took put the tabs in a rail rather than a strip.
 *
 * Several cases sweep "the body" as everything below the first eighth of the panel, which is a
 * cheap way of stepping over the tab strip. A frame with a rail has no strip to step over, and on
 * one drawn at a small glyph scale the first rows of the body are *inside* that eighth - so those
 * cases ask capture_body_top() instead of assuming a strip is there.
 */
static bool capture_railed;

static uint32_t capture_body_top(uint32_t height) { return capture_railed ? 0U : height / 8U; }

/*
 * The rendered page from where its content starts: the same stride, a narrower width and a
 * pointer moved past the tab rail, so a scan written against a panel's margin starts at the body
 * rather than on the rail's labels. The whole page when there is no rail.
 */
static const uint8_t *capture_body(struct inkcell_capture *capture, const uint8_t *pixels,
                                   uint32_t *width) {
    const struct inkcell_box content = mesh_ui_capture_content(capture);
    const size_t bpp = inkcell_capture_state(capture)->surface.bytes_per_pixel;
    *width = (uint32_t)content.w;
    return pixels + (size_t)content.x * bpp;
}

/* Renders `store` as it stands into a fresh capture at `scale`, and hands back a copy of the
   page. The caller frees it. */
static uint8_t *capture_frame(struct mesh_ui_store *store, const struct inkcell_theme *theme,
                              int scale, uint32_t *out_w, uint32_t *out_h, size_t *out_stride) {
    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(store);
    if (!mesh_ui_store_consume_updates(store, &snapshot)) {
        return NULL;
    }
    struct inkcell_capture *capture = NULL;
    if (mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT, scale) != 0) {
        return NULL;
    }
    inkcell_capture_set_theme(capture, theme);
    /* After the theme, not before: a theme carries a scale of its own and adopting one unpins
       whatever was asked for at open. This is the same order the scene scripts' `theme` and
       `scale` lines are read in. */
    inkcell_capture_set_scale(capture, scale);
    const uint8_t *pixels = inkcell_capture_pixels(capture, out_w, out_h, out_stride);
    inkcell_capture_render(capture, &snapshot);
    /*
     * The body, without the rail beside it.
     *
     * At the smaller glyph scales the panel is wide enough in columns to leave the compact width
     * class, and the tabs move from a strip across the top into a rail down the leading edge.
     * Every case here reads the *body* - where a row's words start, where a card's edge is - and
     * measures it from the panel's margin, so the frame is handed back cut to where the content
     * starts: the same picture those cases were written against, at the same margin. At the
     * device's own scale the content is the whole panel and nothing is cut.
     */
    const struct inkcell_box content = mesh_ui_capture_content(capture);
    capture_railed = content.x > 0;
    const size_t bpp = inkcell_capture_state(capture)->surface.bytes_per_pixel;
    const uint32_t width = (uint32_t)content.w;
    const size_t stride = (size_t)width * bpp;
    uint8_t *frame = malloc(stride * (size_t)*out_h);
    if (frame != NULL) {
        for (uint32_t y = 0U; y < *out_h; ++y) {
            memcpy(frame + (size_t)y * stride,
                   pixels + (size_t)y * *out_stride + (size_t)content.x * bpp, stride);
        }
        *out_w = width;
        *out_stride = stride;
    }
    inkcell_capture_close(capture);
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

    for (size_t t = 0; t < inkcell_theme_count() && failure == NULL; ++t) {
        const struct inkcell_theme *theme = inkcell_theme_at(t);
        for (int scale = INKCELL_SCALE_MIN; scale <= INKCELL_SCALE_MAX && failure == NULL;
             scale += INKCELL_SCALE(1)) {
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
                const uint32_t body_top = capture_body_top(height);
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

    for (size_t t = 0; t < inkcell_theme_count() && failure == NULL; ++t) {
        const struct inkcell_theme *theme = inkcell_theme_at(t);
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
                frames[pass] =
                    capture_frame(&store, theme, INKCELL_SCALE(4), &width, &height, &stride);
                if (frames[pass] == NULL) {
                    failure = "capture failed";
                }
            }
            mesh_ui_store_shutdown(&store);
        }

        if (failure == NULL) {
            const uint32_t body_top = capture_body_top(height);
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

    for (size_t t = 0; t < inkcell_theme_count() && failure == NULL; ++t) {
        const struct inkcell_theme *theme = inkcell_theme_at(t);
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
            frames[pass] = capture_frame(&store, theme, INKCELL_SCALE(4), &width, &height, &stride);
            if (frames[pass] == NULL) {
                failure = "capture failed";
            }
            mesh_ui_store_shutdown(&store);
        }

        if (failure == NULL) {
            const uint32_t body_top = capture_body_top(height);
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

/*
 * A row whose control did not get drawn keeps the marker that was standing down for it.
 *
 * `marker_yields_to_control` exists because asking for a control is not the same as getting
 * one. A segmented button falls back to its chosen word when the value column cannot hold the
 * segments - a narrow panel, a large glyph scale - and a row that had dropped its own marker on
 * the strength of *asking* comes out as a label and a word, which is precisely the shape of a
 * row that cannot be changed. An editable setting drawn as a stated fact is the bug this holds:
 * the reader is left with no way to know Left and Right do anything on it.
 *
 * Asserted by drawing the same row twice - once yielding, once with no marker at all - and
 * comparing the two frames, which is the whole claim without a coordinate in it. A test that
 * worked out where the gutter is would agree with a renderer that had moved it.
 *
 * Two ways a row can ask for a control and not get one, and both are here:
 *
 *   - a segmented button that cannot hold its segments draws its chosen word instead
 *   - a *fixed-size* control - a switch, a checkbox, a radio - is not drawn at all when
 *     inkcell_fb_trailing_cols() cannot spare it the cells
 *
 * The second is the one that reads as impossible and is not: the row keeps its label, loses the
 * switch it asked for, and on a panel this narrow that is all there is.
 *
 * Each case gets a wide panel where the control is drawn and a narrow one inside the band where
 * it is not but the marker cell still is. Those bands are real and narrow at this scale - very
 * roughly 128-224 pixels for the segments and 120-160 for a switch - and their *lower* ends
 * matter as much as the upper: under them the row cannot draw a marker either, which is a
 * different answer and not the one under test.
 */
MESH_TEST_CASE(fb_a_segmented_row_that_fell_back_keeps_its_marker, unit) {
    enum { HEIGHT = 96U, MAX_WIDTH = 512U, STRIDE = MAX_WIDTH * 4U };
    static const char *const k_labels[] = {"Off", "On", "Auto"};
    /* Wide enough for the control, then narrow enough that it cannot be drawn - once for the
       segmented button, once for a switch, whose two bands do not coincide. */
    const uint32_t widths[4] = {448U, 160U, 448U, 144U};
    const bool segmented_row[4] = {true, true, false, false};
    const bool expect_control[4] = {true, false, true, false};
    uint8_t *frames[2] = {NULL, NULL};
    const char *failure = NULL;

    for (unsigned i = 0U; i < 2U; ++i) {
        frames[i] = calloc(1U, (size_t)STRIDE * HEIGHT);
        if (frames[i] == NULL) {
            failure = "frame allocation failed";
            goto cleanup;
        }
    }

    for (unsigned panel = 0U; panel < 4U && failure == NULL; ++panel) {
        /* Two passes over one row: the row under test, and the same row with nothing in the
           gutter. Identical frames mean the marker yielded to a control that was drawn; frames
           that differ mean it stood. */
        for (unsigned pass = 0U; pass < 2U; ++pass) {
            struct inkcell_draw_state state = {0};
            state.surface = ui_capture_surface(frames[pass], widths[panel], HEIGHT, STRIDE);
            memset(state.surface.pixels, 0, state.surface.size);
            inkcell_fb_state_set_theme(&state, inkcell_theme_default(), INKCELL_SCALE(2));

            struct inkcell_fb_layout layout = {0};
            layout.footer_y = (int)HEIGHT;
            layout.line = 24;
            layout.rows = 2U;
            layout.cols = widths[panel] / 12U;
            layout.body_w = (int)widths[panel] - 16;
            layout.small = 2;

            struct inkcell_fb_segmented segmented = {
                .count = 3U, .active = 1U, .value = k_labels[1]};
            for (size_t c = 0U; c < 3U; ++c) {
                segmented.labels[c] = k_labels[c];
            }
            struct inkcell_fb_switch sw = {.id = 1U, .on = true};
            const struct inkcell_fb_list_item row = {
                .label = "Mode",
                .label_cols = 6U,
                .marker_icon = pass == 0U ? INKCELL_ICON_STEPPER : INKCELL_ICON_NONE,
                .marker_yields_to_control = pass == 0U,
                .trailing =
                    segmented_row[panel]
                        ? (struct inkcell_fb_trailing){.kind = INKCELL_FB_TRAILING_SEGMENTED,
                                                       .segmented = &segmented}
                        : (struct inkcell_fb_trailing){.kind = INKCELL_FB_TRAILING_SWITCH,
                                                       .sw = &sw},
            };
            struct inkcell_fb_list list = inkcell_fb_list_begin(&layout, 1U, 0U);
            uint32_t i;
            while (inkcell_fb_list_next(&list, &i)) {
                inkcell_fb_list_item(&state, &list, i, &row);
            }
            inkcell_fb_glyph_cache_free(&state);
        }

        const bool yielded = memcmp(frames[0], frames[1], (size_t)STRIDE * HEIGHT) == 0;
        if (yielded != expect_control[panel]) {
            failure = expect_control[panel] ? "a row that drew its control should have stood "
                                              "its marker down"
                                            : "a row whose control was not drawn must keep its "
                                              "marker";
        }
    }

cleanup:
    for (unsigned i = 0U; i < 2U; ++i) {
        free(frames[i]);
    }
    if (failure != NULL) {
        record_failure(test_name, failure);
        return;
    }
    record_success(test_name);
}

MESH_TEST_CASE(fb_glyph_cache_matches_uncached_colors_and_scales, unit) {
    uint8_t cached_pixels[256U * 128U * 4U];
    uint8_t reference[sizeof cached_pixels];
    struct inkcell_draw_state state = {0};
    state.surface = ui_capture_surface(NULL, 256U, 128U, 256U * 4U);
    const char *failure = NULL;
    inkcell_fb_state_set_theme(&state, inkcell_theme_default(), INKCELL_SCALE(4));
    struct inkcell_fb_glyph_cache *cache = state.glyph_cache;
    for (int scale = INKCELL_SCALE_MIN; scale <= INKCELL_SCALE_MAX && failure == NULL;
         scale += INKCELL_SCALE(1)) {
        for (unsigned pass = 0; pass < 3U; ++pass) {
            const struct inkcell_rgb ink = {(uint8_t)(pass * 91U), 170U, 250U};
            const struct inkcell_rgb ground = {30U, (uint8_t)(pass * 71U), 10U};
            state.surface.pixels = cached_pixels;
            state.glyph_cache = cache;
            inkcell_fb_clear(&state, ground);
            inkcell_fb_draw_text(&state, -3, 10, "Ab éñ!?", scale, ink, ground);
            state.surface.pixels = reference;
            state.glyph_cache = NULL;
            inkcell_fb_clear(&state, ground);
            inkcell_fb_draw_text(&state, -3, 10, "Ab éñ!?", scale, ink, ground);
            if (memcmp(reference, cached_pixels, sizeof reference) != 0) {
                failure = "cached glyphs must match uncached output after color and scale changes";
                break;
            }
        }
    }
    state.glyph_cache = cache;
    inkcell_fb_glyph_cache_free(&state);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A keycap carrying one emoji draws it at the key's size, and one carrying anything else does
 * not.
 *
 * The emoji layer of the on-screen keyboard used to draw its forty cells through the ordinary
 * text path, which sizes an emoji like a letter: a 20 px thumbnail of a picture, in a key five
 * times that across, and the layer could not be read without leaning into the panel. The rule
 * that replaced it is the one asserted here - a label that is a single sprite is the key's
 * *face* and is sized to the box - and it is asserted as an extent rather than as a pixel
 * because how a sprite fills its own square is the emoji font's business, not this client's.
 *
 * The second half is what keeps the first from being a blanket rule: the same flag over a
 * letter, a word or an icon has to change nothing, because the four keyboard layers are one
 * grid described once and three of them are text.
 */
MESH_TEST_CASE(fb_emoji_keycap_fills_its_key, unit) {
    uint8_t page[256U * 128U * 4U];
    struct inkcell_draw_state state = {0};
    state.surface = ui_capture_surface(page, 256U, 128U, 256U * 4U);
    inkcell_fb_state_set_theme(&state, inkcell_theme_default(), INKCELL_SCALE(4));

    const struct inkcell_rgb ground = inkcell_fb_color(&state, INKCELL_COLOR_BG);
    const struct inkcell_fb_rect key = {.x = 20, .y = 10, .w = 120, .h = 100};
    const int cell = inkcell_fb_char_adv(&state, INKCELL_SCALE(4));
    const char *failure = NULL;

    /* An extent per pass: the drawn ink's bounding box, which for a sprite on a key that lays
       down no fill of its own is the sprite and nothing else. */
    int width[2] = {0, 0};
    int height[2] = {0, 0};
    for (unsigned pass = 0; pass < 2U && failure == NULL; ++pass) {
        inkcell_fb_clear(&state, ground);
        const struct inkcell_fb_button button = {
            .rect = key,
            .label = "\U0001F600",
            .variant = INKCELL_FB_BUTTON_TEXT,
            .shape = INKCELL_SHAPE_SM,
            .idle_tone = INKCELL_TONE_NORMAL,
            .scale = INKCELL_SCALE(4),
            .emoji_face = (pass == 0U),
        };
        inkcell_fb_draw_button(&state, &button);

        int left = inkcell_fb_panel_width(&state);
        int right = -1;
        int top = inkcell_fb_panel_height(&state);
        int bottom = -1;
        for (uint32_t row = 0U; row < state.surface.height; ++row) {
            const uint8_t *line = page + (size_t)row * state.surface.stride;
            for (uint32_t col = 0U; col < state.surface.width; ++col) {
                const uint8_t *pixel = &line[(size_t)col * 4U];
                if (pixel[0] == ground.b && pixel[1] == ground.g && pixel[2] == ground.r) {
                    continue;
                }
                if ((int)col < left) {
                    left = (int)col;
                }
                if ((int)col > right) {
                    right = (int)col;
                }
                if ((int)row < top) {
                    top = (int)row;
                }
                if ((int)row > bottom) {
                    bottom = (int)row;
                }
            }
        }
        if (right < 0 || bottom < 0) {
            failure = "the keycap drew nothing at all";
            break;
        }
        if (left < key.x || right >= key.x + key.w || top < key.y || bottom >= key.y + key.h) {
            failure = "the keycap drew outside its own key";
            break;
        }
        width[pass] = right - left + 1;
        height[pass] = bottom - top + 1;
    }

    /* Half the key is the floor rather than the whole of it: a sprite carries its own
       transparent margin, and a grinning face does not reach the corners of its square. What
       fails here is the regression - a face drawn at a text cell, which is a fifth of this. */
    if (failure == NULL && (width[0] < key.w / 2 || height[0] < key.h / 2)) {
        failure = "an emoji keycap must be sized to its key, not to a text cell";
    }
    if (failure == NULL && (width[1] > cell || height[1] > cell)) {
        failure = "a keycap that is not asking for a sprite face must stay one text cell";
    }

    inkcell_fb_glyph_cache_free(&state);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A sprite wider than the column map still draws.
 *
 * The bound on that map used to sit above the block path, which has no use for it - so a panel
 * with room for a key wider than INKCELL_FB_EMOJI_BOX_MAX (the capture tool renders up to 4096
 * square) drew every emoji keycap as nothing at all, the selected one as a bare fill. Nothing on
 * the Brick reaches that size, which is exactly why it needs a test rather than an eye.
 *
 * The oversized box is the one this asserts, not the panel: a box the primitive is handed is a
 * box it draws, whatever a caller's arithmetic made of it.
 */
MESH_TEST_CASE(fb_emoji_box_draws_past_the_column_map, unit) {
    uint8_t page[320U * 320U * 4U];
    struct inkcell_draw_state state = {0};
    state.surface = ui_capture_surface(page, 320U, 320U, 320U * 4U);
    inkcell_fb_state_set_theme(&state, inkcell_theme_default(), INKCELL_SCALE(4));

    const uint32_t grinning = 0x1F600U;
    uint16_t sprite = 0;
    const char *failure = NULL;
    if (inkcell_emoji_match(&grinning, 1U, &sprite) == 0U) {
        failure = "the build has no sprite for U+1F600";
    }

    /* The bound itself, one over it, and a size well past it that is not a whole multiple -
       which is the combination the old guard dropped. */
    const int boxes[] = {INKCELL_FB_EMOJI_BOX_MAX, INKCELL_FB_EMOJI_BOX_MAX + 1,
                         INKCELL_FB_EMOJI_BOX_MAX + 7};
    const struct inkcell_rgb ground = inkcell_fb_color(&state, INKCELL_COLOR_BG);
    for (size_t i = 0; i < sizeof boxes / sizeof boxes[0] && failure == NULL; ++i) {
        const int box = boxes[i];
        inkcell_fb_clear(&state, ground);
        inkcell_fb_draw_emoji_box(&state, 10, 10, box, sprite);

        bool drew = false;
        bool escaped = false;
        for (uint32_t row = 0U; row < state.surface.height; ++row) {
            const uint8_t *line = page + (size_t)row * state.surface.stride;
            for (uint32_t col = 0U; col < state.surface.width; ++col) {
                const uint8_t *pixel = &line[(size_t)col * 4U];
                if (pixel[0] == ground.b && pixel[1] == ground.g && pixel[2] == ground.r) {
                    continue;
                }
                drew = true;
                if ((int)row < 10 || (int)row >= 10 + box || (int)col < 10 ||
                    (int)col >= 10 + box) {
                    escaped = true;
                }
            }
        }
        if (!drew) {
            failure = "a box wider than the column map drew nothing";
        } else if (escaped) {
            failure = "a box wider than the column map drew outside itself";
        }
    }

    inkcell_fb_glyph_cache_free(&state);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Every source pixel the same size, or none of the snapping is worth doing.
 *
 * inkcell_fb_emoji_box_fit() is what keeps a five-times upscale from landing as a mix of five- and
 * six-pixel blocks - one eye a pixel wider than the other - and it is also what lets
 * inkcell_fb_draw_emoji_box() take its block path, which draws the identical pixels for a fraction
 * of the comparisons. Both properties are the same one arithmetic fact.
 */
MESH_TEST_CASE(fb_emoji_box_fit_snaps_to_whole_blocks, unit) {
    const char *failure = NULL;
    for (int box = 1; box <= 8 * INKCELL_EMOJI_SIZE && failure == NULL; ++box) {
        const int fit = inkcell_fb_emoji_box_fit(box);
        if (fit > box || fit <= 0) {
            failure = "a fitted box must be positive and never larger than the room for it";
        } else if (box >= 2 * INKCELL_EMOJI_SIZE && fit % INKCELL_EMOJI_SIZE != 0) {
            failure = "a box with room for whole blocks must be a whole number of them";
        } else if (box >= 2 * INKCELL_EMOJI_SIZE && box - fit >= INKCELL_EMOJI_SIZE) {
            failure = "snapping must cost less than a whole block";
        } else if (box < 2 * INKCELL_EMOJI_SIZE && fit != box) {
            failure = "a box the size of a text cell must be left alone";
        }
    }
    MESH_TEST_FAIL_IF(failure != NULL, failure);
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
    /* And a relay chip, which is the newest part of the trailing run and so the newest way for
       one to grow past the bubble measured around it. The longest form it takes: a four-cell
       short name, which is what the store writes when the roster resolves the byte. */
    snprintf(message->relay_name, sizeof message->relay_name, "RLAY");
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
       different colour and the focus ring is laid around it. */
    static const uint8_t acks[] = {MESH_MESSAGE_ACK_NONE, MESH_MESSAGE_ACK_PENDING,
                                   MESH_MESSAGE_ACK_DELIVERED, MESH_MESSAGE_ACK_FAILED};
    /* The longest reason the catalog carries, which is what a bubble has least room for. */
    static const uint8_t kPkiUnknownPubkey = 35U;

    struct inkcell_capture *capture = NULL;
    for (size_t t = 0; t < inkcell_theme_count() && failure == NULL; ++t) {
        const struct inkcell_theme *theme = inkcell_theme_at(t);
        for (int scale = INKCELL_SCALE_MIN; scale <= INKCELL_SCALE_MAX && failure == NULL;
             scale += INKCELL_SCALE(1)) {
            for (size_t a = 0; a < sizeof acks / sizeof acks[0] && failure == NULL; ++a) {
                /* Cursor 0 is on the only bubble there is; anything past it is the same
                   transcript at rest. Both, because the cursor changes the fill and lays a
                   ring around it. */
                for (uint32_t cursor = 0U; cursor < 2U && failure == NULL; ++cursor) {
                    const bool selected = (cursor == 0U);
                    message->ack = acks[a];
                    message->ack_error =
                        acks[a] == MESH_MESSAGE_ACK_FAILED ? kPkiUnknownPubkey : 0U;
                    snapshot->nav.cursor[MESH_UI_SCREEN_MESSAGES] = cursor;

                    if (mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                             INKCELL_CAPTURE_HEIGHT, scale) != 0) {
                        failure = "capture open failed";
                        break;
                    }
                    inkcell_capture_set_theme(capture, theme);
                    inkcell_capture_set_scale(capture, scale);
                    uint32_t width = 0U, height = 0U;
                    size_t stride = 0U;
                    const uint8_t *pixels =
                        inkcell_capture_pixels(capture, &width, &height, &stride);
                    inkcell_capture_render(capture, snapshot);
                    pixels = capture_body(capture, pixels, &width);

                    /* The bubble's own fill, asked of the theme the same way the renderer asks:
                       ours in the secondary container, or the error container once it failed,
                       with the cursor's state layer over whichever it is. */
                    const struct inkcell_paint paint = inkcell_theme_paint(
                        theme,
                        acks[a] == MESH_MESSAGE_ACK_FAILED ? INKCELL_FAMILY_ERROR
                                                           : INKCELL_FAMILY_SECONDARY,
                        INKCELL_SLOT_CONTAINER,
                        selected ? INKCELL_STATE_FOCUSED : INKCELL_STATE_REST);
                    const uint32_t fill = rgb_key(paint.fill);
                    const struct inkcell_rgb bg_rgb = inkcell_theme_color(theme, INKCELL_COLOR_BG);
                    const uint32_t bg = rgb_key(bg_rgb);
                    /* The cursor's ring, laid under the fill and a scale wide all round it. */
                    const uint32_t ring =
                        rgb_key(inkcell_theme_color(theme, INKCELL_COLOR_PRIMARY));

                    /*
                     * The bubble's span on every row first, then the containment check against
                     * it - two passes rather than one, because a row's own span is not the
                     * whole of what the bubble reaches on that row.
                     *
                     * A rounded fill's edge turns across rows: where it is turning, the row
                     * above reaches further across than this row's fill does, and a check that
                     * measured only this row would read the bubble's own corner as ink that
                     * escaped. So a row is allowed the widest of itself and its two
                     * neighbours, which is what "along the edge, including where it turns"
                     * means in pixels.
                     */
                    uint32_t *row_left = calloc(height, sizeof *row_left);
                    uint32_t *row_right = calloc(height, sizeof *row_right);
                    if (row_left == NULL || row_right == NULL) {
                        free(row_left);
                        free(row_right);
                        inkcell_capture_close(capture);
                        capture = NULL;
                        failure = "row span allocation failed";
                        break;
                    }
                    /* The ring's own extent: under the cursor it is the bubble's outer edge, and
                       its corners are blends of three colours no span of the fill can model. */
                    uint32_t ring_x0 = width, ring_x1 = 0U, ring_y0 = height, ring_y1 = 0U;
                    for (uint32_t y = 0; y < height; ++y) {
                        const uint8_t *row = pixels + (size_t)y * stride;
                        uint32_t left = width, right = 0U;
                        for (uint32_t x = 0; x < width; ++x) {
                            const uint32_t key = pixel_key(row + (size_t)x * 4U);
                            if (selected && key == ring) {
                                ring_x0 = x < ring_x0 ? x : ring_x0;
                                ring_x1 = x > ring_x1 ? x : ring_x1;
                                ring_y0 = y < ring_y0 ? y : ring_y0;
                                ring_y1 = y > ring_y1 ? y : ring_y1;
                            }
                            if (key != fill) {
                                continue;
                            }
                            if (x < left) {
                                left = x;
                            }
                            right = x;
                        }
                        row_left[y] = left;
                        row_right[y] = right;
                    }

                    size_t escaped = 0U;
                    uint32_t rows = 0U;
                    for (uint32_t y = 0; y < height; ++y) {
                        const uint8_t *row = pixels + (size_t)y * stride;
                        uint32_t left = row_left[y];
                        uint32_t right = row_right[y];
                        /* A row the bubble does not cover, or covers only in a corner's
                           stepping - neither says anything about containment. */
                        if (left >= right || right - left < (uint32_t)inkcell_scale_px(4, scale)) {
                            continue;
                        }
                        rows += 1U;
                        for (uint32_t n = (y > 0U ? y - 1U : 0U); n <= y + 1U && n < height; ++n) {
                            if (row_left[n] < row_right[n]) {
                                if (row_left[n] < left) {
                                    left = row_left[n];
                                }
                                if (row_right[n] > right) {
                                    right = row_right[n];
                                }
                            }
                        }
                        for (uint32_t x = 0; x < width; ++x) {
                            if (x >= left && x <= right) {
                                continue;
                            }
                            /* Inside the ring, anti-aliased edge included, is inside the bubble. */
                            if (selected && ring_x0 <= ring_x1 && x + 1U >= ring_x0 &&
                                x <= ring_x1 + 1U && y + 1U >= ring_y0 && y <= ring_y1 + 1U) {
                                continue;
                            }
                            const uint8_t *px = row + (size_t)x * 4U;
                            /* The corner the fill is curving through is a blend of the fill and
                               the ground - the bubble's own edge, not ink that left it. */
                            if (pixel_key(px) != bg && !pixel_between(px, bg_rgb, paint.fill)) {
                                ++escaped;
                            }
                        }
                    }
                    free(row_left);
                    free(row_right);
                    inkcell_capture_close(capture);
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
        inkcell_capture_close(capture);
    }
    free(snapshot);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

MESH_TEST_CASE(fb_transcript_cache_matches_reference_after_mutations, unit) {
    struct inkcell_capture *cached = NULL, *reference = NULL;
    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    const char *failure = NULL;
    if (snapshot == NULL || mesh_ui_capture_open(&cached, 1024U, 768U, INKCELL_SCALE(4)) != 0 ||
        mesh_ui_capture_open(&reference, 1024U, 768U, INKCELL_SCALE(4)) != 0) {
        failure = "capture allocation failed";
        goto cleanup;
    }
    inkcell_capture_set_reference(reference, true);
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
            inkcell_i18n_set_locale("es");
        if (pass == 7U) {
            inkcell_capture_set_scale(cached, INKCELL_SCALE(3));
            inkcell_capture_set_scale(reference, INKCELL_SCALE(3));
        }
        if (pass == 8U)
            snapshot->nav.inbox = false;
        if (pass == 9U)
            snapshot->nav.target_node = MESH_MESSAGE_BROADCAST_ADDR;
        if (pass == 10U)
            snapshot->messages.count = 1U;
        inkcell_capture_render(cached, snapshot);
        inkcell_capture_render(reference, snapshot);
        if (memcmp(inkcell_capture_pixels(cached, NULL, NULL, NULL),
                   inkcell_capture_pixels(reference, NULL, NULL, NULL), 1024U * 768U * 4U) != 0) {
            failure = "cached transcript differs after navigation or input mutation";
            break;
        }
    }
cleanup:
    inkcell_i18n_set_locale("en");
    inkcell_capture_close(cached);
    inkcell_capture_close(reference);
    free(snapshot);
    if (failure != NULL)
        record_failure(test_name, failure);
    else
        record_success(test_name);
}

MESH_TEST_CASE(fb_animation_clip_matches_full_composition, unit) {
    struct inkcell_draw_state state[2] = {0};
    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    const char *failure = NULL;
    unsigned clipped = 0U;
    if (snapshot == NULL) {
        record_failure(test_name, "snapshot allocation failed");
        return;
    }
    for (unsigned i = 0; i < 2U; ++i) {
        state[i].surface = ui_capture_surface(NULL, 1024U, 768U, 4096U);
        state[i].surface.pixels = calloc(1U, state[i].surface.size);
        if (state[i].surface.pixels == NULL) {
            failure = "frame allocation failed";
            goto cleanup;
        }
        inkcell_fb_state_set_theme(&state[i], inkcell_theme_default(), INKCELL_SCALE(4));
    }
    state[1].partial_disabled = true;
    snapshot->nav.screen = MESH_UI_SCREEN_MESSAGES;
    for (unsigned frame = 0U; frame < 50U; ++frame) {
        if (frame == 1U || frame == 20U) {
            snprintf(snapshot->nav.toast.text, sizeof snapshot->nav.toast.text, "Saved");
            snapshot->nav.toast.until_ms = 5000U + frame;
        }
        if (frame == 15U || frame == 35U)
            snapshot->nav.toast.text[0] = '\0';
        /* A move between tabs, which since step 13 is also a screen transition: the body is
           redrawn at an offset for the length of one, off a snapshot that does not change while
           it runs. Whatever that declares as damage has to be enough for the clipped
           composition below to still match the full one. */
        if (frame == 25U)
            snapshot->nav.screen = MESH_UI_SCREEN_NODES;
        if (frame == 30U) {
            inkcell_fb_state_set_theme(&state[0], inkcell_theme_default(), INKCELL_SCALE(3));
            inkcell_fb_state_set_theme(&state[1], inkcell_theme_default(), INKCELL_SCALE(3));
        }
        for (unsigned i = 0U; i < 2U; ++i) {
            inkcell_fb_state_set_now(&state[i], 1000U + frame * 16U);
            fb_render_snapshot(&state[i], snapshot);
            /* A second animation above the snackbar must participate in the next clip. */
            const struct inkcell_fb_selection selection = {
                .id = 0x7FFFFFFEU,
                .rect = {.x = 80, .y = 200, .w = 24, .h = 24},
                .shape = INKCELL_FB_SELECTION_RADIO,
                .on = frame >= 5U && frame < 18U,
            };
            inkcell_fb_draw_selection(&state[i], &selection);
        }
        if (state[0].clip_active)
            clipped++;
        if (memcmp(state[0].surface.pixels, state[1].surface.pixels, state[0].surface.size) != 0) {
            failure = "animation clip must restore overlapping content on arrival and dismissal";
            goto cleanup;
        }
    }
    if (clipped == 0U)
        failure = "comparison did not exercise partial drawing";
cleanup:
    for (unsigned i = 0U; i < 2U; ++i) {
        inkcell_fb_glyph_cache_free(&state[i]);
        fb_thread_cache_free(&state[i]);
        fb_render_cache_free(&state[i]);
        free(state[i].surface.pixels);
    }
    free(snapshot);
    if (failure != NULL)
        record_failure(test_name, failure);
    else
        record_success(test_name);
}

/*
 * A screen sliding in while a layer is on the panel, under the partial-composition clip.
 *
 * The field report: pairing raised the passkey keyboard, and it came up with every word of the
 * Devices list still showing through it until the next press repainted the screen - then the
 * same again on the way back. A layer (here the snackbar) carries its last box into the next
 * frame as damage, which is enough to enter the clip on a snapshot that is not changing; a slide
 * is exactly such a run of frames. It has to be drawn through the app, not
 * fb_render_snapshot(), because the app is what starts the slide.
 */
MESH_TEST_CASE(fb_transition_under_layer_matches_full_composition, unit) {
    struct inkcell_capture *clipped = NULL, *reference = NULL;
    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    const char *failure = NULL;
    if (snapshot == NULL || mesh_ui_capture_open(&clipped, 1024U, 768U, INKCELL_SCALE(4)) != 0 ||
        mesh_ui_capture_open(&reference, 1024U, 768U, INKCELL_SCALE(4)) != 0) {
        failure = "capture allocation failed";
        goto cleanup;
    }
    inkcell_capture_set_reference(reference, true);
    snapshot->nav.screen = MESH_UI_SCREEN_RADIO;
    snapshot->nav.devices_open = true;
    snprintf(snapshot->nav.toast.text, sizeof snapshot->nav.toast.text, "Pairing");
    snapshot->nav.toast.until_ms = 60000U;
    bool clip_seen = false;
    for (unsigned frame = 0U; frame < 60U; ++frame) {
        if (frame == 10U) {
            snapshot->nav.keyboard_open = true;
            snapshot->nav.keyboard_passkey = true;
        }
        if (frame == 35U) {
            snapshot->nav.keyboard_open = false;
            snapshot->nav.keyboard_passkey = false;
        }
        inkcell_capture_advance(clipped, 16U);
        inkcell_capture_advance(reference, 16U);
        inkcell_capture_render(clipped, snapshot);
        inkcell_capture_render(reference, snapshot);
        if (inkcell_capture_state(clipped)->clip_active) {
            clip_seen = true;
        }
        if (memcmp(inkcell_capture_pixels(clipped, NULL, NULL, NULL),
                   inkcell_capture_pixels(reference, NULL, NULL, NULL), 1024U * 768U * 4U) != 0) {
            failure = "a screen sliding in under a clip band drew over the screen it replaced";
            break;
        }
    }
    if (failure == NULL && !clip_seen) {
        failure = "comparison did not exercise partial drawing";
    }
cleanup:
    inkcell_capture_close(clipped);
    inkcell_capture_close(reference);
    free(snapshot);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The screen progress bar under the partial-composition clip.
 *
 * fb_animation_clip_matches_full_composition covers the two animated things a body can hold; the
 * bar is the first one that lives in the *chrome*, above `body_y`, and it runs on a snapshot that
 * is not changing - which is precisely the case the clip is entered on. It declares its damage
 * through inkcell_fb_draw_meter(), because it is one, so this is the check that the reuse is
 * enough: clipped and unclipped composition have to agree on every frame of the loop.
 */
MESH_TEST_CASE(fb_progress_clip_matches_full_composition, unit) {
    struct inkcell_draw_state state[2] = {0};
    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    const char *failure = NULL;
    unsigned clipped = 0U;
    if (snapshot == NULL) {
        record_failure(test_name, "snapshot allocation failed");
        return;
    }
    for (unsigned i = 0; i < 2U; ++i) {
        state[i].surface = ui_capture_surface(NULL, 1024U, 768U, 4096U);
        state[i].surface.pixels = calloc(1U, state[i].surface.size);
        if (state[i].surface.pixels == NULL) {
            failure = "frame allocation failed";
            goto cleanup;
        }
        inkcell_fb_state_set_theme(&state[i], inkcell_theme_default(), INKCELL_SCALE(4));
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
            inkcell_fb_state_set_now(&state[i], 1000U + frame * 16U);
            fb_render_snapshot(&state[i], snapshot);
        }
        if (state[0].clip_active) {
            clipped++;
        }
        if (memcmp(state[0].surface.pixels, state[1].surface.pixels, state[0].surface.size) != 0) {
            failure = "the clipped frame lost the progress bar, or what it travelled over";
            goto cleanup;
        }
    }
    if (clipped == 0U) {
        failure = "the bar declared no damage, so the comparison never exercised the clip";
    }
cleanup:
    for (unsigned i = 0U; i < 2U; ++i) {
        inkcell_fb_glyph_cache_free(&state[i]);
        fb_thread_cache_free(&state[i]);
        fb_render_cache_free(&state[i]);
        free(state[i].surface.pixels);
    }
    free(snapshot);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
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

    for (size_t t = 0; t < inkcell_theme_count() && failure == NULL; ++t) {
        const struct inkcell_theme *theme = inkcell_theme_at(t);
        for (int scale = INKCELL_SCALE_MIN; scale <= INKCELL_SCALE_MAX && failure == NULL;
             scale += INKCELL_SCALE(1)) {
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
                const uint32_t body_top = capture_body_top(height);
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

    for (size_t t = 0; t < inkcell_theme_count() && failure == NULL; ++t) {
        const struct inkcell_theme *theme = inkcell_theme_at(t);
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
                frames[pass] =
                    capture_frame(&store, theme, INKCELL_SCALE(4), &width, &height, &stride);
                if (frames[pass] == NULL) {
                    failure = "capture failed";
                }
            }
            mesh_ui_store_shutdown(&store);
        }

        if (failure == NULL) {
            const uint32_t body_top = capture_body_top(height);
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

    for (size_t t = 0; t < inkcell_theme_count() && failure == NULL; ++t) {
        const struct inkcell_theme *theme = inkcell_theme_at(t);
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
            frame = capture_frame(&store, theme, INKCELL_SCALE(4), &width, &height, &stride);
            if (frame == NULL) {
                failure = "capture failed";
            }
        }
        if (failure == NULL) {
            const uint32_t ink = rgb_key(inkcell_theme_tone(theme, INKCELL_TONE_PRIMARY));
            const uint32_t ground = rgb_key(inkcell_theme_color(theme, INKCELL_COLOR_BG));
            const uint32_t body_top = capture_body_top(height);
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

    const int scale =
        INKCELL_SCALE(4); /* what the Brick draws at, and what the bands below are true for */
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
    const uint32_t column_from = INKCELL_CAPTURE_WIDTH / 3U;
    const uint32_t column_to = (INKCELL_CAPTURE_WIDTH * 2U) / 3U;

    struct inkcell_capture *capture = NULL;
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

            if (mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT,
                                     scale) != 0) {
                failure = "capture open failed";
                break;
            }
            inkcell_capture_set_theme(capture, inkcell_theme_at(0));
            inkcell_capture_set_scale(capture, scale);
            uint32_t width = 0U, height = 0U;
            size_t stride = 0U;
            const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
            inkcell_capture_render(capture, snapshot);

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
            inkcell_capture_close(capture);
            capture = NULL;
        }
    }

    if (capture != NULL) {
        inkcell_capture_close(capture);
    }
    free(reference);
    free(snapshot);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
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
    const int scale = INKCELL_SCALE(4);

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

    struct inkcell_capture *capture = NULL;
    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;

    /* The same frame with no pack open, which is what the chrome is compared against. */
    if (mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT, scale) != 0) {
        failure = "capture open failed";
    } else {
        inkcell_capture_set_theme(capture, inkcell_theme_at(0));
        inkcell_capture_set_scale(capture, scale);
        const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
        inkcell_capture_render(capture, snapshot);
        bare = malloc((size_t)height * stride);
        if (bare == NULL || pixels == NULL) {
            failure = "frame allocation failed";
        } else {
            memcpy(bare, pixels, (size_t)height * stride);
        }
        if (inkcell_capture_animating(capture)) {
            failure = "a map with no pack asks for another frame";
        }
        inkcell_capture_close(capture);
        capture = NULL;
    }

    size_t after_one = 0U;
    size_t settled = 0U;
    unsigned frames = 0U;
    if (failure == NULL &&
        mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT, scale) != 0) {
        failure = "capture open failed";
    } else if (failure == NULL) {
        inkcell_capture_set_theme(capture, inkcell_theme_at(0));
        inkcell_capture_set_scale(capture, scale);
        if (mesh_ui_capture_open_map_pack(capture, pack.path) != 0) {
            failure = "the capture would not open the fixture pack";
        }
    }

    if (failure == NULL) {
        const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
        inkcell_capture_render(capture, snapshot);
        after_one = count_fixture_tiles(pixels, width, height, stride, tiles);

        /* Settled: frames until it stops asking for more, with a ceiling well above the tiles a
           view can stand on so a loop that never stopped is a failure rather than a hang. */
        for (frames = 1U; frames < 200U && inkcell_capture_animating(capture); ++frames) {
            inkcell_capture_advance(capture, 33U);
            inkcell_capture_render(capture, snapshot);
        }
        settled = count_fixture_tiles(pixels, width, height, stride, tiles);

        if (inkcell_capture_animating(capture)) {
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
        const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
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
        inkcell_capture_close(capture);
    }
    mesh_test_map_pack_remove(&pack);
    free(bare);
    free(snapshot);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
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
    const int scale = INKCELL_SCALE(4);

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
    struct inkcell_capture *capture = NULL;
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
        mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT, scale) != 0) {
        failure = "capture open failed";
    }
    if (failure == NULL && mesh_ui_capture_open_map_pack(capture, pack.path) != 0) {
        failure = "the capture would not open the fixture pack";
    }
    if (failure == NULL) {
        inkcell_capture_render(capture, snapshot);
        if (!inkcell_capture_animating(capture)) {
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
        for (; frames < 60U && inkcell_capture_animating(capture); ++frames) {
            inkcell_capture_advance(capture, 33U);
            inkcell_capture_render(capture, snapshot);
        }
        if (inkcell_capture_animating(capture)) {
            failure = "a screen with no map on it went on asking for frames";
        }
    }

    if (capture != NULL) {
        inkcell_capture_close(capture);
    }
    mesh_test_map_pack_remove(&pack);
    free(snapshot);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
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
    const int scale = INKCELL_SCALE(4);

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
    struct inkcell_capture *capture = NULL;
    if (snapshot == NULL) {
        failure = "snapshot allocation failed";
    } else {
        snapshot->nav.screen = MESH_UI_SCREEN_NODES;
        snapshot->nav.map_open = true;
        snapshot->handshake_valid = true;
        mesh_map_viewport_init(&snapshot->nav.map_viewport, latitude_i, longitude_i, zoom);
    }

    if (failure == NULL &&
        mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT, scale) != 0) {
        failure = "capture open failed";
    }
    if (failure == NULL) {
        inkcell_capture_set_theme(capture, inkcell_theme_at(0));
        inkcell_capture_set_scale(capture, scale);
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);

        if (mesh_ui_capture_open_map_pack(capture, first.path) != 0) {
            failure = "the capture would not open the first pack";
        } else {
            for (unsigned frame = 0U; frame < 200U; ++frame) {
                inkcell_capture_render(capture, snapshot);
                if (!inkcell_capture_animating(capture)) {
                    break;
                }
                inkcell_capture_advance(capture, 33U);
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
                inkcell_capture_render(capture, snapshot);
                if (!inkcell_capture_animating(capture)) {
                    break;
                }
                inkcell_capture_advance(capture, 33U);
            }
            if (count_pack_tiles(pixels, width, height, stride, tiles, 7U) == 0U) {
                failure = "the second pack did not reach the panel";
            } else if (count_pack_tiles(pixels, width, height, stride, tiles, 0U) != 0U) {
                failure = "the first pack's tiles survived the swap";
            }
        }
    }

    if (capture != NULL) {
        inkcell_capture_close(capture);
    }
    mesh_test_map_pack_remove(&first);
    mesh_test_map_pack_remove(&second);
    free(snapshot);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
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

    snapshot->nav.screen = MESH_UI_SCREEN_RADIO;

    snapshot->nav.devices_open = false;
    snapshot->nav.trend_open = true;
    snapshot->handshake_valid = true;

    const int scale =
        INKCELL_SCALE(4); /* what the Brick draws at, and what the bands below are true for */
    const uint32_t top_band = 96U;
    const uint32_t bottom_band = 56U;

    struct inkcell_capture *capture = NULL;
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

        if (mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT, scale) !=
            0) {
            failure = "capture open failed";
            break;
        }
        inkcell_capture_set_theme(capture, inkcell_theme_at(0));
        inkcell_capture_set_scale(capture, scale);
        uint32_t width = 0U, height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
        inkcell_capture_render(capture, snapshot);

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
        inkcell_capture_close(capture);
        capture = NULL;
    }

    if (failure == NULL && !body_moved) {
        failure = "the two readings drew the same picture, so nothing was tested";
    }

    if (capture != NULL) {
        inkcell_capture_close(capture);
    }
    free(reference);
    free(snapshot);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
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
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    }

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                                   INKCELL_CAPTURE_HEIGHT, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);

    /*
     * Wide enough that a glyph stroke cannot produce it, and narrow enough for a single figure
     * at the smallest scale a theme picks.
     *
     * A run of the colour itself rather than of the capsule, which is not the same number: the
     * pill is drawn anti-aliased, so each of its scanlines fades into the strip at both ends
     * and the solid part of the widest one is a couple of pixels short of the shape. This was
     * twenty and the shape is twenty-one, which left the check passing on the strength of the
     * two pixels a smoother edge then spent. A stroke at this scale is four.
     */
    const unsigned capsule = 12U;
    /* The tab strip and nothing below it, measured rather than estimated: the focus ring on the
       body's first row sits a hairline under the strip's rule and is drawn in the same primary a
       badge is, so an eighth of the panel - which this was - reached far enough to count it. */
    const struct inkcell_draw_state *const drawn = inkcell_capture_state(capture);
    const uint32_t strip = (uint32_t)inkcell_fb_nav_bar_height(
        drawn, inkcell_theme_type_scale(drawn->theme, INKCELL_TYPE_LABEL, drawn->scale));

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    inkcell_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(topmost_row_run(capture, pixels, width, height, stride,
                                              INKCELL_COLOR_PRIMARY, capsule) >= strip,
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "unread messages should badge the Messages tab from another tab");

    /* Muting every conversation empties the total, and the badge goes with it - which is the
       press's whole promise, and the reason the total is what the strip reads. */
    (void)mesh_ui_store_set_conversation_mute(&store, (uint8_t)MESH_UI_CONVERSATION_CHANNEL,
                                              MESH_MESSAGE_BROADCAST_ADDR, 0U, true);
    (void)mesh_ui_store_set_conversation_mute(&store, (uint8_t)MESH_UI_CONVERSATION_DIRECT, 0x3000U,
                                              0U, true);
    MESH_TEST_FAIL_IF_CLEANUP(
        mesh_ui_nav_unread_total(&store) != 0U, inkcell_capture_close(capture);
        mesh_ui_store_shutdown(&store), "the fixture's two conversations should both now be muted");
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    inkcell_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(topmost_row_run(capture, pixels, width, height, stride,
                                              INKCELL_COLOR_PRIMARY, capsule) < strip,
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "a muted mesh should leave the tab strip unbadged");

    inkcell_capture_close(capture);
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
 * A pixel of a card's hairline edge, as the edge is actually drawn.
 *
 * The rule is a hairline and inkcell anti-aliases it into whatever is on either side of it, so
 * on a given row there may be no pixel of the outline colour itself anywhere - only the blend
 * running up to it and back down. An edge asked for by equality is an edge this reads as absent.
 *
 * The two fills are named out rather than measured, and that is the one thing this cannot do
 * without: a cursor row's fill is a state layer of the text colour over the surface, which
 * lands *between* the ground and the rule on every channel in every theme. Admitted by the
 * blend test it would answer this question with the very thing the question is about.
 */
static bool pixel_is_card_edge(const struct inkcell_capture *capture, const uint8_t *pixel) {
    if (pixel_is_role(capture, pixel, INKCELL_COLOR_OUTLINE)) {
        return true;
    }
    /*
     * The three colours the blend test below would otherwise swallow, named out rather than
     * measured. The ground is an endpoint of the range and so matches it trivially; the card's
     * fill and a cursor row's fill both sit between the ground and the rule on every channel in
     * every theme - a cursor row because it is a state layer of the text colour over the
     * surface. Any of them admitted, this answers "the card's edge" with the things the edge is
     * drawn between.
     */
    if (pixel_is_role(capture, pixel, INKCELL_COLOR_BG) ||
        pixel_is_role(capture, pixel, INKCELL_COLOR_SURFACE_SEL) ||
        pixel_is_role(capture, pixel, INKCELL_COLOR_SURFACE)) {
        return false;
    }
    const struct inkcell_theme *theme = inkcell_capture_theme(capture);
    const struct inkcell_rgb outline = inkcell_theme_color(theme, INKCELL_COLOR_OUTLINE);
    return pixel_between(pixel, outline, inkcell_theme_color(theme, INKCELL_COLOR_BG)) ||
           pixel_between(pixel, outline, inkcell_theme_color(theme, INKCELL_COLOR_SURFACE));
}

/*
 * A pixel that belongs to a card's own furniture: the ground, the card, a cursor row, the rule
 * between them, or an anti-aliased step between any two of those.
 *
 * The blends are the whole reason this is a function. A rounded card is drawn by blending each
 * of these into the one behind it, so a row through a corner is mostly steps and only partly
 * colours - and a check that named four colours and rejected everything else read every one of
 * those steps as a heading drawn through the card's edge.
 */
static bool pixel_is_one_of(const struct inkcell_capture *capture, const uint8_t *pixel,
                            const enum inkcell_color *roles, size_t count) {
    const struct inkcell_theme *theme = inkcell_capture_theme(capture);
    for (size_t i = 0U; i < count; ++i) {
        if (pixel_is_role(capture, pixel, roles[i])) {
            return true;
        }
        for (size_t j = i + 1U; j < count; ++j) {
            if (pixel_between(pixel, inkcell_theme_color(theme, roles[i]),
                              inkcell_theme_color(theme, roles[j]))) {
                return true;
            }
        }
    }
    return false;
}

static bool pixel_is_card_furniture(const struct inkcell_capture *capture, const uint8_t *pixel) {
    static const enum inkcell_color k_roles[] = {INKCELL_COLOR_OUTLINE, INKCELL_COLOR_SURFACE,
                                                 INKCELL_COLOR_SURFACE_SEL, INKCELL_COLOR_BG};
    return pixel_is_one_of(capture, pixel, k_roles, sizeof k_roles / sizeof k_roles[0]);
}

/*
 * A group's heading is drawn after the cards, so anything of it that lands on a card's edge
 * paints through the edge - and the scale it happens at is the one nobody renders.
 *
 * At INKCELL_SCALE_MIN the type scale clamps the label onto the body, because a label cannot be
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
static const char *card_edges_carry_no_ink(const struct inkcell_capture *capture,
                                           const uint8_t *pixels, uint32_t width, uint32_t height,
                                           size_t stride, int scale) {
    /* The rail's gutter is half a margin, and the margin does not scale - see fb_list_rail(). */
    const uint32_t right = width > 16U ? width - 16U : width;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        uint32_t edge = 0U;
        for (uint32_t x = 0U; x < right; ++x) {
            if (pixel_is_role(capture, row + (size_t)x * 4U, INKCELL_COLOR_OUTLINE)) {
                edge++;
            }
        }
        /* A card runs nearly the whole panel, so its edge rows are the only ones that can be
           mostly outline. Nothing else on the frame draws a rule in that role - and the colour
           itself rather than pixel_is_card_edge() picks the row, because the blends that
           function also accepts are what the tab strip's own fills are made of. */
        if (edge * 5U < right * 3U) {
            continue;
        }
        for (uint32_t x = 0U; x < right; ++x) {
            if (pixel_is_card_furniture(capture, row + (size_t)x * 4U)) {
                continue;
            }
            (void)scale;
            return "a group's heading drew through the edge of the card under it";
        }
    }
    return NULL;
}

/*
 * And the other half of that question: the gap between two cards keeps its clearance.
 *
 * card_edges_carry_no_ink() asks whether anything is drawn *through* a hairline, which is the
 * failure that corrupts the frame - and it is blind to the one that merely looks wrong. A
 * heading's disc sized to the break it stands in comes out exactly as tall as the break and
 * lands with its crown on the hairline above it and its foot on the hairline below: nothing
 * overlaps, every scanline is legal, and the result reads as a bead wedged between two panels
 * rather than as the header of the card under it.
 *
 * So this asks the *gap* instead, and only the gap. A scanline of the break is one that is
 * mostly body ground; one within a clearance step of a card's edge is the break's own margin,
 * and nothing wearing a family's container - which is what every disc and every filled chip is
 * drawn in - may be on it. The step comes from the theme, so the bar means the same thing at
 * every glyph scale, and it is the step the heading's disc insets itself by.
 *
 * Deliberately not asked of the rows *inside* a card. A row's own disc is as tall as its row and
 * the card's padding is what separates the last one from the edge - a different measurement,
 * made by a different component, and holding it to this one would be this test having an opinion
 * about how much padding a card owes its contents.
 */
static const char *card_edges_keep_their_clearance(const struct inkcell_capture *capture,
                                                   const uint8_t *pixels, uint32_t width,
                                                   uint32_t height, size_t stride, int scale) {
    const struct inkcell_theme *theme = inkcell_capture_theme(capture);
    const int clear = inkcell_theme_space(theme, INKCELL_SPACE_SM, scale);
    /* The rail's gutter is outside every card, exactly as it is outside the check above. */
    const uint32_t right = width > 16U ? width - 16U : width;
    if (clear <= 0) {
        return NULL;
    }
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        uint32_t ground = 0U;
        for (uint32_t x = 0U; x < right; ++x) {
            if (pixel_is_role(capture, row + (size_t)x * 4U, INKCELL_COLOR_BG)) {
                ground++;
            }
        }
        /* A scanline of the break. A card's own rows are mostly its fill, so they never qualify
           however much ground a short value leaves at the end of one. */
        if (ground * 5U < right * 3U) {
            continue;
        }
        bool near_edge = false;
        for (int step = 1; step <= clear && !near_edge; ++step) {
            const int ys[2] = {(int)y - step, (int)y + step};
            for (size_t which = 0U; which < 2U && !near_edge; ++which) {
                if (ys[which] < 0 || ys[which] >= (int)height) {
                    continue;
                }
                const uint8_t *other = pixels + (size_t)ys[which] * stride;
                uint32_t edge = 0U;
                for (uint32_t x = 0U; x < right; ++x) {
                    if (pixel_is_role(capture, other + (size_t)x * 4U, INKCELL_COLOR_OUTLINE)) {
                        edge++;
                    }
                }
                near_edge = edge * 5U >= right * 3U;
            }
        }
        if (!near_edge) {
            continue;
        }
        for (uint32_t x = 0U; x < right; ++x) {
            const uint8_t *px = row + (size_t)x * 4U;
            for (int f = 0; f < INKCELL_FAMILY_COUNT; ++f) {
                if (pixel_is_role(
                        capture, px,
                        inkcell_family_role((enum inkcell_family)f, INKCELL_SLOT_CONTAINER))) {
                    return "a heading's disc is up against the edge of a card";
                }
            }
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
static const char *rail_clears_the_cards(const struct inkcell_capture *capture,
                                         const uint8_t *pixels, uint32_t width, uint32_t height,
                                         size_t stride, bool *saw_rail) {
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        /* The card's own outer edge: its hairline or its focus ring, or its fill on the ends the
           window cut. The cursor's own fill is inside all of them and so is never the rightmost. */
        int card_right = -1;
        bool on_card = false;
        for (uint32_t x = 0U; x < width; ++x) {
            const uint8_t *px = row + (size_t)x * 4U;
            const bool surface = pixel_is_role(capture, px, INKCELL_COLOR_SURFACE);
            on_card = on_card || surface;
            /* The ring only counts past a card's fill: the accent is also the tab strip's. */
            if (surface || pixel_is_role(capture, px, INKCELL_COLOR_OUTLINE) ||
                (on_card && pixel_is_role(capture, px, INKCELL_COLOR_PRIMARY))) {
                card_right = (int)x;
            }
        }
        /*
         * And the tail of the card's own anti-aliased edge, which is where the card really
         * ends: the last pixel of the outline *colour* - or of the focus ring's - is somewhere
         * inside the hairline rather than at its outer face, so a gutter measured from there is
         * measured from inside the card and comes back two pixels short of what the layout
         * left.
         *
         * The tail is whatever fades from that last colour into the ground, walked forward from
         * it one pixel at a time. Adjacency is what makes this safe: it can only ever be this
         * shape's own fade-out, because the rail on the far side of the gutter has clear ground
         * between it and anything here.
         */
        /* The card's own colours and the ring's, and the steps between any two of them: that is
           what the edge fading out is made of and it is all this may absorb. It stops at the
           first pixel of clear ground, so the rail on the far side of the gutter is out of
           reach whatever it is drawn in. */
        static const enum inkcell_color k_card[] = {INKCELL_COLOR_OUTLINE, INKCELL_COLOR_SURFACE,
                                                    INKCELL_COLOR_SURFACE_SEL,
                                                    INKCELL_COLOR_PRIMARY, INKCELL_COLOR_BG};
        while (card_right >= 0 && (uint32_t)card_right + 1U < width) {
            const uint8_t *next = row + (size_t)(card_right + 1) * 4U;
            if (pixel_is_background(capture, next) ||
                !pixel_is_one_of(capture, next, k_card, sizeof k_card / sizeof k_card[0])) {
                break;
            }
            card_right += 1;
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
             * layout leaves the rail centred in inkcell_fb_rail_gutter()'s strip, which is several
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
static const char *cursor_stays_inside_its_card(const struct inkcell_capture *capture,
                                                const uint8_t *pixels, uint32_t width,
                                                uint32_t height, size_t stride) {
    uint32_t widest = 0U;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        uint32_t run = 0U;
        for (uint32_t x = 0U; x < width; ++x) {
            run = pixel_is_role(capture, row + (size_t)x * 4U, INKCELL_COLOR_SURFACE_SEL) ? run + 1U
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
            if (pixel_is_role(capture, row + (size_t)x * 4U, INKCELL_COLOR_SURFACE_SEL)) {
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
            left = left || pixel_is_card_edge(capture, row + (size_t)x * 4U);
        }
        for (uint32_t x = last + 1U; x < width; ++x) {
            right = right || pixel_is_card_edge(capture, row + (size_t)x * 4U);
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
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    }
    /* Past the filter and map rows, then past our own node, onto one that is not us - which is
       the one with enough reported about it to outgrow the window and put a rail up. */
    for (uint32_t step = 0; step < MESH_UI_NODES_LEAD_ROWS + 1U; ++step) {
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
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
    for (int scale = INKCELL_SCALE_MIN; scale <= INKCELL_SCALE_MAX && failure == NULL;
         scale += INKCELL_SCALE(1)) {
        struct inkcell_capture *capture = NULL;
        if (mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT, scale) !=
            0) {
            failure = "capture open failed";
            break;
        }
        inkcell_capture_set_scale(capture, scale);

        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
        inkcell_capture_render(capture, &snapshot);
        pixels = capture_body(capture, pixels, &width);

        const unsigned fill =
            widest_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_SURFACE);
        const unsigned edge =
            widest_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_OUTLINE);
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
            failure =
                card_edges_keep_their_clearance(capture, pixels, width, height, stride, scale);
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

        inkcell_capture_close(capture);
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
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
        mesh_ui_store_request_refresh(&store);
        if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
            failure = "no snapshot after walking the node detail";
            break;
        }
        struct inkcell_capture *capture = NULL;
        if (mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT, 0) != 0) {
            failure = "capture open failed";
            break;
        }
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
        inkcell_capture_render(capture, &snapshot);
        /* Down now stops on a card of facts as a whole, and a card the cursor stands on draws
           no row fill at all - so the question for that press is whether the ring is there. */
        const struct mesh_ui_handshake_state *hs = &snapshot.handshake;
        const struct mesh_ui_node_summary *node =
            mesh_ui_node_detail_find(hs, snapshot.nav.node_detail_node);
        struct mesh_ui_node_item items[MESH_UI_NODE_ITEMS_MAX];
        const uint32_t count = mesh_ui_node_detail_build(
            node, hs->has_my_info && node != NULL && node->node_id == hs->my_info.node_num, 0U,
            mesh_ui_snapshot_traceroute_view(&snapshot, node != NULL ? node->node_id : 0U), hs,
            &snapshot.history, false, items, MESH_UI_NODE_ITEMS_MAX);
        struct mesh_ui_node_span span = {0};
        const bool on_card =
            mesh_ui_node_detail_span(items, count, inkcell_capture_page_rows(capture),
                                     snapshot.nav.cursor[MESH_UI_SCREEN_NODES], &span) &&
            span.card;
        const char *broke = NULL;
        if (!on_card) {
            broke = cursor_stays_inside_its_card(capture, pixels, width, height, stride);
        } else if (widest_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_PRIMARY) <
                   80U) {
            broke = "a card the cursor stands on draws no focus ring";
        }
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
        inkcell_capture_close(capture);
    }

    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    MESH_TEST_FAIL_IF(!rail_seen, "no scroll rail beside the node detail at any glyph scale");
    record_success(test_name);
}

/*
 * How many separate horizontal *bands* of `role` the frame carries: runs at least `min_run`
 * wide, counted once per unbroken span of scanlines that hold one.
 *
 * Bands rather than pixels, and bands rather than a single widest run, because both of the
 * cheaper answers are wrong here. A pixel count cannot tell a filled disc from the partial
 * coverage around a glyph that happens to be drawn in the same role; a widest run cannot tell
 * ten discs down a card from the one pill the navigation bar draws in the primary's container
 * behind the tab you are on. A column of discs is a column of bands, and nothing else on the
 * frame is.
 */
static uint32_t bands_of_rgb(const uint8_t *pixels, uint32_t width, uint32_t height, size_t stride,
                             struct inkcell_rgb want, uint32_t min_run) {
    uint32_t bands = 0U;
    bool inside = false;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        uint32_t run = 0U;
        bool wide = false;
        for (uint32_t x = 0U; x < width; ++x) {
            const uint8_t *px = row + (size_t)x * 4U;
            run = (px[0] == want.b && px[1] == want.g && px[2] == want.r) ? run + 1U : 0U;
            wide = wide || run >= min_run;
        }
        if (wide && !inside) {
            bands++;
        }
        inside = wide;
    }
    return bands;
}

static uint32_t bands_of(const struct inkcell_capture *capture, const uint8_t *pixels,
                         uint32_t width, uint32_t height, size_t stride, enum inkcell_color role,
                         uint32_t min_run) {
    return bands_of_rgb(pixels, width, height, stride,
                        inkcell_theme_color(inkcell_capture_theme(capture), role), min_run);
}

/*
 * A card of verbs wears its colour in the discs, not in the words.
 *
 * The screen this replaced drew every action row's label in the primary: eleven rows of one
 * colour, which is not eleven emphases but a card with none in it - and the one row that deletes
 * something had to shout over ten rows already shouting. The accent did not go away, it moved to
 * a container at each row's leading edge, where the eye finds what a row is *about* without the
 * words competing with it.
 *
 * Asked as two runs rather than as one, because either alone passes on a renderer that has got
 * half of it wrong. That the primary's container is laid down at all says the discs are drawn;
 * that the *error* family's is laid down as well says each disc reads its own row's tone rather
 * than a colour the screen picked once - which is the whole of why INKCELL_FB_LEADING_TONAL takes
 * no family, and it is the row that would be missed if it did, since Remove is the only one on the
 * card that is not the default.
 *
 * A run rather than a pixel count: a glyph drawn in either role contributes a scattering of
 * partial coverage, and a disc is a solid block wider than a cell. Held to the glyph scale so
 * the bar means the same thing at every size the themes allow.
 */
MESH_TEST_CASE(ui_capture_node_detail_verbs_wear_their_colour_in_a_disc, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    while (store.nav.screen != MESH_UI_SCREEN_NODES) {
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    }
    /* Past the lead rows - the filter, the sort and the map row - and then one node further, onto
       somebody who is not us. Counted from the constant rather than written out, because a
       fourth lead row would otherwise leave this walking onto our own node and failing with a
       message about discs. */
    for (uint32_t step = 0; step < MESH_UI_NODES_LEAD_ROWS + 1U; ++step) {
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.node_detail_open, mesh_ui_store_shutdown(&store),
                              "A should open the node detail");
    /* And again, onto the sheet the verbs live on. The detail below it holds exactly one of
       them - the row this press came out of - so it is the wrong screen to ask this of. */
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.node_actions_open, mesh_ui_store_shutdown(&store),
                              "the detail's first row should open the node's verbs");

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_store_shutdown(&store), "no snapshot to render");

    /*
     * Walked onto the destructive row, because a card of verbs is taller than this panel at the
     * larger glyph scales and the one that is not the default is near the bottom of it. Found by
     * building the rows rather than by counting presses: which verbs a node offers depends on
     * what it has reported, so a fixed number of downs is a test that passes while pointing at
     * the wrong row.
     */
    const struct mesh_ui_node_summary *node =
        mesh_ui_node_detail_find(&snapshot.handshake, snapshot.nav.node_detail_node);
    MESH_TEST_FAIL_IF_CLEANUP(node == NULL, mesh_ui_store_shutdown(&store),
                              "the open node is not in the roster");
    /* Our own node offers no verb that costs anything - it cannot be messaged, ignored or
       removed - so this test would have nothing to look at on it. */
    MESH_TEST_FAIL_IF_CLEANUP(snapshot.handshake.has_my_info &&
                                  node->node_id == snapshot.handshake.my_info.node_num,
                              mesh_ui_store_shutdown(&store),
                              "the walk landed on our own node rather than on somebody else's");
    struct mesh_ui_node_item items[MESH_UI_NODE_ACTIONS_MAX];
    const uint32_t count =
        mesh_ui_node_actions_build(node, false, NULL, false, 0U, items, MESH_UI_NODE_ACTIONS_MAX);
    uint32_t remove_row = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].action == (uint8_t)MESH_UI_NODE_ACTION_REMOVE) {
            remove_row = i;
        }
    }
    MESH_TEST_FAIL_IF_CLEANUP(remove_row == count, mesh_ui_store_shutdown(&store),
                              "the fixture's node offers no row to remove it");
    const char *failure = NULL;
    static char detail[192];
    for (int scale = INKCELL_SCALE_MIN; scale <= INKCELL_SCALE_MAX && failure == NULL;
         scale += INKCELL_SCALE(1)) {
        struct inkcell_capture *capture = NULL;
        if (mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT, scale) !=
            0) {
            failure = "capture open failed";
            break;
        }
        inkcell_capture_set_scale(capture, scale);
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
        inkcell_capture_render(capture, &snapshot);

        /*
         * The window this panel has, handed back to the store, and then the walk down to the
         * destructive row - both inside the scale loop because a taller glyph is a shorter
         * window and the nav pages a tall card by it. Up and Down on this screen walk *stops*
         * and ask the store for that window; without it no press moves at all, which is the
         * shape of a test that renders the top of the card forever.
         */
        mesh_ui_store_set_page_rows(&store, inkcell_capture_page_rows(capture));
        for (uint32_t guard = 0U; guard <= count; ++guard) {
            if (snapshot.nav.node_actions_cursor >= remove_row) {
                break;
            }
            (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
            mesh_ui_store_request_refresh(&store);
            (void)mesh_ui_store_consume_updates(&store, &snapshot);
        }
        /* Until the sheet has arrived. It is a layer now rather than a screen, so its first
           frame is a panel still off the bottom edge and what this would otherwise count is the
           detail underneath it - seven accent discs and nothing destructive, which is exactly
           what the detail has. */
        render_until_still(capture, &snapshot);

        /* Wider than a glyph's own strokes at this size and well inside a disc, which is as
           wide as the row is tall. */
        const uint32_t min_run = (uint32_t)inkcell_scale_px(4, scale);
        const uint32_t accent = bands_of(capture, pixels, width, height, stride,
                                         INKCELL_COLOR_PRIMARY_CONTAINER, min_run);
        /*
         * The destructive one is looked for under the cursor, because that is where the walk
         * above leaves it and because the cursor's row is the one row guaranteed to be on the
         * panel at every glyph scale - at the largest, this card is taller than the window and a
         * row stepped off is a row that has scrolled away.
         *
         * The sheet is a menu set with the accent cursor, which keeps the row's own inks - so
         * the disc under the cursor is in the family's container, as it is on every other row.
         * The base is looked for as well, for a theme or a list that lifts the cursor's row with
         * a fill: there a disc commits to full strength, because a container and that fill are
         * both quiet fills on the body ground and therefore near each other. Either way it is
         * the *error* family, which is what the case is about. The marker capsule down the row
         * is one scale wide - narrower than `min_run` by construction - so what this finds is
         * the disc.
         */
        const uint32_t danger =
            bands_of(capture, pixels, width, height, stride, INKCELL_COLOR_ERROR_CONTAINER,
                     min_run) +
            bands_of(capture, pixels, width, height, stride, INKCELL_COLOR_ERROR, min_run);
        /* Four, because one of the accent's bands is the navigation bar's own tab pill and the
           sheet this is about holds several verbs. The sheet's own heading is a title and a
           trailing word rather than a row, so no disc is counted for it, and four still clears
           the verbs by a wide margin. */
        if (accent < 4U || danger < 1U) {
            snprintf(detail, sizeof detail,
                     "the node's verbs draw no tonal disc (%u accent bands, %u destructive, "
                     "at glyph scale %d)",
                     accent, danger, scale);
            failure = detail;
        }
        inkcell_capture_close(capture);
    }

    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * Scanlines on which `first` ink appears and no `second` ink stands to the left of it.
 *
 * The question a two-tier row answers in pixels, and it is asked in both directions. A row that
 * draws its label column and its value in one ink puts one colour across the whole line; a row
 * that draws them as two pieces puts one ink in the label column and the other after it, in that
 * order, on every scanline the glyphs' cores reach.
 *
 * Ordered rather than merely both-present, because both-present is satisfied by a screen that
 * happens to carry the two inks anywhere at all - a trailing age beside a name, which is most of
 * the lists in this client. The label column is what is being pinned, and what makes it the
 * label column is that it comes first.
 *
 * Glyph cores only: a glyph carries coverage, so its edges are blends of the ink and the ground
 * and match no role exactly. That is the whole of why this counts scanlines rather than pixels -
 * a run of them is a row of text, and one is a stray antialiased hit.
 */
static unsigned scanlines_led_by(const struct inkcell_capture *capture, const uint8_t *pixels,
                                 uint32_t width, uint32_t height, size_t stride,
                                 enum inkcell_color first, enum inkcell_color second) {
    unsigned rows = 0U;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        bool saw_first = false;
        bool saw_second = false;
        bool second_led = false;
        for (uint32_t x = 0U; x < width; ++x) {
            const uint8_t *pixel = row + (size_t)x * 4U;
            if (pixel_is_role(capture, pixel, first)) {
                saw_first = true;
            } else if (pixel_is_role(capture, pixel, second)) {
                saw_second = true;
                second_led = second_led || !saw_first;
            }
        }
        if (saw_first && saw_second && !second_led) {
            rows++;
        }
    }
    return rows;
}

MESH_TEST_CASE(ui_capture_node_detail_states_its_labels_quietly, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    /* Walked rather than assigned, the rest of this suite's rule: a test that set the nav by
       hand would keep passing while the presses that get a user here stopped working. */
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    while (store.nav.screen != MESH_UI_SCREEN_NODES) {
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    }
    /* Past the lead rows - the filter, the sort and the map row - onto the first node, counted from
       the constant for the reason the case above counts it. */
    for (uint32_t step = 0; step < MESH_UI_NODES_LEAD_ROWS; ++step) {
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_A, &action);
    MESH_TEST_FAIL_IF_CLEANUP(!store.nav.node_detail_open, mesh_ui_store_shutdown(&store),
                              "A should open the node detail");
    /* Down past the verbs, which are plain rows and say nothing about a label column. Far
       enough that the window is showing facts whichever groups this node turns out to have. */
    for (unsigned step = 0U; step < 12U; ++step) {
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_store_shutdown(&store), "no snapshot to render");

    const char *failure = NULL;
    static char detail[160];
    for (int scale = INKCELL_SCALE_MIN; scale <= INKCELL_SCALE_MAX && failure == NULL;
         scale += INKCELL_SCALE(1)) {
        struct inkcell_capture *capture = NULL;
        if (mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT, scale) !=
            0) {
            failure = "capture open failed";
            break;
        }
        inkcell_capture_set_scale(capture, scale);
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
        inkcell_capture_render(capture, &snapshot);

        /* More than one, so a single antialiased hit cannot pass for a row: the smallest glyph
           this ships draws its cores over several scanlines, and the screen is a column of
           these rows rather than one of them. */
        const unsigned rows = scanlines_led_by(capture, pixels, width, height, stride,
                                               INKCELL_COLOR_TEXT_DIM, INKCELL_COLOR_TEXT);
        if (rows < 2U) {
            snprintf(detail, sizeof detail,
                     "the node detail draws its labels in the value's own ink (%u two-tier "
                     "scanlines at glyph scale %d)",
                     rows, scale);
            failure = detail;
        }
        inkcell_capture_close(capture);
    }

    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A row that states something about itself states it across both of its halves.
 *
 * The other side of the two-tier rule, and the case splitting the headline got wrong first. A
 * settings row's `tone` is a fact about the *row*: an unsaved field is INKCELL_TONE_STRONG and a
 * section the radio has not answered for is INKCELL_TONE_DIM, and while the headline was one
 * composed string both halves took it for free. Split into pieces with the label's tier spelled
 * as a tone of its own, the label fell to the zero - INKCELL_TONE_NORMAL - so an unsaved field
 * kept a strong value beside an ordinary name and an unloaded section read as available. The
 * flag replaced the tone for that reason: a row that says nothing here keeps what it had.
 *
 * Asked as two frames rather than as an ink, and the failed attempts are why. Matching
 * INKCELL_COLOR_TEXT_STRONG in the label column passes on the broken code, because the roles are
 * compared exactly and the dark palette draws TEXT_STRONG, TEXT_ON_SEL and a switch's knob in
 * one pure white - so the *cursor's own row* supplies the strong ink wherever it stands. Asking
 * it across the whole frame is worse: the scroll rail is dim ink at the panel's edge, so every
 * ordinary row on a scrolling list reads as a label that lost its tone.
 *
 * So the question is what an edit *changes*, in the one strip where only the label lives. A
 * toggle rather than a number, because a number's row carries a slider across its full width and
 * that bar reaches into the label column - it would answer this question by itself. On a toggle,
 * everything an edit touches (the value's word, the marker's dot, the switch, the app bar's
 * badge) is to the right of the boundary, so the label's own ink is all that is left to move.
 *
 * The cursor sits one row below the edited one in both frames, because a selected row draws
 * against the highlight in TEXT_ON_SEL whatever its tone - on the row it is standing on, a
 * tone is not on the panel to be read at all.
 */
MESH_TEST_CASE(ui_capture_settings_marks_both_halves_of_an_unsaved_row, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    /* A section the radio has answered for, or it holds no editable field to dirty. */
    struct mesh_ui_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.loaded = true;
    settings.has_display = true;
    settings.screen_on_secs = 60U;
    mesh_ui_store_set_settings(&store, &settings);

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    while (store.nav.screen != MESH_UI_SCREEN_SETTINGS) {
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    }
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_test_settings_open(&store, MESH_UI_SETTINGS_DISPLAY),
                              mesh_ui_store_shutdown(&store), "could not open a settings section");

    /* Down to the first toggle in the section, found by asking the rows rather than by counting
       them here: which fields Display offers is settings_rows.c's business, and a hard-coded
       index would land on a number the day one is added above it. */
    const char *failure = NULL;
    uint32_t toggle_row = 0U;
    bool found = false;
    for (uint32_t r = 0U; r < 32U && !found; ++r) {
        struct mesh_ui_settings_item item;
        if (!mesh_ui_settings_item(&store.settings, NULL, store.nav.settings_edits,
                                   store.nav.settings_edit_count, MESH_UI_SETTINGS_DISPLAY,
                                   MESH_UI_SETTINGS_NO_CHANNEL, r, &item)) {
            break;
        }
        if (item.kind == INKSTAND_FORM_TOGGLE) {
            toggle_row = r;
            found = true;
        }
    }
    MESH_TEST_FAIL_IF_CLEANUP(!found, mesh_ui_store_shutdown(&store),
                              "Display should offer a toggle to dirty");
    for (uint32_t r = 0U; r < toggle_row; ++r) {
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    }
    /* Off the row first, so the clean frame has it at rest. */
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);

    static char detail[192];
    for (int scale = INKCELL_SCALE_MIN; scale <= INKCELL_SCALE_MAX && failure == NULL;
         scale += INKCELL_SCALE(1)) {
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        const struct inkcell_theme *theme = inkcell_theme_at(0U);
        uint8_t *clean = capture_frame(&store, theme, scale, &width, &height, &stride);
        if (clean == NULL) {
            failure = "could not render the settings section";
            break;
        }

        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
        if (store.nav.settings_edit_count == 0U) {
            free(clean);
            failure = "Right on a toggle should leave an unsaved edit";
            break;
        }
        uint8_t *dirty = capture_frame(&store, theme, scale, &width, &height, &stride);
        if (dirty == NULL) {
            free(clean);
            failure = "could not render the edited settings section";
            break;
        }

        /*
         * The list's own band, and the chrome above and below it is excluded rather than
         * trusted. An unsaved edit rewrites the action bar - "back" becomes "save" and
         * "discard" - and those verbs start at the left margin, well inside the label column,
         * so a frame comparison that kept them answers this question with the footer whatever
         * the labels did. The app bar's badge is right-aligned and outside the column already;
         * the sixth at either end is the suite's own estimate of the two bars, the one
         * ui_capture_app_bar_badges_unsaved_edits() uses to say the body starts well below.
         */
        const uint32_t chrome = height / 6U;
        const uint32_t label_right = settings_label_right(theme, width, scale);
        const size_t moved =
            differing_in(clean, dirty, stride, 0U, label_right, chrome, height - chrome);
        if (moved == 0U) {
            snprintf(detail, sizeof detail,
                     "dirtying a row left its label untouched (nothing changed left of column %u "
                     "in the body at glyph scale %d)",
                     label_right, scale);
            failure = detail;
        }
        free(clean);
        free(dirty);

        /* Back to clean for the next scale: the edit is discarded the way a user discards one. */
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_UP, &action);
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_LEFT, &action);
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
        if (failure == NULL && store.nav.settings_edit_count != 0U) {
            failure = "Left should have taken the toggle back to the radio's value";
        }
    }

    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/* ---- what a card is for, and what a verb's colour is for ---------------------------------- */

/*
 * Opens `section` on a populated store and renders it, leaving the page ready to be read.
 *
 * Four of the cases below want the same six steps, and the store and the capture both have to
 * outlive the read - so this hands both back and every caller closes both. Returns NULL, or the
 * message to fail with.
 */
static const char *render_section(enum mesh_ui_settings_section section,
                                  struct mesh_ui_store *store, struct inkcell_capture **capture) {
    *capture = NULL;
    if (mesh_ui_store_init(store) != 0) {
        return "store init failed";
    }
    mesh_test_nav_populate(store);
    /* The Radio tab's pages are sections drawn by the same pane, reached from their cards. */
    if (section == MESH_UI_SETTINGS_RADIO_DETAILS || section == MESH_UI_SETTINGS_NODE_LISTS) {
        if (!mesh_test_open_radio_page(store, section)) {
            return "the page could not be opened";
        }
    } else {
        if (!mesh_test_open_tab(store, MESH_UI_SCREEN_SETTINGS)) {
            return "the Settings tab could not be reached";
        }
        if (!mesh_test_settings_open(store, section)) {
            return "the section could not be opened";
        }
    }
    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(store);
    if (!mesh_ui_store_consume_updates(store, &snapshot)) {
        return "no snapshot to render";
    }
    if (mesh_ui_capture_open(capture, INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT,
                             INKCELL_SCALE(4)) != 0) {
        return "capture open failed";
    }
    inkcell_capture_render(*capture, &snapshot);
    return NULL;
}

/*
 * A section with no groups draws no cards.
 *
 * A card says "these rows belong together", so it needs something to be together apart from. One
 * card wrapping a whole section says nothing at all: it is a border drawn round the page. Modules
 * is the case - fourteen rows, no headings, nothing to group - and it was drawn inside a single
 * surface because the renderer set its card flag for every open section and only the *assignment*
 * checked for headings.
 *
 * Asked the way ui_capture_draws_the_status_cards() asks the opposite question, and with the same
 * two roles, so the pair cannot drift: a card is a fill spanning most of the width with a
 * hairline doing the same, and neither belongs on a screen of plain rows. A selected row is a
 * fill on its own, which is why the edge is checked too and why the threshold is what Status is
 * held to rather than zero.
 */
MESH_TEST_CASE(ui_capture_an_ungrouped_section_draws_no_card, unit) {
    struct mesh_ui_store store;
    struct inkcell_capture *capture = NULL;
    const char *failure = render_section(MESH_UI_SETTINGS_MODULES, &store, &capture);
    if (failure == NULL) {
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
        if (widest_row_run(capture, pixels, width, height, stride, INKCELL_COLOR_OUTLINE) >= 80U) {
            failure = "Modules draws a card edge across a section that has no groups";
        }
    }
    if (capture != NULL) {
        inkcell_capture_close(capture);
    }
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * And a section that *does* have groups still draws them, which is the other half of the rule
 * above and the reason it is two cases. A gate that silenced every card would pass the one
 * before this and lose the grouping everywhere - Radio actions is five groups and is what the
 * cards were added for.
 */
MESH_TEST_CASE(ui_capture_a_grouped_section_draws_its_groups, unit) {
    struct mesh_ui_store store;
    struct inkcell_capture *capture = NULL;
    const char *failure = render_section(MESH_UI_SETTINGS_RADIO_DETAILS, &store, &capture);
    if (failure == NULL) {
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
        /* Its groups are sections now - edgeless surfaces with the page between them - so
           what says "five groups" is the number of times the middle of the column comes up
           off the ground, not an outline. */
        const struct inkcell_rgb ground =
            inkcell_theme_color(inkcell_capture_theme(capture), INKCELL_COLOR_BG);
        const uint32_t mid = width / 2U;
        uint32_t sections = 0U;
        bool on_ground = true;
        for (uint32_t y = 0U; y < height; ++y) {
            const uint8_t *p = pixels + (size_t)y * stride + (size_t)mid * 4U;
            const bool ground_here = p[0] == ground.b && p[1] == ground.g && p[2] == ground.r;
            if (on_ground && !ground_here) {
                ++sections;
            }
            on_ground = ground_here;
        }
        /* The tab strip and the footer are the other two surfaces the column crosses. */
        if (sections < 2U + 3U) {
            failure = "Radio actions draws fewer than three sections for a section of five groups";
        }
    }
    if (capture != NULL) {
        inkcell_capture_close(capture);
    }
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A verb's colour is in its gutter, never in its words.
 *
 * INKCELL_FB_LEADING_TONAL is documented with exactly this - "with the colour in the disc the words
 * go back to the ordinary ink" - and the settings renderer said it again in a comment of its own.
 * Neither was true: the row handed its tone to inkcell_fb_list_item, which inks the label from it,
 * so every weighted row drew weighted words. Radio actions carries a warning or an error on nine
 * rows in ten, and what that came to on the screen was a wall of orange with the two rows that
 * cannot be undone somewhere inside it - the thing the discs were introduced to end.
 *
 * Read as a bound on *where* rather than a count of how many, because the tone is still spent:
 * the accent edge down an irreversible row is drawn in the row's own family, and it lives in the
 * gutter with the disc. So the claim is that no tone ink reaches the text column, which holds
 * whatever the rows come to say and however many of them carry a weight. It fails by a wide
 * margin in either direction - the labels put warning ink out to 43% of the width, and the
 * gutter alone reaches 5%.
 */
MESH_TEST_CASE(ui_capture_a_verb_keeps_the_ordinary_ink, unit) {
    struct mesh_ui_store store;
    struct inkcell_capture *capture = NULL;
    const char *failure = render_section(MESH_UI_SETTINGS_RADIO_DETAILS, &store, &capture);
    char message[160];
    if (failure == NULL) {
        uint32_t width = 0U;
        uint32_t height = 0U;
        size_t stride = 0U;
        const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
        const struct inkcell_theme *theme = inkcell_capture_theme(capture);
        const struct inkcell_rgb warn = inkcell_theme_tone(theme, INKCELL_TONE_WARNING);
        const struct inkcell_rgb err = inkcell_theme_tone(theme, INKCELL_TONE_ERROR);
        /* Where the leading gutter ends, generously: a disc is one line tall and the words start
           just past it, so a tenth of the panel is room for the gutter and nothing else. */
        const uint32_t gutter = width / 10U;
        uint32_t rightmost = 0U;
        for (uint32_t y = 0U; y < height; ++y) {
            const uint8_t *row = pixels + (size_t)y * stride;
            for (uint32_t x = gutter; x < width; ++x) {
                const uint8_t *p = row + (size_t)x * 4U;
                const bool toned = (p[0] == warn.b && p[1] == warn.g && p[2] == warn.r) ||
                                   (p[0] == err.b && p[1] == err.g && p[2] == err.r);
                if (toned && x > rightmost) {
                    rightmost = x;
                }
            }
        }
        if (rightmost > 0U) {
            snprintf(message, sizeof message,
                     "a verb's tone reaches the text column: warning or error ink at x=%u, past "
                     "the gutter at x=%u",
                     rightmost, gutter);
            failure = message;
        }
    }
    if (capture != NULL) {
        inkcell_capture_close(capture);
    }
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * The column a settings section writes its rows in, measured off a rendered frame.
 *
 * Two cases want this and they ask it of different things - one of the rows of one section,
 * one of several sections against each other - so it is measured once here rather than twice
 * in two slightly different sweeps. It hands back the bands it found as well as the column
 * they agreed on, because the first case is about the rows that did *not* agree.
 *
 * Returns `width` when there was nothing readable, which is a failure for either caller.
 */
static uint32_t settings_text_column(const uint8_t *frame, uint32_t width, uint32_t height,
                                     size_t stride, const struct inkcell_theme *theme, int scale,
                                     uint32_t *out_lefts, uint32_t max_lefts, uint32_t *out_bands,
                                     uint32_t *out_agreed) {
    /*
     * A row's content is whatever is not furniture, rather than one ink.
     *
     * Asking for INKCELL_COLOR_TEXT exactly finds only the solid core of a glyph, and at the
     * small scales a stroke is two pixels wide and blended the whole way through - so the
     * leftmost "text" pixel came back somewhere in the middle of a word, differently on every
     * row. What the four colours below have in common is that they are the only things drawn
     * flat: the ground, a card's surface and its hairline, and the cursor's fill. Everything
     * else on these rows - a word, a blend at the edge of one, a slider's track - is content,
     * and content starts where the column starts.
     */
    const struct inkcell_rgb furniture[] = {
        inkcell_theme_color(theme, INKCELL_COLOR_BG),
        inkcell_theme_color(theme, INKCELL_COLOR_SURFACE),
        inkcell_theme_color(theme, INKCELL_COLOR_SURFACE_SEL),
        inkcell_theme_color(theme, INKCELL_COLOR_OUTLINE),
    };
    /* From the panel's own margin: a card in a list is drawn wider than the rows standing in
       it, so its left edge and its corners are outside every column this is about. */
    const uint32_t left_edge = (uint32_t)theme->metrics.margin;
    /*
     * And only as far right as the label column, which is where the question is settled
     * anyway - and what keeps the scroll rail out of it. The rail is a bar down the whole body
     * on the one side, so a scan that ran to the panel's edge found content on every scanline,
     * never saw a blank one, and read the entire section as a single row.
     */
    const uint32_t right_edge = settings_label_right(theme, width, scale);
    /* The suite's own estimate of the two bars, the one
       ui_capture_settings_marks_both_halves_of_an_unsaved_row() uses. */
    const uint32_t body_top = capture_body_top(height);
    const uint32_t body_bottom = height - height / 8U;
    /* A glyph's left bearing is the only thing two rows of the same column may differ by. The
       failure this is about is a whole step - nine of these - so the allowance can be generous
       and still catch it. */
    const uint32_t bearing = (uint32_t)inkcell_scale_px(3, scale);
    uint32_t band_left = width;
    uint32_t band_rows = 0U;
    uint32_t bands = 0U;
    for (uint32_t y = body_top; y <= body_bottom; ++y) {
        const uint8_t *row = frame + (size_t)y * stride;
        /*
         * Where this scanline's own ground begins, which is where the search for content
         * starts. A card is drawn wider than the rows standing in it and its edge is a hairline
         * blended against the panel, so a scan that began at the margin found that blend on
         * every scanline the card covers and reported the card's left edge as if it were a
         * word. Starting at the card's own surface steps over its edge without anything here
         * having to know how thick one is; a scanline with no card on it - a heading, in the
         * break between the card that ended and the one it opens - has none to find and starts
         * at the margin.
         */
        uint32_t start = left_edge;
        for (uint32_t x = left_edge; x < right_edge; ++x) {
            const uint8_t *p = row + (size_t)x * 4U;
            if ((p[0] == furniture[1].b && p[1] == furniture[1].g && p[2] == furniture[1].r) ||
                (p[0] == furniture[2].b && p[1] == furniture[2].g && p[2] == furniture[2].r)) {
                start = x;
                break;
            }
        }
        uint32_t left = width;
        for (uint32_t x = start; x < right_edge && left == width; ++x) {
            const uint8_t *p = row + (size_t)x * 4U;
            bool flat = false;
            for (size_t f = 0U; f < sizeof furniture / sizeof furniture[0]; ++f) {
                flat = flat ||
                       (p[0] == furniture[f].b && p[1] == furniture[f].g && p[2] == furniture[f].r);
            }
            if (!flat) {
                left = x;
            }
        }
        if (left < band_left) {
            band_left = left;
        }
        if (left != width) {
            band_rows++;
            continue;
        }
        /*
         * A blank scanline closes the band above it: one row of words, measured at its
         * leftmost.
         *
         * A band has to be as tall as a letter to be one. A card's hairline is a scanline or
         * two of blend against the ground, and it runs the width of the card rather than of the
         * column - so counted as a row it reports the card's own left edge and nothing about
         * where any word starts.
         */
        const uint32_t tall = band_rows >= (uint32_t)inkcell_scale_px(2, scale);
        band_rows = 0U;
        if (band_left == width || !tall) {
            band_left = width;
            continue;
        }
        if (bands < max_lefts) {
            out_lefts[bands++] = band_left;
        }
        band_left = width;
    }

    /*
     * The column the section is written in: the one the most rows agree on.
     *
     * Taken from the rows rather than derived here, because deriving it would be this test
     * re-stating the arithmetic it is meant to be checking.
     */
    uint32_t column = width;
    uint32_t agreed = 0U;
    for (uint32_t a = 0U; a < bands; ++a) {
        uint32_t near = 0U;
        for (uint32_t b = 0U; b < bands; ++b) {
            const uint32_t hi = out_lefts[a] > out_lefts[b] ? out_lefts[a] : out_lefts[b];
            const uint32_t lo = out_lefts[a] > out_lefts[b] ? out_lefts[b] : out_lefts[a];
            near += (hi - lo <= bearing) ? 1U : 0U;
        }
        if (near > agreed) {
            agreed = near;
            column = out_lefts[a];
        }
    }
    *out_bands = bands;
    *out_agreed = agreed;
    return column;
}

/*
 * Every row of a settings section starts its words in one column, whatever height the row is.
 *
 * The leading slot is declared for a whole list or for none of it, and that rule is stated twice
 * in fb_widgets_list.h - once on INKCELL_FB_LEADING_ICON ("reserved whether or not this row filled
 * it") and once on the empty slot that exists for nothing else. The geometry had been breaking it
 * since the settings screen gained a row two steps tall: the gutter was measured off the row's own
 * fill height rather than off the list's step, so a section that mixed a slider in with its
 * neighbours drew that row's label, its value and its track a step further right than the rows
 * above and below it. Position is the worst of them - four of its six top rows carry a track -
 * and the seam is perfectly legible on the panel once it is pointed at.
 *
 * Asked of the *normal* ink and of the body band, which between them leave in exactly the rows
 * this is about. A group heading stands at the panel's own margin rather than in the column, and
 * it is dim; the row under the cursor is drawn in the on-selection ink; the app bar's back arrow
 * is above the band. What is left is the rows of the section, and their left edges have only a
 * glyph's own bearing to differ by - where the bug under test moves one of them by a whole step.
 */
MESH_TEST_CASE(ui_capture_a_section_starts_every_row_in_one_column, unit) {
    const char *failure = NULL;
    static char detail[256];

    for (size_t t = 0; t < inkcell_theme_count() && failure == NULL; ++t) {
        const struct inkcell_theme *theme = inkcell_theme_at(t);
        for (int scale = INKCELL_SCALE_MIN; scale <= INKCELL_SCALE_MAX && failure == NULL;
             scale += INKCELL_SCALE(1)) {
            struct mesh_ui_store store;
            if (mesh_ui_store_init(&store) != 0) {
                failure = "store init failed";
                break;
            }
            mesh_test_nav_populate(&store);
            /* A Position the radio has answered for, with every one of its scales somewhere in
               the middle of its own track - a row at an end of its scale still draws the track,
               but a value the field reads as a word does not, and this wants the rows that do. */
            struct mesh_ui_settings settings = store.settings;
            settings.loaded = true;
            settings.has_position = true;
            settings.gps_mode = 1U;
            settings.position_broadcast_secs = 900U;
            settings.position_broadcast_smart_enabled = true;
            settings.smart_minimum_distance = 100U;
            settings.smart_minimum_interval_secs = 30U;
            settings.gps_update_interval = 120U;
            mesh_ui_store_set_settings(&store, &settings);

            uint8_t *frame = NULL;
            uint32_t width = 0U;
            uint32_t height = 0U;
            size_t stride = 0U;
            if (!mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS)) {
                failure = "the Settings tab could not be reached";
            } else if (!mesh_test_settings_open(&store, MESH_UI_SETTINGS_POSITION)) {
                failure = "Settings > Position could not be opened";
            } else {
                frame = capture_frame(&store, theme, scale, &width, &height, &stride);
                if (frame == NULL) {
                    failure = "capture failed";
                }
            }

            if (failure == NULL) {
                uint32_t lefts[64];
                uint32_t bands = 0U;
                uint32_t agreed = 0U;
                const uint32_t bearing = (uint32_t)inkcell_scale_px(3, scale);
                const uint32_t column = settings_text_column(
                    frame, width, height, stride, theme, scale, lefts,
                    (uint32_t)(sizeof lefts / sizeof lefts[0]), &bands, &agreed);
                /*
                 * Nothing may start to the *right* of it, and that is the whole assertion.
                 *
                 * Left of it is the gutter, and a row is entitled to put a disc there - that is
                 * what the slot is: the rows carrying a symbol start it in the gutter and their
                 * words in the column, and the rows carrying none leave the gutter empty. Right
                 * of it there is nothing a row may legitimately be, so a row that begins there
                 * has been indented by something that is not the list.
                 */
                for (uint32_t a = 0U; a < bands && failure == NULL; ++a) {
                    if (lefts[a] > column + bearing) {
                        snprintf(detail, sizeof detail,
                                 "a settings row starts in its own column: row %u begins at x=%u "
                                 "where the section is written at x=%u, on theme %s at glyph "
                                 "scale %d",
                                 a + 1U, lefts[a], column, theme->name, scale);
                        failure = detail;
                    }
                }
                if (failure == NULL && (bands < 4U || agreed < 3U)) {
                    snprintf(detail, sizeof detail,
                             "only %u rows of Position were readable at glyph scale %d, %u of "
                             "them in one column - there is nothing here to compare",
                             bands, scale, agreed);
                    failure = detail;
                }
            }
            free(frame);
            mesh_ui_store_shutdown(&store);
        }
    }

    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * And every *section* writes in the same column as every other, which is the rule one level out.
 *
 * The case above holds one list to one column; this holds the tab to one. The two used to be
 * different questions, and the answer to the second was "it depends": the leading slot was
 * reserved off whether the section happened to hold a verb, so Position - which has the fixed
 * position pair below the fold - indented every row past a disc's width and Radio UI, which is
 * the same list of fields with no press in it, did not. Walking between them the words moved
 * about two cells sideways for a reason nothing on either screen showed, and Device did it a
 * third way by having no cards at all to indent inside.
 *
 * Four sections, chosen to be the four shapes that used to disagree: one that holds a verb
 * *and* fields (Position), one that holds only fields and draws cards (Radio UI), one that
 * holds only fields and draws none (Device), and About radio, which is mostly readings with a
 * press among them. About radio is on this tab only while another node is being administered -
 * the radio on the link has it on the Radio tab, whose column is that tab's - so the fixture
 * points the tab at one. A section list is deliberately not among them - the root and
 * Modules are lists of subjects that fill a narrower icon slot on every row, which is a
 * different list and says so.
 *
 * Measured rather than compared against a constant, for the reason the case above measures: a
 * number written here would be this test restating the arithmetic under it, and would go stale
 * the first time a margin moved.
 */
MESH_TEST_CASE(ui_capture_every_section_starts_in_the_same_column, unit) {
    static const enum mesh_ui_settings_section k_sections[] = {
        MESH_UI_SETTINGS_POSITION,
        MESH_UI_SETTINGS_RADIO_UI,
        MESH_UI_SETTINGS_DEVICE,
        MESH_UI_SETTINGS_RADIO,
    };
    const char *failure = NULL;
    static char detail[256];

    for (size_t t = 0; t < inkcell_theme_count() && failure == NULL; ++t) {
        const struct inkcell_theme *theme = inkcell_theme_at(t);
        for (int scale = INKCELL_SCALE_MIN; scale <= INKCELL_SCALE_MAX && failure == NULL;
             scale += INKCELL_SCALE(1)) {
            const uint32_t bearing = (uint32_t)inkcell_scale_px(3, scale);
            uint32_t columns[sizeof k_sections / sizeof k_sections[0]];
            size_t measured = 0U;

            for (size_t i = 0; i < sizeof k_sections / sizeof k_sections[0] && failure == NULL;
                 ++i) {
                struct mesh_ui_store store;
                if (mesh_ui_store_init(&store) != 0) {
                    failure = "store init failed";
                    break;
                }
                mesh_test_nav_populate(&store);
                /* Everything these four read, answered at once - a section the radio has not
                   spoken for draws one sentence and no rows, and a sentence is not a column. */
                struct mesh_ui_settings settings = store.settings;
                settings.loaded = true;
                settings.has_metadata = true;
                settings.has_position = true;
                settings.has_device = true;
                settings.has_display = true;
                settings.gps_mode = 1U;
                settings.position_broadcast_secs = 900U;
                settings.gps_update_interval = 120U;
                settings.has_ui_config = true;
                settings.ui_brightness = 153U;
                settings.ui_screen_timeout = 60U;
                settings.fw_supported = true;
                snprintf(settings.firmware_version, sizeof settings.firmware_version, "%s",
                         "2.7.6");
                snprintf(settings.fw_channel, sizeof settings.fw_channel, "%s", "stable");
                settings.admin_dest = 0x8F21B005U;
                snprintf(settings.admin_dest_name, sizeof settings.admin_dest_name, "%s", "BRVO");
                mesh_ui_store_set_settings(&store, &settings);

                uint8_t *frame = NULL;
                uint32_t width = 0U;
                uint32_t height = 0U;
                size_t stride = 0U;
                if (!mesh_test_open_tab(&store, MESH_UI_SCREEN_SETTINGS) ||
                    !mesh_test_settings_open(&store, k_sections[i])) {
                    snprintf(detail, sizeof detail, "section %u could not be opened",
                             (unsigned)k_sections[i]);
                    failure = detail;
                } else {
                    frame = capture_frame(&store, theme, scale, &width, &height, &stride);
                    if (frame == NULL) {
                        failure = "capture failed";
                    }
                }
                if (failure == NULL) {
                    uint32_t lefts[64];
                    uint32_t bands = 0U;
                    uint32_t agreed = 0U;
                    const uint32_t column = settings_text_column(
                        frame, width, height, stride, theme, scale, lefts,
                        (uint32_t)(sizeof lefts / sizeof lefts[0]), &bands, &agreed);
                    if (bands < 3U || agreed < 2U) {
                        snprintf(detail, sizeof detail,
                                 "only %u rows of section %u were readable at glyph scale %d - "
                                 "there is nothing here to compare",
                                 bands, (unsigned)k_sections[i], scale);
                        failure = detail;
                    } else {
                        columns[measured++] = column;
                    }
                }
                free(frame);
                mesh_ui_store_shutdown(&store);
            }

            for (size_t i = 1U; i < measured && failure == NULL; ++i) {
                const uint32_t hi = columns[i] > columns[0] ? columns[i] : columns[0];
                const uint32_t lo = columns[i] > columns[0] ? columns[0] : columns[i];
                if (hi - lo > bearing) {
                    snprintf(detail, sizeof detail,
                             "section %u is written at x=%u where section %u is written at x=%u, "
                             "on theme %s at glyph scale %d - the Settings tab has one column",
                             (unsigned)k_sections[i], columns[i], (unsigned)k_sections[0],
                             columns[0], theme->name, scale);
                    failure = detail;
                }
            }
        }
    }

    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A verb's disc clears the edges of the card it stands on, and the cursor standing anywhere on
 * that card takes none of them off the frame.
 *
 * A settings group is one card whatever is in it, verbs included, so a verb is now a card row
 * like any other - and it is the one card row whose leading slot is *filled*: a tonal disc
 * nearly as tall as the step, where a field puts a pencil the size of a glyph. That makes the
 * first and last rows of a card the case to watch, because a card's hairline is drawn outside
 * its own rows' boxes (inkcell_fb_list_cards()) and a disc drawn to the full slot would meet it
 * there. Three readings of one hairline, and they are the three ways this goes wrong:
 *
 *   - the disc's crown reaches the edge above it, so the two read as one mark;
 *   - the hairline is left inside the card's own first or last row, and that row's highlight
 *     paints it out - the card reads as open on precisely the row being pointed at;
 *   - the card pads into a step it cannot pad into and lands its edge across the disc.
 *
 * Two assertions, because no one of them catches all three. **Purity**: nothing filled from a
 * tonal family - which is what a disc is filled from - may stand on a scanline a card's edge
 * owns, or the one either side of it. **Presence**: the number of edges does not depend on where
 * the cursor is. The second is the one the first cannot make, and it is the whole reason this
 * case walks the cursor: a highlight laid over an edge does not corrupt that scanline, it
 * *removes* it, so a frame where the card has lost its bottom is a frame where a check looking
 * for edges simply finds one fewer. The first version of this case looked only at row 0 and
 * passed against exactly that.
 *
 * The Radio card's details page is the fixture because it is the section that mixes the two
 * hardest: About radio's fourteen facts with two verbs among them, so the discs land in the middle
 * of a card of readings rather than in a column of their own. It is also the shape that used to be
 * drawn a third way - the verbs stood on the bare panel under the card holding the fields - and a
 * case that still passes here would have caught that change breaking either edge it moved.
 *
 * The walk stops short of the window, so every frame shows the same cards and the counts are
 * comparable, and it starts one row down. The very first row of the body is a case of its own
 * and not this one: inkcell_fb_list_cards() spends a card's top hairline upward, and the first card
 * has nowhere above it to spend into, so the ceiling clamps the hairline back inside the row and
 * the cursor there covers it. That is the body's edge, deliberate, and it would read here as an
 * edge the cursor removed.
 */
MESH_TEST_CASE(ui_capture_a_verbs_disc_clears_its_sections_edges, unit) {
    const char *failure = NULL;
    static char detail[256];

    for (size_t t = 0; t < inkcell_theme_count() && failure == NULL; ++t) {
        const struct inkcell_theme *theme = inkcell_theme_at(t);
        for (int scale = INKCELL_SCALE_MIN; scale <= INKCELL_SCALE_MAX && failure == NULL;
             scale += INKCELL_SCALE(1)) {
            struct mesh_ui_store store;
            if (mesh_ui_store_init(&store) != 0) {
                failure = "store init failed";
                break;
            }
            mesh_test_nav_populate(&store);
            /*
             * A radio that has answered for itself. `fw_supported` is what puts the firmware
             * pair - the verbs this case is about - on the screen at all; without it the whole
             * group collapses to one fact, and an earlier version of this case passed against a
             * broken renderer for exactly that reason. The connection gives the section its
             * second group, and so its cards.
             */
            struct mesh_ui_settings settings = store.settings;
            settings.loaded = true;
            settings.has_metadata = true;
            settings.fw_supported = true;
            snprintf(settings.firmware_version, sizeof settings.firmware_version, "%s", "2.7.6");
            snprintf(settings.fw_channel, sizeof settings.fw_channel, "%s", "stable");
            settings.connection.valid = true;
            settings.connection.has_wifi = true;
            settings.connection.wifi_connected = true;
            snprintf(settings.connection.wifi_ssid, sizeof settings.connection.wifi_ssid, "%s",
                     "shed");
            mesh_ui_store_set_settings(&store, &settings);

            if (!mesh_test_open_radio_page(&store, MESH_UI_SETTINGS_RADIO_DETAILS)) {
                failure = "Radio > details could not be opened";
                mesh_ui_store_shutdown(&store);
                break;
            }

            const struct inkcell_rgb edge = inkcell_theme_color(theme, INKCELL_COLOR_OUTLINE);
            const struct inkcell_rgb ground = inkcell_theme_color(theme, INKCELL_COLOR_BG);
            const struct inkcell_rgb surface = inkcell_theme_color(theme, INKCELL_COLOR_SURFACE);
            struct mesh_ui_action act;
            memset(&act, 0, sizeof act);
            uint32_t seen[4] = {0U, 0U, 0U, 0U};
            uint32_t frames = 0U;
            size_t discs = 0U;

            /* Off the first row, for the reason in the note above. */
            (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &act);
            for (uint32_t row = 0U; row < 4U && failure == NULL; ++row) {
                if (row > 0U) {
                    const uint32_t before = store.nav.cursor[store.nav.screen];
                    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &act);
                    if (store.nav.cursor[store.nav.screen] == before) {
                        break;
                    }
                }
                uint32_t width = 0U;
                uint32_t height = 0U;
                size_t stride = 0U;
                uint8_t *frame = capture_frame(&store, theme, scale, &width, &height, &stride);
                if (frame == NULL) {
                    failure = "capture failed";
                    break;
                }
                /*
                 * A group's edge is where its section meets the page: the scanline on which the
                 * middle of the column turns from the ground to the section's surface or back.
                 * Sections are edgeless - no hairline is drawn round one - so the boundary is the
                 * edge, and it is what a disc's crown must stay clear of and what a row's lift
                 * must not paint out. Taken at the middle of the column because a section's
                 * rounded corners and the cursor's capsule are both at its sides.
                 */
                const uint32_t mid = content_column_width(theme, width, scale) / 2U +
                                     (width - content_column_width(theme, width, scale)) / 2U;
                /* A run off the ground is a section when it is taller than a hairline and
                   stands clear of both ends of the frame - which leaves out the rule under the
                   tab strip and the footer, the column's other two crossings. */
                uint32_t bounds[64];
                uint32_t bound_count = 0U;
                uint32_t run_top = 0U;
                bool in_run = false;
                for (uint32_t y = 0U; y <= height; ++y) {
                    bool ground_here = true;
                    if (y < height) {
                        const uint8_t *here = frame + (size_t)y * stride + (size_t)mid * 4U;
                        ground_here =
                            here[0] == ground.b && here[1] == ground.g && here[2] == ground.r;
                    }
                    if (!ground_here && !in_run) {
                        in_run = true;
                        run_top = y;
                    } else if (ground_here && in_run) {
                        in_run = false;
                        const bool section = y - run_top > 4U && run_top > 0U && y < height;
                        if (section && bound_count + 2U <= sizeof bounds / sizeof bounds[0]) {
                            bounds[bound_count++] = run_top;
                            bounds[bound_count++] = y - 1U;
                        }
                    }
                }
                uint32_t edges = 0U;
                for (uint32_t b = 0U; b < bound_count && failure == NULL; ++b) {
                    const uint32_t y = bounds[b];
                    edges++;
                    const uint32_t from = y > 0U ? y - 1U : y;
                    const uint32_t to = y + 1U < height ? y + 1U : y;
                    for (uint32_t ny = from; ny <= to && failure == NULL; ++ny) {
                        const uint8_t *band = frame + (size_t)ny * stride;
                        for (uint32_t x = 0U; x < width && failure == NULL; ++x) {
                            const uint8_t *p = band + (size_t)x * 4U;
                            if ((p[0] == edge.b && p[1] == edge.g && p[2] == edge.r) ||
                                (p[0] == ground.b && p[1] == ground.g && p[2] == ground.r) ||
                                (p[0] == surface.b && p[1] == surface.g && p[2] == surface.r)) {
                                continue;
                            }
                            for (int f = 0; f < (int)INKCELL_FAMILY_COUNT; ++f) {
                                const struct inkcell_rgb fill = inkcell_theme_family(
                                    theme, (enum inkcell_family)f, INKCELL_SLOT_CONTAINER);
                                if (p[0] != fill.b || p[1] != fill.g || p[2] != fill.r) {
                                    continue;
                                }
                                /*
                                 * And that it is a *filled shape* rather than one pixel of a
                                 * glyph's edge that happens to land on the same colour.
                                 *
                                 * A family's container fill is a mid tone, and antialiasing
                                 * text against a card's surface walks through mid tones: on the
                                 * High contrast theme the tertiary container is the exact grey
                                 * a descender's last row comes out at, so "Syslog" on the
                                 * bottom row of a card read here as a disc touching that card's
                                 * edge. A disc is a dozen scanlines tall and its crown has the
                                 * next one under it, so the pixel above or below is the same
                                 * fill; a glyph's edge has ink on one side and ground on the
                                 * other. The check is the shape, which is what the sweep meant
                                 * all along - it had simply never met a row whose descenders
                                 * sat one scanline off an edge.
                                 */
                                const uint8_t *up = ny > 0U ? band - stride + (size_t)x * 4U : NULL;
                                const uint8_t *down =
                                    ny + 1U < height ? band + stride + (size_t)x * 4U : NULL;
                                const bool solid = (up != NULL && up[0] == fill.b &&
                                                    up[1] == fill.g && up[2] == fill.r) ||
                                                   (down != NULL && down[0] == fill.b &&
                                                    down[1] == fill.g && down[2] == fill.r);
                                if (!solid) {
                                    continue;
                                }
                                snprintf(detail, sizeof detail,
                                         "a leading disc reaches a card's edge at (%u,%u), which "
                                         "the edge owns at y=%u - cursor on row %u, theme %s, "
                                         "glyph scale %d",
                                         x, ny, y, row, theme->name, scale);
                                failure = detail;
                                break;
                            }
                        }
                    }
                }
                /* And that there was a disc on the frame at all, or the sweep above passes for
                   want of anything to find. */
                for (uint32_t y = 0U; y < height && discs == 0U; ++y) {
                    const uint8_t *line = frame + (size_t)y * stride;
                    for (uint32_t x = 0U; x < width && discs == 0U; ++x) {
                        const uint8_t *p = line + (size_t)x * 4U;
                        for (int f = 0; f < (int)INKCELL_FAMILY_COUNT; ++f) {
                            const struct inkcell_rgb fill = inkcell_theme_family(
                                theme, (enum inkcell_family)f, INKCELL_SLOT_CONTAINER);
                            if (p[0] == fill.b && p[1] == fill.g && p[2] == fill.r) {
                                discs++;
                                break;
                            }
                        }
                    }
                }
                seen[frames++] = edges;
                free(frame);
            }

            if (failure == NULL && (frames < 2U || seen[0] < 2U)) {
                snprintf(detail, sizeof detail,
                         "the details page gave %u frames and %u section edges at glyph scale %d - "
                         "there is nothing here to stand on",
                         frames, frames > 0U ? seen[0] : 0U, scale);
                failure = detail;
            }
            if (failure == NULL && discs == 0U) {
                snprintf(detail, sizeof detail,
                         "About radio drew no tonal disc at glyph scale %d on theme %s - there is "
                         "no verb here to clear anything",
                         scale, theme->name);
                failure = detail;
            }
            for (uint32_t f = 1U; f < frames && failure == NULL; ++f) {
                if (seen[f] == seen[0]) {
                    continue;
                }
                snprintf(detail, sizeof detail,
                         "moving the cursor down %u took a section edge off the frame: %u edges "
                         "against %u where the walk started - theme %s, glyph scale %d",
                         f, seen[f], seen[0], theme->name, scale);
                failure = detail;
            }
            mesh_ui_store_shutdown(&store);
        }
    }

    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}

/*
 * A dialog's two answers, and the press that moves between them.
 *
 * The pair is laid out side by side when the words fit on one line and *stacked* when they do
 * not, and inkcell decides which from the words and the panel's width. The nav used to toggle
 * on every direction, which was right only because it had no way to be wrong; what it does now
 * is resolve the press against the boxes the frame registered, which is right for both layouts
 * and for neither by accident.
 *
 * Two panels rather than one, because one layout proves nothing: the whole claim is that the
 * same press gives different answers on two frames, and that the difference comes from the
 * frame rather than from anything the nav remembered.
 */
static bool dialog_moves(uint32_t width, uint32_t height, uint8_t cursor, enum inkcell_key key,
                         const char **failure) {
    struct mesh_ui_store store;
    if (mesh_ui_store_init(&store) != 0) {
        *failure = "store init failed";
        return false;
    }
    mesh_test_nav_populate(&store);
    /* Raised rather than walked to: which row opens a confirm is nav_settings.c's business and
       is covered there. What is under test is the press once one is up. */
    store.nav.screen = MESH_UI_SCREEN_SETTINGS;
    store.nav.settings_section = MESH_UI_SETTINGS_ACTIONS;
    store.nav.confirm.subject = (uint8_t)MESH_UI_SETTINGS_ACTION_REBOOT;
    store.nav.confirm.open = true;
    store.nav.confirm.cursor = cursor;

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    if (!mesh_ui_store_consume_updates(&store, &snapshot)) {
        mesh_ui_store_shutdown(&store);
        *failure = "no snapshot to render";
        return false;
    }

    struct inkcell_capture *capture = NULL;
    if (mesh_ui_capture_open(&capture, width, height, INKCELL_SCALE(2)) != 0) {
        mesh_ui_store_shutdown(&store);
        *failure = "capture open failed";
        return false;
    }
    /* Until the layer has arrived: a dialog half way in is a dialog whose boxes are half way
       in too, and the press is answered against where they came to rest. */
    render_until_still(capture, &snapshot);

    /* The seam: what the frame collected, handed to the model that answers the press. On the
       device this is mesh_ui_controller_handle_key() reading it back through the backend. */
    mesh_ui_store_set_focus_map(&store, inkcell_capture_state(capture)->focus);
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, key, &action);
    const bool moved = store.nav.confirm.cursor != cursor;

    inkcell_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    return moved;
}

MESH_TEST_CASE(ui_capture_a_dialog_answers_the_press_its_own_layout_was_given, unit) {
    const char *failure = NULL;
    /* Wide: the two answers share a line, so the press between them is sideways and the
       vertical one goes nowhere. */
    const bool wide_sideways =
        dialog_moves(INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT, 0U, INKCELL_KEY_LEFT, &failure);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    MESH_TEST_FAIL_IF(!wide_sideways, "left should reach the other answer on a panel wide "
                                      "enough to put them side by side");
    const bool wide_vertical =
        dialog_moves(INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT, 0U, INKCELL_KEY_DOWN, &failure);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    MESH_TEST_FAIL_IF(wide_vertical, "down should go nowhere when the answers are side by side");

    /*
     * Narrow: the same two answers, stacked, and the same two presses the other way round -
     * from the geometry, without the nav being told which layout it got.
     *
     * 200 px is not a panel anything ships on; it is the width at which "Reboot now" and
     * "Cancel" stop fitting on one line at this glyph scale, which is the only thing that
     * decides the layout. A number chosen for a device would be a number that stops meaning
     * anything the first time the type scale moves.
     */
    const bool narrow_vertical = dialog_moves(200U, 480U, 0U, INKCELL_KEY_DOWN, &failure);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    MESH_TEST_FAIL_IF(!narrow_vertical, "down should reach the answer stacked under this one");
    const bool narrow_sideways = dialog_moves(200U, 480U, 0U, INKCELL_KEY_LEFT, &failure);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    MESH_TEST_FAIL_IF(narrow_sideways, "left should go nowhere when the answers are stacked");
    const bool wide_accept = dialog_moves(INKCELL_CAPTURE_WIDTH, INKCELL_CAPTURE_HEIGHT, 1U,
                                          INKCELL_KEY_RIGHT, &failure);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    MESH_TEST_FAIL_IF(!wide_accept, "right should reach accept from the default cancel answer");
    record_success(test_name);
}

MESH_TEST_CASE(ui_capture_a_dialog_falls_back_before_its_first_frame, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct inkcell_focus_item items[2];
    struct inkcell_focus_map map;
    inkcell_focus_begin(&map, items, 2U);
    mesh_ui_store_set_focus_map(&store, &map);
    store.nav.screen = MESH_UI_SCREEN_SETTINGS;
    store.nav.confirm.open = true;
    store.nav.confirm.cursor = 1U;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    const bool moved = store.nav.confirm.cursor == 0U;

    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(!moved, "a stale underlying frame should not trap focus on Cancel");
    record_success(test_name);
}

MESH_TEST_CASE(ui_capture_a_dialog_ignores_the_rows_behind_it, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");

    struct inkcell_focus_item items[3];
    struct inkcell_focus_map map;
    inkcell_focus_begin(&map, items, 3U);
    /* The rectangles measured on the first-run LoRa save: the row behind spans the buttons'
       vertical centre and used to win the unfiltered search from Cancel to the right. */
    (void)inkcell_focus_add(&map, (uint32_t)MESH_UI_FOCUS_ROWS + 9U, 8, 493, 1000, 36);
    (void)inkcell_focus_add(&map, (uint32_t)MESH_UI_FOCUS_DIALOG + 1U, 616, 481, 146, 44);
    (void)inkcell_focus_add(&map, (uint32_t)MESH_UI_FOCUS_DIALOG, 770, 481, 230, 44);
    mesh_ui_store_set_focus_map(&store, &map);
    store.nav.screen = MESH_UI_SCREEN_SETTINGS;
    store.nav.confirm.open = true;
    store.nav.confirm.cursor = 1U;

    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    const bool moved = store.nav.confirm.cursor == 0U;

    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(!moved, "a modal should resolve focus without the dimmed rows behind it");
    record_success(test_name);
}

/*
 * A list set with the accent cursor says where the cursor is on the row itself - a light lift and
 * a capsule down the leading edge - so the frame's travelling ring stays off it: one cursor, one
 * mark. The row is still registered, so a press and a pointer reach it, and it is the row the
 * cursor moved to that is registered after a press.
 */
MESH_TEST_CASE(ui_capture_an_accent_list_keeps_the_ring_off_its_rows, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    while (store.nav.screen != MESH_UI_SCREEN_SETTINGS) {
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_R1, &action);
    }

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                                   INKCELL_CAPTURE_HEIGHT, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");
    struct inkcell_draw_state *const state = inkcell_capture_state(capture);

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    render_until_still(capture, &snapshot);

    const uint32_t first = store.nav.cursor[MESH_UI_SCREEN_SETTINGS];
    struct inkcell_focus_rect row = {0, 0, 0, 0};
    const bool registered =
        inkcell_focus_rect_of(state->focus, (uint32_t)MESH_UI_FOCUS_ROWS + first, &row);
    const bool ringed = inkcell_fb_focus_ring_rect(state, NULL, NULL);
    const uint32_t marked = inkcell_focus_marked(state->focus);

    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    render_until_still(capture, &snapshot);
    const uint32_t next = store.nav.cursor[MESH_UI_SCREEN_SETTINGS];
    const bool moved =
        next != first &&
        inkcell_focus_rect_of(state->focus, (uint32_t)MESH_UI_FOCUS_ROWS + next, &row);
    const bool ringed_after = inkcell_fb_focus_ring_rect(state, NULL, NULL);

    inkcell_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(!registered, "the cursor row should still be a target");
    MESH_TEST_FAIL_IF(marked != INKCELL_FOCUS_NONE,
                      "an accent row should not hand the frame's ring its box");
    MESH_TEST_FAIL_IF(ringed || ringed_after, "no ring should circle an accent list's cursor");
    MESH_TEST_FAIL_IF(!moved, "the row the cursor moved to should be the registered one");
    record_success(test_name);
}

/*
 * The focus ring is drawn by the frame, on the box that drew itself focused, and it travels.
 *
 * The screens never name the ring's target - the control marks itself while drawing - so what
 * this holds is the seam between the two: after a render, the ring is exactly on the focused
 * answer's registered box; after a press, it is on its way to the other one rather than already
 * there, and it lands. A dialog's two answers are the fixture because a list here carries its
 * own cursor (see the case above) and a question's answers are where the ring still goes.
 */
MESH_TEST_CASE(ui_capture_focus_ring_follows_the_dialog_cursor, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);
    struct mesh_ui_action action;
    memset(&action, 0, sizeof action);
    store.nav.screen = MESH_UI_SCREEN_SETTINGS;
    store.nav.confirm.open = true;
    store.nav.confirm.cursor = 0U;
    store.nav.confirm.subject = (uint8_t)MESH_UI_SETTINGS_ACTION_REBOOT;

    struct inkcell_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, INKCELL_CAPTURE_WIDTH,
                                                   INKCELL_CAPTURE_HEIGHT, INKCELL_SCALE(4)) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");
    struct inkcell_draw_state *const state = inkcell_capture_state(capture);

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    render_until_still(capture, &snapshot);

    const uint32_t first = (uint32_t)MESH_UI_FOCUS_DIALOG + store.nav.confirm.cursor;
    struct inkcell_focus_rect box = {0, 0, 0, 0};
    struct inkcell_focus_rect ring = {0, 0, 0, 0};
    const bool placed = inkcell_focus_rect_of(state->focus, first, &box) &&
                        inkcell_fb_focus_ring_rect(state, &ring, NULL);
    MESH_TEST_FAIL_IF_CLEANUP(!placed, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the focused answer should be registered and ringed");
    MESH_TEST_FAIL_IF_CLEANUP(memcmp(&box, &ring, sizeof box) != 0, inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the ring should sit on the focused answer's own box");
    MESH_TEST_FAIL_IF_CLEANUP(inkcell_focus_marked(state->focus) != first,
                              inkcell_capture_close(capture);
                              mesh_ui_store_shutdown(&store),
                              "the rows dimmed behind the question must not take the ring");

    const uint8_t before = store.nav.confirm.cursor;
    (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_DOWN, &action);
    if (store.nav.confirm.cursor == before) {
        (void)mesh_ui_store_handle_key(&store, INKCELL_KEY_RIGHT, &action);
    }
    (void)mesh_ui_store_consume_updates(&store, &snapshot);
    inkcell_capture_render(capture, &snapshot);
    const bool travelling = inkcell_capture_animating(capture);
    render_until_still(capture, &snapshot);

    const uint32_t next = (uint32_t)MESH_UI_FOCUS_DIALOG + store.nav.confirm.cursor;
    const bool landed = next != first && inkcell_focus_rect_of(state->focus, next, &box) &&
                        inkcell_fb_focus_ring_rect(state, &ring, NULL) &&
                        memcmp(&box, &ring, sizeof box) == 0;
    inkcell_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    MESH_TEST_FAIL_IF(!travelling, "a press should set the ring travelling, not jump it");
    MESH_TEST_FAIL_IF(!landed, "the ring should land on the answer the cursor moved to");
    record_success(test_name);
}

/* Whether rows [top, bottom) of the frame are one colour - a band nothing was drawn in. */
static bool capture_band_is_blank(const struct inkcell_capture *capture, uint32_t top,
                                  uint32_t bottom) {
    uint32_t width = 0U, height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = inkcell_capture_pixels(capture, &width, &height, &stride);
    if (pixels == NULL || bottom > height) {
        return false;
    }
    const uint8_t *first = pixels + (size_t)top * stride;
    for (uint32_t y = top; y < bottom; ++y) {
        const uint8_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0U; x < width; ++x) {
            if (memcmp(row + (size_t)x * 4U, first, 4U) != 0) {
                return false;
            }
        }
    }
    return true;
}

/*
 * A window types on the keyboard under the reader's hands, so the frame drawn for it leaves the
 * grid out: on the device the lower half of the screen is keys, in a window it is empty. The
 * field is what stays, and it is taller than the device's two lines.
 */
MESH_TEST_CASE(fb_keyboard_grid_is_left_out_of_a_window, unit) {
    struct inkcell_capture *capture = NULL;
    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    const char *failure = NULL;
    if (snapshot == NULL || mesh_ui_capture_open(&capture, 1024U, 768U, INKCELL_SCALE(4)) != 0) {
        failure = "capture allocation failed";
        goto cleanup;
    }
    snapshot->nav.screen = MESH_UI_SCREEN_MESSAGES;
    snapshot->nav.keyboard_open = true;
    snprintf(snapshot->nav.target_name, sizeof snapshot->nav.target_name, "BRVO");
    snprintf(snapshot->nav.draft, sizeof snapshot->nav.draft, "ok");

    inkcell_capture_render(capture, snapshot);
    if (capture_band_is_blank(capture, 600U, 700U)) {
        failure = "the device frame should draw the grid under the field";
        goto cleanup;
    }
    inkcell_capture_state(capture)->pointer = true;
    inkcell_capture_render(capture, snapshot);
    if (!capture_band_is_blank(capture, 600U, 700U)) {
        failure = "a window's frame should not draw a grid the reader types past";
        goto cleanup;
    }
    if (capture_band_is_blank(capture, 200U, 260U)) {
        failure = "a window's field should grow into the room the grid left";
    }
cleanup:
    inkcell_capture_close(capture);
    free(snapshot);
    MESH_TEST_FAIL_IF(failure != NULL, failure);
    record_success(test_name);
}
