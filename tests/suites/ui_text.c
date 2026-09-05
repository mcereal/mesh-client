#define _POSIX_C_SOURCE 200809L

/* Cell-based text measurement, the 5x7 font and the emoji tables. */

#include "framework/mesh_test.h"

#include "mesh/ui/emoji.h"
#include "mesh/ui/font5x7.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The generator caps sequences well below this; it is a sanity bound, not a limit. */
#define EMOJI_TEST_MAX_SEQUENCE 16U

/* The font has to answer for every codepoint, and has to say which answers are real: the
   difference decides whether a name reads as itself or as a row of boxes. */
MESH_TEST_CASE(font5x7_coverage, unit) {
    struct mesh_font_glyph glyph;

    MESH_TEST_FAIL_IF(!mesh_font5x7_glyph('A', &glyph) || glyph.columns[0] == 0U,
                      "ASCII 'A' should have a glyph");
    /* An unaccented letter never reaches into the line gap. */
    for (int col = 0; col < MESH_FONT_WIDTH; ++col) {
        MESH_TEST_FAIL_IF(glyph.above[col] != 0U, "'A' should not draw above its cell");
    }

    /* Lowercase leaves rows 0 and 1 clear, so an accent fits inside the cell: e-acute is the
       'e' glyph with extra bits in those rows and nothing hanging above. */
    struct mesh_font_glyph base;
    struct mesh_font_glyph accented;
    MESH_TEST_FAIL_IF(!mesh_font5x7_glyph('e', &base) || !mesh_font5x7_glyph(0x00E9U, &accented),
                      "e and e-acute should both have glyphs");
    bool mark_in_cell = false;
    for (int col = 0; col < MESH_FONT_WIDTH; ++col) {
        if ((accented.columns[col] & ~base.columns[col]) != 0U) {
            mark_in_cell = true;
        }
        if (accented.above[col] != 0U) {
            record_failure(test_name, "a lowercase accent should fit inside the cell");
            return;
        }
    }
    MESH_TEST_FAIL_IF(!mark_in_cell, "e-acute should differ from e");

    /* Capitals occupy all seven rows, so their mark goes into the gap above instead. */
    MESH_TEST_FAIL_IF(!mesh_font5x7_glyph(0x00C9U, &accented), "E-acute should have a glyph");
    bool mark_above = false;
    for (int col = 0; col < MESH_FONT_WIDTH; ++col) {
        if (accented.above[col] != 0U) {
            mark_above = true;
        }
    }
    if (!mark_above) {
        record_failure(test_name, "an uppercase accent should hang above the cell");
        return;
    }

    /* Emoji and anything else outside the font report false and draw the replacement box -
       one box, because the caller now hands over codepoints rather than bytes. */
    struct {
        const char *label;
        uint32_t codepoint;
        bool covered;
    } cases[] = {
        {"latin-1 o-diaeresis", 0x00F6U, true},
        {"latin extended-a s-caron", 0x0161U, true},
        {"o with stroke", 0x00F8U, true},
        {"curly apostrophe", 0x2019U, true},
        {"non-breaking space", 0x00A0U, true},
        {"evergreen tree emoji", 0x1F332U, false},
        {"satellite antenna emoji", 0x1F4E1U, false},
        {"cjk", 0x4E2DU, false},
    };
    struct mesh_font_glyph tofu;
    (void)mesh_font5x7_glyph(0x1F600U, &tofu);
    bool tofu_visible = false;
    for (int col = 0; col < MESH_FONT_WIDTH; ++col) {
        if (tofu.columns[col] != 0U) {
            tofu_visible = true;
        }
    }
    if (!tofu_visible) {
        record_failure(test_name, "the replacement box should be visible");
        return;
    }

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        if (mesh_font5x7_glyph(cases[i].codepoint, &glyph) != cases[i].covered ||
            mesh_font5x7_has_glyph(cases[i].codepoint) != cases[i].covered) {
            record_failure(test_name, cases[i].label);
            return;
        }
        /* Everything the font cannot draw gets the same box, so an unreadable name reads as
           "characters I do not have" rather than as a different kind of noise per character. */
        const bool is_box = memcmp(glyph.columns, tofu.columns, sizeof glyph.columns) == 0;
        MESH_TEST_FAIL_IF(is_box == cases[i].covered, cases[i].label);
    }

    record_success(test_name);
}

/*
 * A cell is what the framebuffer draws in one column, and it is neither a byte nor always a
 * codepoint. Layout and drawing share this walker, so if it disagrees with itself a line
 * measures one width and draws another.
 */
MESH_TEST_CASE(ui_text_cells, unit) {
    struct {
        const char *label;
        const char *text;
        size_t cells;
    } cases[] = {
        {"ascii", "Trail", 5U},
        {"accented", "Jos\xC3\xA9", 4U},
        /* One four-byte emoji is one cell - the bug this all started with. */
        {"single emoji", "\xF0\x9F\x8C\xB2", 1U},
        {"emoji and text", "\xF0\x9F\x8C\xB2 Pine", 6U},
        /* A flag is a pair of regional indicators and draws as one glyph, not two letters. */
        {"flag", "\xF0\x9F\x87\xB5\xF0\x9F\x87\xB7", 1U},
        {"two flags", "\xF0\x9F\x87\xB5\xF0\x9F\x87\xB7\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8", 2U},
        /* A variation selector has no width of its own: this used to draw as two boxes. */
        {"vs16", "\xE2\x9B\xB0\xEF\xB8\x8F", 1U},
        /* Skin tone modifiers attach to the emoji before them. */
        {"skin tone", "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", 1U},
        /* A ZWJ family is one glyph. */
        {"zwj family", "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7",
         1U},
        {"empty", "", 0U},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        MESH_TEST_FAIL_IF(mesh_ui_text_cells(cases[i].text) != cases[i].cells, cases[i].label);
        /* The offset of the last cell has to land on the NUL, or measuring and clipping
           disagree and a clipped line loses or keeps half a character. */
        const size_t end = mesh_ui_text_cell_offset(cases[i].text, cases[i].cells);
        MESH_TEST_FAIL_IF(cases[i].text[end] != '\0', cases[i].label);
    }

    /* Clipping never lands inside a cell: two of three trees survive whole. */
    char line[32];
    snprintf(line, sizeof line, "%s", "\xF0\x9F\x8C\xB2\xF0\x9F\x8F\xA0\xF0\x9F\x9A\x97");
    mesh_ui_text_cell_truncate(line, 2U);
    MESH_TEST_FAIL_IF(strcmp(line, "\xF0\x9F\x8C\xB2\xF0\x9F\x8F\xA0") != 0,
                      "truncate split a cell");

    /* A flag is never split into the two letters it is spelled with. */
    snprintf(line, sizeof line, "%s", "\xF0\x9F\x87\xB5\xF0\x9F\x87\xB7x");
    mesh_ui_text_cell_truncate(line, 1U);
    MESH_TEST_FAIL_IF(strcmp(line, "\xF0\x9F\x87\xB5\xF0\x9F\x87\xB7") != 0,
                      "truncate split a flag into regional indicators");

    record_success(test_name);
}

/* What each cell resolves to: a sprite, or a font glyph. The digits are the interesting part. */
MESH_TEST_CASE(ui_text_cell_kinds, unit) {
    struct {
        const char *label;
        const char *text;
        bool is_emoji;
        size_t bytes;
    } cases[] = {
        {"letter", "A", false, 1U},
        {"tree", "\xF0\x9F\x8C\xB2", true, 4U},
        {"flag consumes both indicators", "\xF0\x9F\x87\xB5\xF0\x9F\x87\xB7", true, 8U},
        {"vs16 consumed with its base", "\xE2\x9B\xB0\xEF\xB8\x8F", true, 6U},
        /*
         * The emoji font claims the digits, '#' and '*' because they lead keycap sequences.
         * A bare digit has to stay a digit - "K9" is a name, not a keycap - while the keycap
         * spelling, digit plus selector plus U+20E3, has to reach the sprite.
         */
        {"bare digit is text", "9", false, 1U},
        {"hash is text", "#", false, 1U},
        {"keycap is emoji", "9\xEF\xB8\x8F\xE2\x83\xA3", true, 7U},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const struct mesh_ui_text_cell cell = mesh_ui_text_cell_next(cases[i].text);
        MESH_TEST_FAIL_IF(cell.is_emoji != cases[i].is_emoji || cell.bytes != cases[i].bytes,
                          cases[i].label);
    }

    /* Every sprite id a match hands back has to be in range and decode to something. */
    uint16_t sprite = 0;
    const uint32_t tree[] = {0x1F332U};
    MESH_TEST_FAIL_IF(mesh_emoji_match(tree, 1U, &sprite) != 1U, "the evergreen should match");
    uint8_t pixels[MESH_EMOJI_SIZE * MESH_EMOJI_SIZE];
    mesh_emoji_decode(sprite, pixels);
    bool opaque = false;
    for (size_t i = 0; i < sizeof pixels; ++i) {
        uint8_t rgb[3];
        if (mesh_emoji_color(pixels[i], rgb)) {
            opaque = true;
        }
    }
    if (!opaque) {
        record_failure(test_name, "a decoded sprite should have opaque pixels");
        return;
    }

    /* Nothing in the Latin ranges the text font covers may be stolen by the emoji table. */
    for (uint32_t cp = 0x20U; cp < 0x180U; ++cp) {
        if (!mesh_font5x7_has_glyph(cp)) {
            continue;
        }
        char utf8[5] = {0};
        if (cp < 0x80U) {
            utf8[0] = (char)cp;
        } else if (cp < 0x800U) {
            utf8[0] = (char)(0xC0U | (cp >> 6));
            utf8[1] = (char)(0x80U | (cp & 0x3FU));
        }
        if (mesh_ui_text_cell_next(utf8).is_emoji) {
            record_failure(test_name, "the emoji table stole a character the font can draw");
            return;
        }
    }

    record_success(test_name);
}

/* Every sprite the tables point at has to decode inside its bounds. A generated table that
   went out of sync with the runtime would otherwise read past the run array on some rare
   emoji nobody tests by hand. */
MESH_TEST_CASE(emoji_table_integrity, unit) {
    const struct mesh_emoji_table *table = &mesh_emoji_table;

    MESH_TEST_FAIL_IF(table->single_count == 0U || table->sequence_count == 0U,
                      "the emoji table is empty");

    for (uint32_t i = 1; i < table->single_count; ++i) {
        MESH_TEST_FAIL_IF(table->singles[i - 1U].codepoint >= table->singles[i].codepoint,
                          "singles are not sorted, so bisecting them is wrong");
    }

    for (uint32_t i = 1; i < table->sequence_count; ++i) {
        const struct mesh_emoji_sequence *previous = &table->sequences[i - 1U];
        const struct mesh_emoji_sequence *current = &table->sequences[i];
        MESH_TEST_FAIL_IF(previous->first > current->first,
                          "sequences are not sorted by their first codepoint");
        /* Longest first within a leading codepoint is what makes the match greedy. */
        MESH_TEST_FAIL_IF(previous->first == current->first && previous->length < current->length,
                          "sequences are not ordered longest first");
    }

    uint8_t pixels[MESH_EMOJI_SIZE * MESH_EMOJI_SIZE];
    for (uint32_t i = 0; i < table->single_count; ++i) {
        mesh_emoji_decode(table->singles[i].sprite, pixels);
    }
    for (uint32_t i = 0; i < table->sequence_count; ++i) {
        const struct mesh_emoji_sequence *entry = &table->sequences[i];
        MESH_TEST_FAIL_IF(entry->length < 2U || entry->length > EMOJI_TEST_MAX_SEQUENCE,
                          "a sequence has an implausible length");
        mesh_emoji_decode(entry->sprite, pixels);
    }

    record_success(test_name);
}
