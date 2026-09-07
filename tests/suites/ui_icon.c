#define _POSIX_C_SOURCE 200809L

/* The monochrome icon sprites: the table, the decoder, and the contract a screen relies on. */

#include "framework/mesh_test.h"

#include "mesh/ui/icon.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/*
 * Every id the enum carries has sprite data behind it.
 *
 * The enum comes from include/mesh/ui/icons.def and the sprites are generated from the same
 * file, so the two can only disagree when somebody adds a line and does not re-run
 * scripts/gen-icons.py - which the compiler is perfectly happy with, and which shows up on the
 * panel as one icon drawn in place of another for every id after the new one.
 */
MESH_TEST_CASE(icon_table_covers_every_id, unit) {
    const struct mesh_ui_icon_table *table = &mesh_ui_icon_table;
    char message[128];

    for (int i = 0; i < (int)MESH_UI_ICON_COUNT; ++i) {
        const enum mesh_ui_icon icon = (enum mesh_ui_icon)i;
        const uint32_t start = table->run_offsets[i];
        const uint32_t end = table->run_offsets[i + 1];
        MESH_TEST_FAIL_IF(end < start, "run offsets should not go backwards");

        /* The runs of one icon cover its whole sprite exactly. A short icon would leave the
           decoder's tail zeroed - a symbol with its bottom rows missing - and a long one would
           spill into the next id. */
        size_t pixels = 0;
        for (uint32_t run = start; run < end; ++run) {
            pixels += table->runs[run * 2U];
        }
        if (pixels != (size_t)MESH_UI_ICON_SIZE * MESH_UI_ICON_SIZE) {
            snprintf(message, sizeof message, "%s covers %zu pixels, expected %d",
                     i == 0 ? "MESH_UI_ICON_NONE" : mesh_ui_icon_name(icon), pixels,
                     MESH_UI_ICON_SIZE * MESH_UI_ICON_SIZE);
            record_failure(test_name, message);
            return;
        }

        for (uint32_t run = start; run < end; ++run) {
            if (table->runs[run * 2U] == 0U) {
                record_failure(test_name, "a run of zero pixels wastes two bytes and draws none");
                return;
            }
            if (table->runs[run * 2U + 1U] > MESH_UI_ICON_MAX_ALPHA) {
                record_failure(test_name, "coverage should fit in four bits");
                return;
            }
        }
    }
    record_success(test_name);
}

/* An icon that draws nothing is a mistake nobody sees until the panel is in front of them:
   an empty slot and a blank sprite look the same from every layer above this one. */
MESH_TEST_CASE(icon_sprites_have_ink, unit) {
    uint8_t alpha[MESH_UI_ICON_SIZE * MESH_UI_ICON_SIZE];
    char message[128];

    for (int i = 1; i < (int)MESH_UI_ICON_COUNT; ++i) {
        const enum mesh_ui_icon icon = (enum mesh_ui_icon)i;
        mesh_ui_icon_alpha(icon, alpha);

        size_t ink = 0;
        for (size_t p = 0; p < sizeof alpha; ++p) {
            ink += alpha[p];
        }
        /* Measured against the symbol's own box rather than the whole sprite: the sprite is a
           window a little wider than Material's grid, and the air around the outside is not
           somewhere a symbol was ever going to put ink. Counting it would make these bounds a
           fact about how much padding the generator crops with. */
        const size_t body = (size_t)MESH_UI_ICON_SIZE * MESH_UI_ICON_BODY / MESH_UI_ICON_WINDOW;
        const size_t full = body * body * MESH_UI_ICON_MAX_ALPHA;
        /*
         * Coverage rather than solid pixels: the thinnest symbols in the set are strokes - the
         * chevron, the check, the space bar - and most of a 2 px stroke scaled to 16 is partial
         * coverage, so counting only solid pixels would hold a chevron to a standard a chevron
         * cannot meet. The two failures worth catching are at the ends: a glyph name the font
         * answered with nothing, and one it answered with the missing-glyph box.
         */
        if (ink < full / 20U || ink > full * 9U / 10U) {
            snprintf(message, sizeof message,
                     "%s covers %zu%% of the symbol's box, which is not a symbol",
                     mesh_ui_icon_name(icon), ink * 100U / full);
            record_failure(test_name, message);
            return;
        }
    }
    record_success(test_name);
}

/*
 * No sprite is a picture of a symbol with a slice taken off it.
 *
 * The generator crops a window out of the glyph it rasterises, and where that window sits is
 * arithmetic nobody can check by eye afterwards: a 32x32 table of hex is not a picture. It has
 * been wrong once - the window was placed off the drawing origin, which this face puts on the
 * ascender rather than on the em box the symbol is drawn in, so every sprite in the set came out
 * a tenth of an em low and lost its bottom rows. On the panel that reads as a slightly flat
 * chevron and a gear with no teeth underneath, which is exactly the kind of wrong that ships.
 *
 * Two things a cut leaves behind, and neither is a thing a symbol with air around it has:
 *
 *   - a *saturated* edge, where the crop went through the middle of a shape. Faint coverage on
 *     the outermost pixel is fine and expected - the widest symbols in the set (`hub`, the
 *     antenna) reach the grid's edge, and resampling spreads them a little further.
 *   - ink that sits off-centre, because a window that cut one side has moved the whole symbol
 *     towards the other.
 */
MESH_TEST_CASE(icon_sprites_are_not_cropped, unit) {
    uint8_t alpha[MESH_UI_ICON_SIZE * MESH_UI_ICON_SIZE];
    char message[160];
    const int last = MESH_UI_ICON_SIZE - 1;

    for (int i = 1; i < (int)MESH_UI_ICON_COUNT; ++i) {
        const enum mesh_ui_icon icon = (enum mesh_ui_icon)i;
        mesh_ui_icon_alpha(icon, alpha);

        int min_x = last;
        int max_x = 0;
        int min_y = last;
        int max_y = 0;
        int edge = 0;
        for (int y = 0; y <= last; ++y) {
            for (int x = 0; x <= last; ++x) {
                const int a = alpha[y * MESH_UI_ICON_SIZE + x];
                if (a == 0) {
                    continue;
                }
                if (x == 0 || x == last || y == 0 || y == last) {
                    edge = a > edge ? a : edge;
                }
                min_x = x < min_x ? x : min_x;
                max_x = x > max_x ? x : max_x;
                min_y = y < min_y ? y : min_y;
                max_y = y > max_y ? y : max_y;
            }
        }

        if (edge > MESH_UI_ICON_MAX_ALPHA / 2) {
            snprintf(message, sizeof message,
                     "%s is solid at the sprite's edge (%d of %d): the window cut it",
                     mesh_ui_icon_name(icon), edge, MESH_UI_ICON_MAX_ALPHA);
            record_failure(test_name, message);
            return;
        }

        /* Doubled so the centre of an even-sided sprite is a whole number: 31 for 32 px. The
           tolerance is two pixels' worth, which the asymmetric symbols in the set - the
           bluetooth rune, the backspace arrow - sit inside and a cut edge does not. */
        const int centre_x = min_x + max_x;
        const int centre_y = min_y + max_y;
        const int slack = 4;
        if (centre_x < last - slack || centre_x > last + slack || centre_y < last - slack ||
            centre_y > last + slack) {
            snprintf(message, sizeof message,
                     "%s sits at (%d, %d) rather than (%d, %d): the window is off-centre",
                     mesh_ui_icon_name(icon), centre_x, centre_y, last, last);
            record_failure(test_name, message);
            return;
        }
    }
    record_success(test_name);
}

/* The rule every icon slot leans on: a zeroed slot is an empty one. */
MESH_TEST_CASE(icon_none_is_blank, unit) {
    uint8_t alpha[MESH_UI_ICON_SIZE * MESH_UI_ICON_SIZE];
    memset(alpha, 0xFF, sizeof alpha);
    mesh_ui_icon_alpha(MESH_UI_ICON_NONE, alpha);
    for (size_t p = 0; p < sizeof alpha; ++p) {
        MESH_TEST_FAIL_IF(alpha[p] != 0U, "MESH_UI_ICON_NONE should decode to nothing");
    }

    MESH_TEST_FAIL_IF(mesh_ui_icon_is_valid(MESH_UI_ICON_NONE),
                      "MESH_UI_ICON_NONE is the absence of an icon, not one of them");
    MESH_TEST_FAIL_IF(mesh_ui_icon_name(MESH_UI_ICON_NONE)[0] != '\0',
                      "MESH_UI_ICON_NONE should have no glyph name");

    /* An id from a newer build - a snapshot replayed, a capture scene naming an icon this
       binary does not have - decodes to nothing rather than reading past the table. */
    memset(alpha, 0xFF, sizeof alpha);
    mesh_ui_icon_alpha((enum mesh_ui_icon)MESH_UI_ICON_COUNT, alpha);
    for (size_t p = 0; p < sizeof alpha; ++p) {
        MESH_TEST_FAIL_IF(alpha[p] != 0U, "an unknown icon should decode to nothing");
    }
    MESH_TEST_FAIL_IF(mesh_ui_icon_name((enum mesh_ui_icon)MESH_UI_ICON_COUNT)[0] != '\0',
                      "an unknown icon should have no glyph name");
    record_success(test_name);
}

/* Names are what the generator matched in the font and what a failure message can print. Two
   ids sharing one is not an error, but a blank one means the catalog and the table have
   parted company. */
MESH_TEST_CASE(icon_names_are_present, unit) {
    for (int i = 1; i < (int)MESH_UI_ICON_COUNT; ++i) {
        const char *name = mesh_ui_icon_name((enum mesh_ui_icon)i);
        MESH_TEST_FAIL_IF(name[0] == '\0', "every icon should name the glyph it was drawn from");
        MESH_TEST_FAIL_IF(!mesh_ui_icon_is_valid((enum mesh_ui_icon)i),
                          "every id in the enum should be drawable");
    }
    record_success(test_name);
}
