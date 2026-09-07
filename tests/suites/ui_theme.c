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

#include "mesh/ui/anim.h"
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
        const struct mesh_ui_rgb good = mesh_ui_theme_tone(theme, MESH_UI_TONE_SUCCESS);
        const struct mesh_ui_rgb bad = mesh_ui_theme_tone(theme, MESH_UI_TONE_ERROR);
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
    const struct mesh_ui_rgb accent = mesh_ui_theme_color(&bare, MESH_UI_COLOR_PRIMARY);
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
/*
 * The tone a level has earned - the one sentence the airtime figure, the card heading and the
 * meter under them all speak, so that a number and the picture of it cannot disagree.
 */
MESH_TEST_CASE(ui_theme_tone_for_load_bands, unit) {
    MESH_TEST_FAIL_IF(mesh_ui_tone_for_load(0, 250, 500) != MESH_UI_TONE_SUCCESS,
                      "nothing used should be good news");
    MESH_TEST_FAIL_IF(mesh_ui_tone_for_load(249, 250, 500) != MESH_UI_TONE_SUCCESS,
                      "just under the warning is still good");
    /* Both thresholds are inclusive lower bounds, which is the half of this most likely to be
       got wrong later: exactly 25% of the air is already a mesh worth looking at. */
    MESH_TEST_FAIL_IF(mesh_ui_tone_for_load(250, 250, 500) != MESH_UI_TONE_WARNING,
                      "the warning threshold itself should warn");
    MESH_TEST_FAIL_IF(mesh_ui_tone_for_load(499, 250, 500) != MESH_UI_TONE_WARNING,
                      "just under the bad threshold is still a warning");
    MESH_TEST_FAIL_IF(mesh_ui_tone_for_load(500, 250, 500) != MESH_UI_TONE_ERROR,
                      "the bad threshold itself should be bad");
    MESH_TEST_FAIL_IF(mesh_ui_tone_for_load(1000, 250, 500) != MESH_UI_TONE_ERROR,
                      "a full track is bad news");

    /* Thresholds handed over backwards must not make the middle band unreachable: a screen
       permanently in the red reads as a mesh in trouble rather than as a caller's typo. */
    MESH_TEST_FAIL_IF(mesh_ui_tone_for_load(300, 500, 250) != MESH_UI_TONE_WARNING,
                      "swapped thresholds should still band the middle");

    /* Every tone it can answer with names a family, which is exactly what a meter is allowed
       to be filled with: mesh_ui_theme_validate() holds every family against the track and
       fb_draw_meter() folds anything that names none back to the primary. The two are one
       contract and this is where it is checked. */
    for (int32_t level = 0; level <= MESH_UI_ANIM_ONE; level += 50) {
        const enum mesh_ui_tone tone = mesh_ui_tone_for_load(level, 250, 500);
        MESH_TEST_FAIL_IF(mesh_ui_tone_family(tone) == MESH_UI_FAMILY_COUNT,
                          "a load tone escaped the families a meter is validated for");
    }
    record_success(test_name);
}

/*
 * A meter is a track with a fill in it, and a theme that loses either half loses the widget:
 * an unfindable track is a bar that vanishes when the reading is low, a fill that matches its
 * track is one that vanishes when the reading is high.
 */
MESH_TEST_CASE(ui_theme_validate_holds_the_meter_pairs, unit) {
    char reason[128];

    struct mesh_ui_theme flat = *mesh_ui_theme_default();
    flat.colors[MESH_UI_COLOR_SUCCESS] = flat.colors[MESH_UI_COLOR_METER_TRACK];
    reason[0] = '\0';
    MESH_TEST_FAIL_IF(mesh_ui_theme_validate(&flat, reason, sizeof reason),
                      "a meter fill the colour of its own track passed validation");
    MESH_TEST_FAIL_IF(reason[0] == '\0', "validation failed without saying why");

    struct mesh_ui_theme invisible_track = *mesh_ui_theme_default();
    invisible_track.colors[MESH_UI_COLOR_METER_TRACK] = invisible_track.colors[MESH_UI_COLOR_BG];
    MESH_TEST_FAIL_IF(mesh_ui_theme_validate(&invisible_track, reason, sizeof reason),
                      "a meter track the colour of the ground passed validation");

    /* And on a card, which is the half a borrowed SURFACE_SEL could not hold: a track validated
       against the body and invisible on a surface is a bar that exists on one screen. */
    struct mesh_ui_theme invisible_on_card = *mesh_ui_theme_default();
    invisible_on_card.colors[MESH_UI_COLOR_METER_TRACK] =
        invisible_on_card.colors[MESH_UI_COLOR_SURFACE];
    MESH_TEST_FAIL_IF(mesh_ui_theme_validate(&invisible_on_card, reason, sizeof reason),
                      "a meter track the colour of a card passed validation");

    /* And the geometry half: a bar with no thickness draws nothing at all, which is the one
       way a theme can turn the widget off without saying so. */
    struct mesh_ui_theme thin = *mesh_ui_theme_default();
    thin.metrics.meter_thickness = 0U;
    MESH_TEST_FAIL_IF(mesh_ui_theme_validate(&thin, reason, sizeof reason),
                      "a theme drawing meters no pixels tall passed validation");
    record_success(test_name);
}

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
MESH_TEST_CASE(ui_theme_states_its_geometry, unit) {
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
        /* Also in glyph-scale steps, and bounded from both ends: nothing is an invisible bar,
           and a bar as tall as the text beside it is a block, not a meter. */
        MESH_TEST_FAIL_IF(metrics->meter_thickness == 0U, "a theme draws meters no pixels tall");
        MESH_TEST_FAIL_IF(metrics->meter_thickness > 3U,
                          "a theme's meter is as tall as the row it sits in");
        /*
         * The shape scale has to be a scale: rounder as it goes up, and never so round that a
         * corner eats the row it belongs to. A flat table - every shape the same radius - is
         * legal and is what an entirely square theme looks like; what is not legal is a large
         * shape squarer than a small one, because then a screen naming MESH_UI_SHAPE_LG gets
         * something less round than one naming MESH_UI_SHAPE_SM and the vocabulary is lying.
         */
        for (int shape = MESH_UI_SHAPE_NONE; shape < MESH_UI_SHAPE_FULL; ++shape) {
            MESH_TEST_FAIL_IF(metrics->shape[shape] > 4U,
                              "a theme's corners are rounder than the box they are on");
            if (shape > MESH_UI_SHAPE_NONE) {
                MESH_TEST_FAIL_IF(metrics->shape[shape] < metrics->shape[shape - 1],
                                  "a theme's shape scale gets squarer as it goes up");
            }
        }
        MESH_TEST_FAIL_IF(metrics->shape[MESH_UI_SHAPE_NONE] != 0U,
                          "a theme rounds the corners of MESH_UI_SHAPE_NONE");
    }

    /*
     * The radius accessor: steps times the scale, and the pill answering with something the
     * fill primitive will clamp rather than with a length of its own.
     *
     * The clamp is the contract worth pinning. fb_fill_round_rect() takes half the shorter side
     * when a radius overshoots it, so MESH_UI_SHAPE_FULL only has to be bigger than any box it
     * could be handed - and a number that merely looks big (a hundred pixels, say) stops being
     * big the day somebody draws a full-screen panel.
     */
    const struct mesh_ui_theme *shaped = mesh_ui_theme_default();
    const int scale = mesh_ui_theme_scale(shaped);
    const struct mesh_ui_metrics *shaped_metrics = mesh_ui_theme_metrics(shaped);
    for (int shape = MESH_UI_SHAPE_NONE; shape < MESH_UI_SHAPE_FULL; ++shape) {
        const int want = (int)shaped_metrics->shape[shape] * scale;
        MESH_TEST_FAIL_IF(mesh_ui_theme_radius(shaped, (enum mesh_ui_shape)shape, scale) != want,
                          "a shape's radius is not its step count times the glyph scale");
    }
    MESH_TEST_FAIL_IF(mesh_ui_theme_radius(shaped, MESH_UI_SHAPE_FULL, scale) < 4096,
                      "a pill's radius is small enough for a panel to outgrow it");
    /* A NULL theme resolves to the default, like every other lookup here, and a shape outside
       the enum is square rather than undefined - a renderer with a stale enum draws a box, not
       a corner of garbage. */
    MESH_TEST_FAIL_IF(mesh_ui_theme_radius(NULL, MESH_UI_SHAPE_MD, scale) !=
                          mesh_ui_theme_radius(shaped, MESH_UI_SHAPE_MD, scale),
                      "a NULL theme did not resolve to the default for a radius");
    MESH_TEST_FAIL_IF(
        mesh_ui_theme_radius(shaped, (enum mesh_ui_shape)MESH_UI_SHAPE_COUNT, scale) != 0,
        "a shape outside the scale was not square");

    char reason[128];
    struct mesh_ui_theme swallowed = *mesh_ui_theme_default();
    swallowed.colors[MESH_UI_COLOR_SURFACE] = swallowed.colors[MESH_UI_COLOR_PRIMARY];
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
MESH_TEST_CASE(ui_theme_states_its_type_scale, unit) {
    for (size_t i = 0; i < mesh_ui_theme_count(); ++i) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(i);

        for (int scale = MESH_UI_SCALE_MIN; scale <= MESH_UI_SCALE_MAX; ++scale) {
            const int title = mesh_ui_theme_type_scale(theme, MESH_UI_TYPE_TITLE, scale);
            const int body = mesh_ui_theme_type_scale(theme, MESH_UI_TYPE_BODY, scale);
            const int label = mesh_ui_theme_type_scale(theme, MESH_UI_TYPE_LABEL, scale);

            /* Every role stays inside what the font registry can rasterise. This is the whole
               reason the table holds offsets and the accessor clamps: a title one step above a
               body already at the maximum is a size nothing can draw. */
            MESH_TEST_FAIL_IF(title < MESH_UI_SCALE_MIN || title > MESH_UI_SCALE_MAX,
                              "a title scale fell outside the drawable range");
            MESH_TEST_FAIL_IF(label < MESH_UI_SCALE_MIN || label > MESH_UI_SCALE_MAX,
                              "a label scale fell outside the drawable range");

            /* The body role is the body scale by definition - it is the zero the other two are
               offsets from, and a theme that moved it would be renaming the scale. */
            MESH_TEST_FAIL_IF(body != mesh_ui_theme_clamp_scale(theme, scale),
                              "the body role is not the body scale");

            /*
             * The ordering is the vocabulary. A screen naming TITLE must never get something
             * smaller than one naming BODY, and BODY never smaller than LABEL - at the ends of
             * the range they collapse onto each other, which is the scale degrading rather than
             * inverting.
             */
            MESH_TEST_FAIL_IF(title < body, "a title is drawn smaller than the body");
            MESH_TEST_FAIL_IF(body < label, "a label is drawn larger than the body");
        }

        /* At the top of the range the title has nowhere to go and collapses onto the body; at
           the bottom the label does. Pinned because it is the behaviour a caller relies on
           instead of a bounds check of its own. */
        MESH_TEST_FAIL_IF(mesh_ui_theme_type_scale(theme, MESH_UI_TYPE_TITLE, MESH_UI_SCALE_MAX) !=
                              MESH_UI_SCALE_MAX,
                          "a title at the maximum scale did not collapse onto it");
        MESH_TEST_FAIL_IF(mesh_ui_theme_type_scale(theme, MESH_UI_TYPE_LABEL, MESH_UI_SCALE_MIN) !=
                              MESH_UI_SCALE_MIN,
                          "a label at the minimum scale did not collapse onto it");
    }

    /* Out of range answers the body scale rather than reading past the table, and NULL is the
       default theme as everywhere else in this header. */
    const struct mesh_ui_theme *theme = mesh_ui_theme_default();
    const int body = mesh_ui_theme_type_scale(theme, MESH_UI_TYPE_BODY, 4);
    MESH_TEST_FAIL_IF(mesh_ui_theme_type_scale(theme, (enum mesh_ui_type) - 1, 4) != body,
                      "a negative type role read something");
    MESH_TEST_FAIL_IF(mesh_ui_theme_type_scale(theme, MESH_UI_TYPE_COUNT, 4) != body,
                      "a type role past the end read something");
    MESH_TEST_FAIL_IF(mesh_ui_theme_type_scale(NULL, MESH_UI_TYPE_TITLE, 4) !=
                          mesh_ui_theme_type_scale(theme, MESH_UI_TYPE_TITLE, 4),
                      "a NULL theme did not fall back to the default");
    record_success(test_name);
}

MESH_TEST_CASE(ui_theme_states_its_spacing, unit) {
    for (size_t i = 0; i < mesh_ui_theme_count(); ++i) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(i);
        const int scale = mesh_ui_theme_scale(theme);

        MESH_TEST_FAIL_IF(mesh_ui_theme_space(theme, MESH_UI_SPACE_NONE, scale) != 0,
                          "MESH_UI_SPACE_NONE is not nothing");

        int previous = 0;
        for (int space = MESH_UI_SPACE_XS; space < MESH_UI_SPACE_COUNT; ++space) {
            const int gap = mesh_ui_theme_space(theme, (enum mesh_ui_space)space, scale);
            /* A theme that asked for a gap gets at least a pixel of one, however small the
               scale: a half-step rounded away to zero is an inset that silently stops
               existing. */
            MESH_TEST_FAIL_IF(gap < 1, "a spacing step rounded away to nothing");
            /* And it has to be a scale, for the reason the shape steps do - a widget naming MD
               and getting less room than one naming SM is a vocabulary that lies. */
            MESH_TEST_FAIL_IF(gap < previous, "a theme's spacing scale gets tighter as it goes up");
            /* Nothing in the scale is a whole row: these are gaps between things, not rows.
               Four steps is already taller than the glyph cell at any scale, so a table that
               reaches it is a theme spending body rows on its own furniture. */
            MESH_TEST_FAIL_IF(gap > 4 * scale,
                              "a theme's spacing step is as tall as the row it separates");
            previous = gap;
        }

        /* The scale is glyph-relative, so it grows with the text. A theme whose gaps did not
           move when the glyph scale did would be the literals this replaced. */
        MESH_TEST_FAIL_IF(mesh_ui_theme_space(theme, MESH_UI_SPACE_MD, MESH_UI_SCALE_MAX) <
                              mesh_ui_theme_space(theme, MESH_UI_SPACE_MD, MESH_UI_SCALE_MIN),
                          "a spacing step did not grow with the glyph scale");
    }

    const struct mesh_ui_theme *theme = mesh_ui_theme_default();
    MESH_TEST_FAIL_IF(mesh_ui_theme_space(theme, (enum mesh_ui_space) - 1, 4) != 0,
                      "a negative spacing token read something");
    MESH_TEST_FAIL_IF(mesh_ui_theme_space(theme, MESH_UI_SPACE_COUNT, 4) != 0,
                      "a spacing token past the end read something");
    MESH_TEST_FAIL_IF(mesh_ui_theme_space(NULL, MESH_UI_SPACE_SM, 4) !=
                          mesh_ui_theme_space(theme, MESH_UI_SPACE_SM, 4),
                      "a NULL theme did not fall back to the default");
    record_success(test_name);
}

MESH_TEST_CASE(ui_theme_states_its_motion, unit) {
    for (size_t i = 0; i < mesh_ui_theme_count(); ++i) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(i);

        uint32_t previous = 0U;
        for (int motion = MESH_UI_MOTION_SHORT; motion <= MESH_UI_MOTION_LONG; ++motion) {
            const uint32_t ms = mesh_ui_theme_motion(theme, (enum mesh_ui_motion)motion);
            /* A duration of zero is "already there", which is a theme with no motion at all
               rather than a theme with a broken token. Nothing here is allowed to be that by
               accident, so the three transition tokens are held to a range a human can see and
               will not wait for. */
            MESH_TEST_FAIL_IF(ms < 60U, "a theme's transition is too short to be seen");
            MESH_TEST_FAIL_IF(ms > 600U, "a theme's transition is long enough to wait for");
            /*
             * The three have to be a scale, for the reason the shape steps do: a widget naming
             * MESH_UI_MOTION_SHORT and getting something slower than MESH_UI_MOTION_LONG is a
             * vocabulary that lies, and the asymmetry the snackbar depends on - out shorter
             * than in - is exactly this ordering.
             */
            MESH_TEST_FAIL_IF(motion > MESH_UI_MOTION_SHORT && ms <= previous,
                              "a theme's motion scale does not get longer as it goes up");
            previous = ms;
        }

        /* The loop is not a transition and is not on that scale: it is one pass of something
           with no end, and it has to be long enough to read as travel rather than as flicker. */
        const uint32_t loop = mesh_ui_theme_motion(theme, MESH_UI_MOTION_LOOP);
        MESH_TEST_FAIL_IF(loop <= previous, "a theme's loop is shorter than a transition");
        MESH_TEST_FAIL_IF(loop < 600U, "a theme's indeterminate loop reads as flicker");
    }

    /* Out of range answers 0 rather than reading past the table - the same contract the shape
       accessor has, and what keeps a token added to the enum but not to a theme from being a
       buffer overrun instead of a still control. */
    const struct mesh_ui_theme *theme = mesh_ui_theme_default();
    MESH_TEST_FAIL_IF(mesh_ui_theme_motion(theme, (enum mesh_ui_motion) - 1) != 0U,
                      "a negative motion token read something");
    MESH_TEST_FAIL_IF(mesh_ui_theme_motion(theme, MESH_UI_MOTION_COUNT) != 0U,
                      "a motion token past the end read something");
    /* NULL is the default theme, as it is everywhere else in this header. */
    MESH_TEST_FAIL_IF(mesh_ui_theme_motion(NULL, MESH_UI_MOTION_SHORT) !=
                          mesh_ui_theme_motion(theme, MESH_UI_MOTION_SHORT),
                      "a NULL theme did not fall back to the default");
    record_success(test_name);
}

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
        const int chrome = mesh_ui_theme_type_scale(theme, MESH_UI_TYPE_LABEL, scale);
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
        /* The master is what the coverage is stored at, and it is what the resampler indexes
           with - a font declaring one bigger than the buffer would read off the end of it. */
        MESH_TEST_FAIL_IF(font->master_w == 0U || font->master_w > MESH_UI_GLYPH_MASTER_MAX_WIDTH,
                          "a font's master is wider than the glyph buffer");
        MESH_TEST_FAIL_IF(font->master_h == 0U || font->master_h > MESH_UI_GLYPH_MASTER_MAX_HEIGHT,
                          "a font's master is taller than the glyph buffer");
        MESH_TEST_FAIL_IF(font->sampling != MESH_UI_FONT_PIXEL &&
                              font->sampling != MESH_UI_FONT_SMOOTH,
                          "a font asks for a sampling this layer does not have");

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

        /*
         * Coverage, not a mask. Every value has to be inside the ramp the renderer quantises
         * against - one above it indexes past the blend table - and a capital A has to carry
         * some, or the font is drawing nothing and reporting success.
         */
        (void)mesh_ui_font_glyph(font, (uint32_t)'A', &glyph);
        bool inked = false;
        bool in_range = true;
        for (size_t px = 0; px < (size_t)font->master_w * (size_t)font->master_h; ++px) {
            if (glyph.alpha[px] > MESH_UI_GLYPH_MAX_ALPHA) {
                in_range = false;
            }
            if (glyph.alpha[px] > 0U) {
                inked = true;
            }
        }
        MESH_TEST_FAIL_IF(!in_range, "a glyph carries coverage above the ramp");
        MESH_TEST_FAIL_IF(!inked, "a capital A has no coverage at all");

        /* A pixel font is a mask stored as coverage: every value is off or solid, which is what
           lets the resampler block-replicate it and land on exactly the old spans. */
        if (font->sampling == MESH_UI_FONT_PIXEL) {
            bool binary = true;
            for (size_t px = 0; px < (size_t)font->master_w * (size_t)font->master_h; ++px) {
                if (glyph.alpha[px] != 0U && glyph.alpha[px] != MESH_UI_GLYPH_MAX_ALPHA) {
                    binary = false;
                }
            }
            MESH_TEST_FAIL_IF(!binary, "a pixel font carries partial coverage");
        }
    }

    /* A theme always resolves to a font, even asking for one that does not exist. */
    struct mesh_ui_theme no_font = *mesh_ui_theme_default();
    no_font.font_id = "not-a-font";
    MESH_TEST_FAIL_IF(mesh_ui_theme_font(&no_font) != mesh_ui_font_default(),
                      "an unknown font id did not fall back to the default");
    MESH_TEST_FAIL_IF(mesh_ui_theme_font(NULL) == NULL, "a NULL theme resolved to no font");
    record_success(test_name);
}

/*
 * The colour of the frame's ground, sampled from the right-hand edge halfway down.
 *
 * Neither corner works, and where the sample has had to move twice is itself the argument for
 * this one. It was the top-left until the tab strip grew a recessed surface across the first
 * rows; it was the bottom-left until the footer became an action bar and grew the same surface
 * across the last ones. The chrome now frames the body top and bottom, so the only pixels left
 * that are still the ground on every screen are the ones the *body* never reaches: rows fill to
 * `xres - margin` and the scroll rail lives in the half-margin gutter but stops a quarter of one
 * short of the edge, so the last column is untouched by anything a screen draws.
 */
static bool ground_is(const uint8_t *pixels, size_t stride, uint32_t width, uint32_t height,
                      struct mesh_ui_rgb color) {
    const uint8_t *px = pixels + stride * (size_t)(height / 2U) + 4U * (size_t)(width - 1U);
    /* 32 bpp with every bitfield zero, which is what the capture fabricates: B,G,R,X. */
    return px[0] == color.b && px[1] == color.g && px[2] == color.r;
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
    MESH_TEST_FAIL_IF_CLEANUP(
        !ground_is(pixels, stride, width, height, mesh_ui_theme_color(first, MESH_UI_COLOR_BG)),
        THEME_CLEANUP, "the frame is not drawn on the theme's background");
    memcpy(reference, pixels, page_bytes);

    for (size_t i = 1U; i < mesh_ui_theme_count(); ++i) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(i);
        mesh_ui_capture_set_theme(capture, theme);
        mesh_ui_capture_set_scale(capture, 2);
        MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_theme(capture) != theme, THEME_CLEANUP,
                                  "the capture did not take the theme it was given");
        mesh_ui_capture_render(capture, &snapshot);

        MESH_TEST_FAIL_IF_CLEANUP(
            !ground_is(pixels, stride, width, height, mesh_ui_theme_color(theme, MESH_UI_COLOR_BG)),
            THEME_CLEANUP, "a theme's frame is not drawn on that theme's background");
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
        !ground_is(pixels, stride, width, height,
                   mesh_ui_theme_color(mesh_ui_theme_default(), MESH_UI_COLOR_BG)),
        SNAPSHOT_CLEANUP, "an unnamed theme did not leave the capture's own in place");

    /* Now the app names one. Every other theme in the registry has to arrive this way. */
    for (size_t i = 0; i < mesh_ui_theme_count(); ++i) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(i);
        snprintf(snapshot.settings.client.theme, sizeof snapshot.settings.client.theme, "%s",
                 theme->id);
        mesh_ui_capture_render(capture, &snapshot);
        MESH_TEST_FAIL_IF_CLEANUP(mesh_ui_capture_theme(capture) != theme, SNAPSHOT_CLEANUP,
                                  "the renderer did not adopt the theme the snapshot named");
        MESH_TEST_FAIL_IF_CLEANUP(
            !ground_is(pixels, stride, width, height, mesh_ui_theme_color(theme, MESH_UI_COLOR_BG)),
            SNAPSHOT_CLEANUP, "the frame is not drawn on the named theme's background");
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

/*
 * The families are an index, not a switch: mesh_ui_family_role() multiplies, and the enum is
 * laid out so that it can. The _Static_asserts in the header pin the three corners of that
 * layout; this walks the whole grid, because an enum that is right at the corners and wrong in
 * the middle would build and then paint the warning colour onto the success rows.
 */
MESH_TEST_CASE(ui_theme_family_slots_are_an_index, unit) {
    for (int f = 0; f < MESH_UI_FAMILY_COUNT; ++f) {
        const enum mesh_ui_family family = (enum mesh_ui_family)f;
        for (int sl = 0; sl < MESH_UI_SLOT_COUNT; ++sl) {
            const enum mesh_ui_color role = mesh_ui_family_role(family, (enum mesh_ui_slot)sl);
            MESH_TEST_FAIL_IF(role >= MESH_UI_COLOR_COUNT, "a family slot left the palette");
            MESH_TEST_FAIL_IF((int)role != (int)MESH_UI_COLOR_PRIMARY + f * MESH_UI_SLOT_COUNT + sl,
                              "a family slot is not where the arithmetic says it is");
        }
        /* Tone and family name the same thing from two directions, and a round trip that lost
           its way would silently recolour every screen that speaks tones. */
        MESH_TEST_FAIL_IF(mesh_ui_tone_family(mesh_ui_family_tone(family)) != family,
                          "a family did not survive the trip through its tone");
        MESH_TEST_FAIL_IF(mesh_ui_tone_role(mesh_ui_family_tone(family)) !=
                              mesh_ui_family_role(family, MESH_UI_SLOT_BASE),
                          "a family tone did not resolve to its own base");
    }

    /* The neutral three name no family - which is the answer fb_draw_meter() acts on. */
    MESH_TEST_FAIL_IF(mesh_ui_tone_family(MESH_UI_TONE_NORMAL) != MESH_UI_FAMILY_COUNT,
                      "the normal tone claimed a family");
    MESH_TEST_FAIL_IF(mesh_ui_tone_family(MESH_UI_TONE_DIM) != MESH_UI_FAMILY_COUNT,
                      "the dim tone claimed a family");
    MESH_TEST_FAIL_IF(mesh_ui_tone_family(MESH_UI_TONE_STRONG) != MESH_UI_FAMILY_COUNT,
                      "the strong tone claimed a family");

    /* Nonsense answers with the primary rather than with nothing, for the reason an unknown
       MESHCLIENT_THEME warns rather than failing: a bad enum must not leave a widget unpainted. */
    MESH_TEST_FAIL_IF(mesh_ui_family_role((enum mesh_ui_family) - 1, MESH_UI_SLOT_BASE) !=
                          MESH_UI_COLOR_PRIMARY,
                      "an out-of-range family did not fall back to the primary");
    MESH_TEST_FAIL_IF(mesh_ui_family_role(MESH_UI_FAMILY_ERROR, (enum mesh_ui_slot)99) !=
                          MESH_UI_COLOR_ERROR,
                      "an out-of-range slot did not fall back to the base");
    record_success(test_name);
}

/*
 * The state layer replaced five stated roles with one operation, so the operation has to hold
 * the properties those roles were picked by hand to have: it moves the fill towards its own
 * ink, it moves further when pressed than when selected, and it never arrives.
 *
 * The last is the one worth a test. A layer that overshot would hand a widget the ink colour as
 * its fill, which is a control that vanishes at exactly the moment it is being used.
 */
MESH_TEST_CASE(ui_theme_state_layer_moves_towards_the_ink, unit) {
    const struct mesh_ui_rgb fill = {10, 20, 30};
    const struct mesh_ui_rgb ink = {250, 240, 230};

    const struct mesh_ui_rgb rest = mesh_ui_theme_state_layer(fill, ink, MESH_UI_STATE_REST);
    MESH_TEST_FAIL_IF(rest.r != fill.r || rest.g != fill.g || rest.b != fill.b,
                      "resting is not the fill untouched");

    const struct mesh_ui_rgb sel = mesh_ui_theme_state_layer(fill, ink, MESH_UI_STATE_SELECTED);
    const struct mesh_ui_rgb act = mesh_ui_theme_state_layer(fill, ink, MESH_UI_STATE_ACTIVE);
    MESH_TEST_FAIL_IF(sel.r <= fill.r || sel.g <= fill.g || sel.b <= fill.b,
                      "the selected layer did not move towards the ink");
    MESH_TEST_FAIL_IF(act.r <= sel.r || act.g <= sel.g || act.b <= sel.b,
                      "pressed is not a step further than selected");
    MESH_TEST_FAIL_IF(act.r >= ink.r || act.g >= ink.g || act.b >= ink.b,
                      "the layer reached the ink, so the fill and its label are one colour");

    /* On a light palette the same call has to darken, which is the whole reason the layer mixes
       the *ink* in rather than white: one rule, both directions, neither spelled out. */
    const struct mesh_ui_rgb pale = {245, 245, 245};
    const struct mesh_ui_rgb dark_ink = {20, 20, 20};
    const struct mesh_ui_rgb pale_sel =
        mesh_ui_theme_state_layer(pale, dark_ink, MESH_UI_STATE_SELECTED);
    MESH_TEST_FAIL_IF(pale_sel.r >= pale.r, "the layer lightened a light fill");

    /* Two colours a hair apart still have to separate: truncating the mix towards zero is how a
       state layer becomes a no-op on exactly the themes whose tiers are closest together. */
    const struct mesh_ui_rgb near_a = {100, 100, 100};
    const struct mesh_ui_rgb near_b = {104, 104, 104};
    const struct mesh_ui_rgb nudged =
        mesh_ui_theme_state_layer(near_a, near_b, MESH_UI_STATE_SELECTED);
    MESH_TEST_FAIL_IF(nudged.r == near_a.r, "a near-flat pair produced no state layer at all");
    record_success(test_name);
}

/*
 * mesh_ui_theme_paint() exists so that a fill and the label on it can only ever be chosen
 * together. This is that promise: for every theme, family and slot, what comes back is a pair
 * validate() has already held to 4.5:1 - selected included, because the state layer moves the
 * fill towards the ink and so can only ever cost contrast.
 */
MESH_TEST_CASE(ui_theme_paint_returns_a_validated_pair, unit) {
    static const enum mesh_ui_slot k_slots[] = {MESH_UI_SLOT_BASE, MESH_UI_SLOT_CONTAINER};
    static const enum mesh_ui_state k_states[] = {MESH_UI_STATE_REST, MESH_UI_STATE_SELECTED,
                                                  MESH_UI_STATE_ACTIVE};
    for (size_t i = 0; i < mesh_ui_theme_count(); ++i) {
        const struct mesh_ui_theme *theme = mesh_ui_theme_at(i);
        for (int f = 0; f < MESH_UI_FAMILY_COUNT; ++f) {
            for (size_t sl = 0; sl < sizeof k_slots / sizeof k_slots[0]; ++sl) {
                for (size_t st = 0; st < sizeof k_states / sizeof k_states[0]; ++st) {
                    const struct mesh_ui_paint paint = mesh_ui_theme_paint(
                        theme, (enum mesh_ui_family)f, k_slots[sl], k_states[st]);
                    /* A base takes no state layer at all - it is already the full-strength end
                       a pressed control commits to - so every state must answer with the same
                       fill there. Anything else is a saturated pair being diluted. */
                    if (k_slots[sl] == MESH_UI_SLOT_BASE) {
                        const struct mesh_ui_rgb base =
                            mesh_ui_theme_family(theme, (enum mesh_ui_family)f, MESH_UI_SLOT_BASE);
                        MESH_TEST_FAIL_IF(paint.fill.r != base.r || paint.fill.g != base.g ||
                                              paint.fill.b != base.b,
                                          "a state layer was applied to a family's base");
                    }
                    const double ratio = mesh_ui_theme_contrast(paint.ink, paint.fill);
                    char why[160];
                    snprintf(why, sizeof why, "%s family %d slot %d state %d is %.2f:1", theme->id,
                             f, (int)k_slots[sl], (int)k_states[st], ratio);
                    MESH_TEST_FAIL_IF(ratio + 0.005 < 4.5, why);
                }
            }
        }
        /* Naming the ink half selects the same pair, so a caller cannot invert one by asking
           for it the other way round. */
        const struct mesh_ui_paint by_fill = mesh_ui_theme_paint(
            theme, MESH_UI_FAMILY_ERROR, MESH_UI_SLOT_CONTAINER, MESH_UI_STATE_REST);
        const struct mesh_ui_paint by_ink = mesh_ui_theme_paint(
            theme, MESH_UI_FAMILY_ERROR, MESH_UI_SLOT_ON_CONTAINER, MESH_UI_STATE_REST);
        MESH_TEST_FAIL_IF(by_fill.fill.r != by_ink.fill.r || by_fill.ink.r != by_ink.ink.r,
                          "asking for a slot by its ink returned a different pair");
    }
    record_success(test_name);
}

/*
 * The two contracts that only exist because the palette has families, and that nothing else
 * would catch: a theme may not hand two verdicts one colour, and it may not hide a marker bar
 * by making a family the colour of the cursor fill it is laid under.
 */
MESH_TEST_CASE(ui_theme_validate_holds_the_family_contracts, unit) {
    char reason[128];

    /* Success and error the same colour passes every contrast rule there is - both are perfectly
       readable - and makes "connected" and "failed" indistinguishable. */
    struct mesh_ui_theme collapsed = *mesh_ui_theme_default();
    collapsed.colors[MESH_UI_COLOR_WARNING] = collapsed.colors[MESH_UI_COLOR_ERROR];
    reason[0] = '\0';
    MESH_TEST_FAIL_IF(mesh_ui_theme_validate(&collapsed, reason, sizeof reason),
                      "a theme with one colour for warning and error passed validation");
    MESH_TEST_FAIL_IF(reason[0] == '\0', "validation failed without saying why");

    /* A family whose base is the cursor fill draws a marker bar nobody can see - which is what
       the contrast theme's secondary and tertiary would have been, left as white. */
    struct mesh_ui_theme invisible_marker = *mesh_ui_theme_default();
    invisible_marker.colors[MESH_UI_COLOR_TERTIARY] =
        invisible_marker.colors[MESH_UI_COLOR_SURFACE_SEL];
    MESH_TEST_FAIL_IF(mesh_ui_theme_validate(&invisible_marker, reason, sizeof reason),
                      "a family the colour of the cursor fill passed validation");

    /* A container with no ink checked against it is the failure the four-slot family exists to
       make impossible, so it has to be caught for every family and not just the primary. */
    struct mesh_ui_theme flat_container = *mesh_ui_theme_default();
    flat_container.colors[MESH_UI_COLOR_ON_SUCCESS_CONTAINER] =
        flat_container.colors[MESH_UI_COLOR_SUCCESS_CONTAINER];
    MESH_TEST_FAIL_IF(mesh_ui_theme_validate(&flat_container, reason, sizeof reason),
                      "a container and its own ink in one colour passed validation");
    record_success(test_name);
}
