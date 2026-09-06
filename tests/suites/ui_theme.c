#define _POSIX_C_SOURCE 200809L

/*
 * Themes, fonts, and the promise that switching one changes the whole frame.
 *
 * Two kinds of case live here. The first are about the tables themselves: every theme is
 * complete, names a font that exists, and is readable - mesh_ui_theme_validate() measures the
 * contrast rather than trusting a palette that looked fine on the monitor it was picked on.
 *
 * The second kind is the one that matters for the future theme switcher: a snapshot rendered
 * under two themes must differ, and the ground it is drawn on must be the ground the theme
 * names. That is what catches a renderer that quietly kept a colour of its own - the failure
 * mode a role-based palette exists to prevent, and one no amount of looking at the dark theme
 * would reveal.
 */

#include "framework/mesh_test.h"
#include "support/ui_fixture.h"

#include "mesh/ui/backends/fb_capture.h"
#include "mesh/ui/font.h"
#include "mesh/ui/store.h"
#include "mesh/ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

MESH_TEST_CASE(ui_theme_registry_round_trips, unit) {
    MESH_TEST_FAIL_IF(mesh_ui_theme_count() == 0U, "no themes are registered");
    MESH_TEST_FAIL_IF(mesh_ui_theme_default() != mesh_ui_theme_at(0U),
                      "the default is not the first theme");

    for (size_t i = 0; i < mesh_ui_theme_count(); ++i) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(i);
        MESH_TEST_FAIL_IF(theme == NULL, "a registered theme is NULL");
        MESH_TEST_FAIL_IF(mesh_ui_theme_by_id(theme->id) != theme,
                          "a theme does not come back under its own id");
        for (size_t j = 0; j < i; ++j) {
            MESH_TEST_FAIL_IF(strcmp(mesh_ui_theme_at(j)->id, theme->id) == 0,
                              "two themes share an id");
        }
    }

    MESH_TEST_FAIL_IF(mesh_ui_theme_at(mesh_ui_theme_count()) != NULL,
                      "an index past the end returned a theme");
    MESH_TEST_FAIL_IF(mesh_ui_theme_by_id("no-such-theme") != NULL,
                      "an unknown id returned a theme");
    MESH_TEST_FAIL_IF(mesh_ui_theme_by_id(NULL) != NULL, "a NULL id returned a theme");

    /* The dark theme is the one the device has always drawn, and the id is what a saved
       preference and MESHCLIENT_THEME will carry, so it is a compatibility surface. */
    MESH_TEST_FAIL_IF(strcmp(mesh_ui_theme_default()->id, "dark") != 0,
                      "the default theme is no longer 'dark'");
    record_success(test_name);
}

/* Every theme has to answer for every role and be readable on its own ground. */
MESH_TEST_CASE(ui_theme_tables_are_readable, unit) {
    for (size_t i = 0; i < mesh_ui_theme_count(); ++i) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(i);
        char reason[128];
        if (!mesh_ui_theme_validate(theme, reason, sizeof reason)) {
            fprintf(stderr, "  theme '%s': %s\n", theme->id, reason);
            MESH_TEST_FAIL_IF(true, "a theme failed its readability contract");
        }

        /* Every tone has to resolve to a role a theme actually filled in, and the tones that
           mean opposite things must not come out the same colour - "connected" and "failed"
           telling you nothing apart is the whole failure the colour-blind theme is about. */
        const struct mesh_ui_rgb good = mesh_ui_theme_tone(theme, MESH_UI_TONE_GOOD);
        const struct mesh_ui_rgb bad = mesh_ui_theme_tone(theme, MESH_UI_TONE_BAD);
        MESH_TEST_FAIL_IF(good.r == bad.r && good.g == bad.g && good.b == bad.b,
                          "a theme draws good and bad in the same colour");
    }
    record_success(test_name);
}

/*
 * The avatar palettes.
 *
 * Every theme has to offer at least one tint, answer for any seed at all, and hand back only
 * colours it actually stated - a lookup that ran off the end of a short palette would draw a
 * conversation's disc in whatever zeroed bytes follow it, which on a dark theme is a black
 * disc with black initials.
 *
 * The readability of each tint against the ground is mesh_ui_theme_validate()'s job, and
 * ui_theme_tables_are_readable already runs it over the whole registry.
 */
MESH_TEST_CASE(ui_theme_avatar_palettes, unit) {
    for (size_t i = 0; i < mesh_ui_theme_count(); ++i) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(i);
        MESH_TEST_FAIL_IF(theme->avatar_count == 0U || theme->avatar_count > MESH_UI_AVATAR_TINTS,
                          "a theme states no avatar tints, or more than fit");

        /* Node numbers are consecutive off a vendor's block, so the seeds that matter are
           adjacent ones - and the palette has to spread them rather than hand a whole mesh the
           same colour. A theme offering more than one tint must use more than one here. */
        bool seen[MESH_UI_AVATAR_TINTS];
        memset(seen, 0, sizeof seen);
        for (uint32_t seed = 0x8F21B000U; seed < 0x8F21B040U; ++seed) {
            const struct mesh_ui_rgb tint = mesh_ui_theme_avatar(theme, seed);
            bool known = false;
            for (uint8_t slot = 0; slot < theme->avatar_count; ++slot) {
                if (tint.r == theme->avatars[slot].r && tint.g == theme->avatars[slot].g &&
                    tint.b == theme->avatars[slot].b) {
                    seen[slot] = true;
                    known = true;
                }
            }
            MESH_TEST_FAIL_IF(!known, "an avatar seed resolved to a colour the theme never stated");
        }
        uint8_t used = 0U;
        for (uint8_t slot = 0; slot < theme->avatar_count; ++slot) {
            used = (uint8_t)(used + (seen[slot] ? 1U : 0U));
        }
        MESH_TEST_FAIL_IF(theme->avatar_count > 1U && used < 2U,
                          "a run of neighbouring node numbers all got the same avatar tint");

        /* The same conversation is the same colour every time, which is the whole reason the
           seed is an identity rather than a name. */
        const struct mesh_ui_rgb once = mesh_ui_theme_avatar(theme, 0x3000U);
        const struct mesh_ui_rgb twice = mesh_ui_theme_avatar(theme, 0x3000U);
        MESH_TEST_FAIL_IF(once.r != twice.r || once.g != twice.g || once.b != twice.b,
                          "the same seed gave two different tints");
    }

    /* A theme that states no palette at all still has to answer, because the lookup is on the
       drawing path and a NULL there would be a blank screen rather than a wrong colour. */
    struct mesh_ui_theme bare = *mesh_ui_theme_default();
    bare.avatar_count = 0U;
    const struct mesh_ui_rgb fallback = mesh_ui_theme_avatar(&bare, 7U);
    const struct mesh_ui_rgb accent = mesh_ui_theme_color(&bare, MESH_UI_COLOR_ACCENT);
    MESH_TEST_FAIL_IF(fallback.r != accent.r || fallback.g != accent.g || fallback.b != accent.b,
                      "a theme with no palette should fall back to the accent it already owes");
    record_success(test_name);
}

/* A pair of colours far apart is a high ratio, a pair close together is near 1, and the extreme
   is exactly 21. The renderer never calls this, but every theme is admitted by it. */
MESH_TEST_CASE(ui_theme_contrast_is_the_wcag_ratio, unit) {
    const struct mesh_ui_rgb black = {0U, 0U, 0U};
    const struct mesh_ui_rgb white = {255U, 255U, 255U};
    const double extreme = mesh_ui_theme_contrast(black, white);
    MESH_TEST_FAIL_IF(extreme < 20.9 || extreme > 21.1, "black on white is not 21:1");
    MESH_TEST_FAIL_IF(mesh_ui_theme_contrast(white, black) != extreme,
                      "the ratio depends on the order of its arguments");
    MESH_TEST_FAIL_IF(mesh_ui_theme_contrast(white, white) != 1.0, "a colour on itself is not 1:1");
    record_success(test_name);
}

/* A theme that failed the contract has to say which pair failed, or the message is useless to
   whoever added it. Built here rather than registered, so no bad theme ships in the table. */
MESH_TEST_CASE(ui_theme_validate_rejects_an_unreadable_palette, unit) {
    struct mesh_ui_theme broken = *mesh_ui_theme_default();
    broken.colors[MESH_UI_COLOR_TEXT] = broken.colors[MESH_UI_COLOR_BG];

    char reason[128];
    reason[0] = '\0';
    MESH_TEST_FAIL_IF(mesh_ui_theme_validate(&broken, reason, sizeof reason),
                      "text drawn in the background colour passed validation");
    MESH_TEST_FAIL_IF(reason[0] == '\0', "validation failed without saying why");

    struct mesh_ui_theme no_font = *mesh_ui_theme_default();
    no_font.font_id = "not-a-font";
    MESH_TEST_FAIL_IF(mesh_ui_theme_validate(&no_font, reason, sizeof reason),
                      "a theme naming a font that does not exist passed validation");
    record_success(test_name);
}

/*
 * Cards: the geometry a theme owes them, and the contrast their text owes the fill.
 *
 * A card is the one component drawn on MESH_UI_COLOR_SURFACE rather than on the ground, so
 * every tone a card row can take needs a pair in the validation table - and the pair that is
 * easiest to lose is the *edge*, because on a theme whose surface sits a step off the ground
 * the hairline is the whole of what says a card is there at all.
 */
MESH_TEST_CASE(ui_theme_states_card_geometry, unit) {
    for (size_t i = 0; i < mesh_ui_theme_count(); ++i) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(i);
        const struct mesh_ui_metrics *metrics = mesh_ui_theme_metrics(theme);
        /* Zero padding is a card whose text touches its own edge, which is not a card. The
           radius may legitimately be zero: that is a theme asking for square corners. */
        MESH_TEST_FAIL_IF(metrics->card_pad == 0U, "a theme gives its cards no inset");
        /* Both are multiplied by the glyph scale, so a step is a whole cell of chrome. Two of
           them either way is already a quarter of a row on the Brick's panel; more than that is
           a theme spending its body rows on its own furniture. */
        MESH_TEST_FAIL_IF(metrics->card_pad > 4U, "a theme's card inset would eat the body");
        MESH_TEST_FAIL_IF(metrics->card_radius > 4U, "a theme's card corners are rounder than the "
                                                     "card");
    }

    char reason[128];
    struct mesh_ui_theme swallowed = *mesh_ui_theme_default();
    swallowed.colors[MESH_UI_COLOR_SURFACE] = swallowed.colors[MESH_UI_COLOR_ACCENT];
    reason[0] = '\0';
    MESH_TEST_FAIL_IF(mesh_ui_theme_validate(&swallowed, reason, sizeof reason),
                      "a card fill that swallows the accent heading passed validation");

    struct mesh_ui_theme edgeless = *mesh_ui_theme_default();
    edgeless.colors[MESH_UI_COLOR_RULE] = edgeless.colors[MESH_UI_COLOR_SURFACE];
    reason[0] = '\0';
    MESH_TEST_FAIL_IF(mesh_ui_theme_validate(&edgeless, reason, sizeof reason),
                      "a card edge invisible against its own fill passed validation");
    record_success(test_name);
}

/*
 * Cycling, which is the whole of what the Settings row does.
 *
 * Every theme has to be reachable by pressing A enough times, and pressing it once more from
 * the last one has to come back to the first - a user who has stepped somewhere unreadable
 * gets home the same way they left.
 */
MESH_TEST_CASE(ui_theme_cycles_through_every_theme, unit) {
    const size_t count = mesh_ui_theme_count();
    const struct mesh_ui_theme *theme = mesh_ui_theme_default();
    bool seen[16];
    memset(seen, 0, sizeof seen);
    MESH_TEST_FAIL_IF(count > (sizeof seen / sizeof seen[0]),
                      "more themes than this case can track; raise `seen`");

    for (size_t step = 0; step < count; ++step) {
        for (size_t i = 0; i < count; ++i) {
            if (mesh_ui_theme_at(i) == theme) {
                MESH_TEST_FAIL_IF(seen[i], "cycling revisited a theme before covering them all");
                seen[i] = true;
            }
        }
        theme = mesh_ui_theme_next(theme);
        MESH_TEST_FAIL_IF(theme == NULL, "cycling ran off the end of the registry");
    }
    for (size_t i = 0; i < count; ++i) {
        MESH_TEST_FAIL_IF(!seen[i], "cycling never reached one of the themes");
    }
    MESH_TEST_FAIL_IF(theme != mesh_ui_theme_default(),
                      "a full cycle did not come back to where it started");

    /* A theme that is not in the registry at all - a copy, say - lands somewhere usable
       rather than nowhere. */
    struct mesh_ui_theme stray = *mesh_ui_theme_default();
    MESH_TEST_FAIL_IF(mesh_ui_theme_next(&stray) != mesh_ui_theme_default(),
                      "cycling from an unregistered theme did not fall back to the default");
    record_success(test_name);
}

/* What a saved preference is read back through, and what the environment is asked with. */
MESH_TEST_CASE(ui_theme_resolves_ids_and_the_environment, unit) {
    MESH_TEST_FAIL_IF(mesh_ui_theme_resolve("light") != mesh_ui_theme_by_id("light"),
                      "a known id did not resolve to its theme");
    MESH_TEST_FAIL_IF(mesh_ui_theme_resolve("no-such-theme") != mesh_ui_theme_default(),
                      "an unknown id did not resolve to the default");
    MESH_TEST_FAIL_IF(mesh_ui_theme_resolve("") != mesh_ui_theme_default(),
                      "an empty id did not resolve to the default");
    MESH_TEST_FAIL_IF(mesh_ui_theme_resolve(NULL) != mesh_ui_theme_default(),
                      "a NULL id did not resolve to the default");

    /* mesh_ui_theme_env() answers NULL when nobody named one, which is what tells the app the
       choice is the user's to make rather than the environment's. */
    char saved[64];
    saved[0] = '\0';
    const char *const previous = getenv("MESHCLIENT_THEME");
    const bool had_env = (previous != NULL);
    if (had_env) {
        snprintf(saved, sizeof saved, "%s", previous);
    }

    (void)unsetenv("MESHCLIENT_THEME");
    MESH_TEST_FAIL_IF(mesh_ui_theme_env() != NULL, "an unset MESHCLIENT_THEME named a theme");
    MESH_TEST_FAIL_IF(mesh_ui_theme_from_env() != mesh_ui_theme_default(),
                      "an unset MESHCLIENT_THEME did not fall back to the default");

    (void)setenv("MESHCLIENT_THEME", "light", 1);
    MESH_TEST_FAIL_IF(mesh_ui_theme_env() != mesh_ui_theme_by_id("light"),
                      "MESHCLIENT_THEME did not name its theme");

    /* A typo must not leave a handheld with no UI, and must not read as a deliberate pin. */
    (void)setenv("MESHCLIENT_THEME", "not-a-theme", 1);
    MESH_TEST_FAIL_IF(mesh_ui_theme_env() != NULL, "an unknown MESHCLIENT_THEME named a theme");
    MESH_TEST_FAIL_IF(mesh_ui_theme_from_env() != mesh_ui_theme_default(),
                      "an unknown MESHCLIENT_THEME did not fall back to the default");

    if (had_env) {
        (void)setenv("MESHCLIENT_THEME", saved, 1);
    } else {
        (void)unsetenv("MESHCLIENT_THEME");
    }
    record_success(test_name);
}

/* Metrics are theme data, and the scale a theme asks for is clamped rather than trusted. */
MESH_TEST_CASE(ui_theme_scale_is_clamped, unit) {
    const struct mesh_ui_theme *theme = mesh_ui_theme_default();
    MESH_TEST_FAIL_IF(mesh_ui_theme_clamp_scale(theme, 0) != mesh_ui_theme_scale(theme),
                      "scale 0 is not the theme's own");
    MESH_TEST_FAIL_IF(mesh_ui_theme_clamp_scale(theme, 99) != MESH_UI_SCALE_MAX,
                      "an absurd scale was not clamped down");
    MESH_TEST_FAIL_IF(mesh_ui_theme_clamp_scale(theme, -4) != mesh_ui_theme_scale(theme),
                      "a negative scale is not the theme's own");
    MESH_TEST_FAIL_IF(mesh_ui_theme_clamp_scale(NULL, 1) != MESH_UI_SCALE_MIN,
                      "a NULL theme did not fall back to the default and clamp");

    /* Chrome is smaller than the body but never below the floor, whatever the body is at. */
    for (int scale = MESH_UI_SCALE_MIN; scale <= MESH_UI_SCALE_MAX; ++scale) {
        const int chrome = mesh_ui_theme_chrome_scale(theme, scale);
        MESH_TEST_FAIL_IF(chrome < MESH_UI_SCALE_MIN, "chrome text fell below the minimum scale");
        MESH_TEST_FAIL_IF(chrome > scale, "chrome text is bigger than the body text");
    }
    record_success(test_name);
}

/* The font is a seam, not a constant. Whatever is registered has to measure sanely, because
   every column count, button width and bubble height in the UI is derived from it. */
MESH_TEST_CASE(ui_theme_fonts_measure, unit) {
    MESH_TEST_FAIL_IF(mesh_ui_font_count() == 0U, "no fonts are registered");
    for (size_t i = 0; i < mesh_ui_font_count(); ++i) {
        const struct mesh_ui_font *font = mesh_ui_font_at(i);
        MESH_TEST_FAIL_IF(font == NULL || font->id == NULL, "a registered font is unusable");
        MESH_TEST_FAIL_IF(mesh_ui_font_by_id(font->id) != font,
                          "a font does not come back under its own id");
        MESH_TEST_FAIL_IF(font->width == 0U || font->width > MESH_UI_GLYPH_MAX_WIDTH,
                          "a font is wider than the glyph buffer");
        MESH_TEST_FAIL_IF(font->height == 0U || font->height > MESH_UI_GLYPH_MAX_HEIGHT,
                          "a font is taller than the glyph buffer");

        /* Advances have to grow with the multiplier, or every measurement above breaks. */
        MESH_TEST_FAIL_IF(mesh_ui_font_advance(font, 2) <= mesh_ui_font_advance(font, 1),
                          "the character advance does not grow with the scale");
        MESH_TEST_FAIL_IF(mesh_ui_font_line(font, 1) < (int)font->height,
                          "the line advance does not clear the cell");

        /* A glyph the font has, and one nothing has: both are drawable, one is the tofu. */
        struct mesh_ui_glyph glyph;
        MESH_TEST_FAIL_IF(!mesh_ui_font_glyph(font, (uint32_t)'A', &glyph),
                          "the font has no capital A");
        MESH_TEST_FAIL_IF(mesh_ui_font_glyph(font, 0x10FFFDU, &glyph),
                          "the font claims a private-use codepoint");
    }

    /* A theme always resolves to a font, even asking for one that does not exist. */
    struct mesh_ui_theme no_font = *mesh_ui_theme_default();
    no_font.font_id = "not-a-font";
    MESH_TEST_FAIL_IF(mesh_ui_theme_font(&no_font) != mesh_ui_font_default(),
                      "an unknown font id did not fall back to the default");
    MESH_TEST_FAIL_IF(mesh_ui_theme_font(NULL) == NULL, "a NULL theme resolved to no font");
    record_success(test_name);
}

/* The pixel at the corner of the panel: fb_render_snapshot() clears to the theme's ground
   before it draws, so this is the whole frame's background by construction. */
static bool corner_is(const uint8_t *pixels, struct mesh_ui_rgb color) {
    /* 32 bpp with every bitfield zero, which is what the capture fabricates: B,G,R,X. */
    return pixels[0] == color.b && pixels[1] == color.g && pixels[2] == color.r;
}

/*
 * The end of the whole exercise: one snapshot, two themes, two different frames.
 *
 * If a renderer holds a colour of its own, the ground still changes here but the frames stay
 * closer than they should - so the case checks both that the ground follows the theme and that
 * the frames differ at all.
 */
MESH_TEST_CASE(ui_theme_switch_repaints_the_frame, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_store_shutdown(&store), "no snapshot to render");

    struct mesh_ui_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, 320U, 240U, 2) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
    const size_t page_bytes = stride * (size_t)height;
    uint8_t *reference = malloc(page_bytes);
    MESH_TEST_FAIL_IF_CLEANUP(reference == NULL, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "out of memory");

#define THEME_CLEANUP                                                                              \
    free(reference);                                                                               \
    mesh_ui_capture_close(capture);                                                                \
    mesh_ui_store_shutdown(&store)

    const struct mesh_ui_theme *first = mesh_ui_theme_at(0U);
    mesh_ui_capture_set_theme(capture, first);
    mesh_ui_capture_set_scale(capture, 2);
    mesh_ui_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(!corner_is(pixels, mesh_ui_theme_color(first, MESH_UI_COLOR_BG)),
                              THEME_CLEANUP, "the frame is not drawn on the theme's background");
    memcpy(reference, pixels, page_bytes);

    for (size_t i = 1U; i < mesh_ui_theme_count(); ++i) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(i);
        mesh_ui_capture_set_theme(capture, theme);
        mesh_ui_capture_set_scale(capture, 2);
        MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_theme(capture) != theme, THEME_CLEANUP,
                                  "the capture did not take the theme it was given");
        mesh_ui_capture_render(capture, &snapshot);

        MESH_TEST_FAIL_IF_CLEANUP(!corner_is(pixels, mesh_ui_theme_color(theme, MESH_UI_COLOR_BG)),
                                  THEME_CLEANUP,
                                  "a theme's frame is not drawn on that theme's background");
        MESH_TEST_FAIL_IF_CLEANUP(memcmp(reference, pixels, page_bytes) == 0, THEME_CLEANUP,
                                  "two themes rendered the same snapshot identically");
    }

    /* And back: a theme switch is not one-way, and the frame it produces is the frame that
       theme produced before - which is what makes a switcher safe to leave in a menu. */
    mesh_ui_capture_set_theme(capture, first);
    mesh_ui_capture_set_scale(capture, 2);
    mesh_ui_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(memcmp(reference, pixels, page_bytes) != 0, THEME_CLEANUP,
                              "switching back did not reproduce the first frame");

#undef THEME_CLEANUP
    free(reference);
    mesh_ui_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * The switch, end to end: the app names a theme in the snapshot and the next frame is drawn in
 * it.
 *
 * This is the contract the Settings row rests on. Nothing pushes at the backend - the choice
 * rides in the client info like every other fact about this client, and the renderer picks it
 * up, which is what keeps backends a function of the snapshot.
 */
MESH_TEST_CASE(ui_theme_follows_the_snapshot, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_store_shutdown(&store), "no snapshot to render");

    struct mesh_ui_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, 320U, 240U, 2) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");
    mesh_ui_capture_set_theme(capture, mesh_ui_theme_default());
    mesh_ui_capture_set_scale(capture, 2);

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
    const size_t page_bytes = stride * (size_t)height;

#define SNAPSHOT_CLEANUP                                                                           \
    mesh_ui_capture_close(capture);                                                                \
    mesh_ui_store_shutdown(&store)

    /* A snapshot that names nothing leaves the capture drawing with what it was opened with -
       which is what lets the capture harness, where no app fills the client info, work at all. */
    mesh_ui_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(
        !corner_is(pixels, mesh_ui_theme_color(mesh_ui_theme_default(), MESH_UI_COLOR_BG)),
        SNAPSHOT_CLEANUP, "an unnamed theme did not leave the capture's own in place");

    /* Now the app names one. Every other theme in the registry has to arrive this way. */
    for (size_t i = 0; i < mesh_ui_theme_count(); ++i) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(i);
        snprintf(snapshot.settings.client.theme, sizeof snapshot.settings.client.theme, "%s",
                 theme->id);
        mesh_ui_capture_render(capture, &snapshot);
        MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_theme(capture) != theme, SNAPSHOT_CLEANUP,
                                  "the renderer did not adopt the theme the snapshot named");
        MESH_TEST_FAIL_IF_CLEANUP(!corner_is(pixels, mesh_ui_theme_color(theme, MESH_UI_COLOR_BG)),
                                  SNAPSHOT_CLEANUP,
                                  "the frame is not drawn on the named theme's background");
    }

    /*
     * A scale the caller named survives all of that.
     *
     * A theme carries a glyph multiplier, so adopting one from a snapshot could quietly swap
     * an explicit capture scale for that theme's default - which would silently mis-size every
     * screenshot a caller asked for at a particular size. Proven by rendering the same theme
     * at the pinned scale directly and comparing the pages, because the scale is not otherwise
     * visible from out here.
     */
    const struct mesh_ui_theme *const last = mesh_ui_capture_theme(capture);
    uint8_t *pinned = malloc(page_bytes);
    MESH_TEST_FAIL_IF_CLEANUP(pinned == NULL, SNAPSHOT_CLEANUP, "out of memory");
    memcpy(pinned, pixels, page_bytes);

    mesh_ui_capture_set_theme(capture, last);
    mesh_ui_capture_set_scale(capture, 2);
    mesh_ui_capture_render(capture, &snapshot);
    const bool scale_held = (memcmp(pinned, pixels, page_bytes) == 0);
    free(pinned);
    MESH_TEST_FAIL_IF_CLEANUP(!scale_held, SNAPSHOT_CLEANUP,
                              "following a snapshot's theme discarded the caller's scale");

    /* An id from a build that had more themes than this one is ignored rather than obeyed, and
       the frame stays readable in whatever was already up. */
    const struct mesh_ui_theme *before = mesh_ui_capture_theme(capture);
    snprintf(snapshot.settings.client.theme, sizeof snapshot.settings.client.theme, "%s",
             "solarized");
    mesh_ui_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_theme(capture) != before, SNAPSHOT_CLEANUP,
                              "an unknown theme id changed what the frame is drawn with");

#undef SNAPSHOT_CLEANUP
    mesh_ui_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}

/*
 * A theme carries its own glyph multiplier, so taking a theme takes its scale too - otherwise a
 * large-text theme would render at whatever size the theme before it asked for.
 */
MESH_TEST_CASE(ui_theme_carries_its_own_scale, unit) {
    struct mesh_ui_store store;
    MESH_TEST_FAIL_IF(mesh_ui_store_init(&store) != 0, "store init failed");
    mesh_test_nav_populate(&store);

    struct mesh_ui_snapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    mesh_ui_store_request_refresh(&store);
    MESH_TEST_FAIL_IF_CLEANUP(!mesh_ui_store_consume_updates(&store, &snapshot),
                              mesh_ui_store_shutdown(&store), "no snapshot to render");

    struct mesh_ui_capture *capture = NULL;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_open(&capture, 320U, 240U, 0) != 0,
                              mesh_ui_store_shutdown(&store), "capture open failed");

    uint32_t width = 0U;
    uint32_t height = 0U;
    size_t stride = 0U;
    const uint8_t *pixels = mesh_ui_capture_pixels(capture, &width, &height, &stride);
    const size_t page_bytes = stride * (size_t)height;
    uint8_t *at_theme_scale = malloc(page_bytes);
    MESH_TEST_FAIL_IF_CLEANUP(at_theme_scale == NULL, mesh_ui_capture_close(capture);
                              mesh_ui_store_shutdown(&store), "out of memory");

#define SCALE_CLEANUP                                                                              \
    free(at_theme_scale);                                                                          \
    mesh_ui_capture_close(capture);                                                                \
    mesh_ui_store_shutdown(&store)

    /* Named rather than inherited: mesh_ui_capture_open() honours MESHCLIENT_THEME, so a run
       with it set would otherwise start from a different theme - and a different scale - than
       the one this case switches back to. */
    const struct mesh_ui_theme *theme = mesh_ui_theme_default();
    mesh_ui_capture_set_theme(capture, theme);
    mesh_ui_capture_render(capture, &snapshot);
    memcpy(at_theme_scale, pixels, page_bytes);

    /* A different multiplier is a different frame - if it were not, the check below would pass
       whatever set_theme() did with the scale. */
    mesh_ui_capture_set_scale(capture, MESH_UI_SCALE_MAX);
    mesh_ui_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(memcmp(at_theme_scale, pixels, page_bytes) == 0, SCALE_CLEANUP,
                              "two glyph scales rendered identically");

    /* Taking the theme again takes its scale with it, so the frame comes back. */
    mesh_ui_capture_set_theme(capture, theme);
    mesh_ui_capture_render(capture, &snapshot);
    MESH_TEST_FAIL_IF_CLEANUP(memcmp(at_theme_scale, pixels, page_bytes) != 0, SCALE_CLEANUP,
                              "taking a theme did not take the scale it asks for");

#undef SCALE_CLEANUP
    free(at_theme_scale);
    mesh_ui_capture_close(capture);
    mesh_ui_store_shutdown(&store);
    record_success(test_name);
}
