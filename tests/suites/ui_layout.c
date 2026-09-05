#define _POSIX_C_SOURCE 200809L

/*
 * The backend-agnostic layout primitives: the cell-measured line builder and the scroll window.
 *
 * These are worth their own suite because they are where the byte/cell confusion used to live.
 * Every case here uses a real multi-byte cell - U+2B50, the star the Nodes tab marks a pinned
 * node with, which is three bytes and one column - so a helper that quietly counted bytes fails
 * loudly instead of only misbehaving on somebody's emoji-named radio.
 */

#include "framework/mesh_test.h"

#include "mesh/ui/emoji.h"
#include "mesh/ui/layout.h"

#include <string.h>

#define STAR "\xE2\xAD\x90" /* U+2B50, three bytes, one drawn cell */

MESH_TEST_CASE(layout_line_builds_and_measures, unit) {
    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    MESH_TEST_FAIL_IF(mesh_ui_line_width(&line) != 0U, "a reset line should be empty");

    mesh_ui_line_printf(&line, "%s", "ab");
    mesh_ui_line_printf(&line, "%s%d", STAR, 7);
    MESH_TEST_FAIL_IF(strcmp(mesh_ui_line_text(&line), "ab" STAR "7") != 0,
                      "printf should append rather than replace");
    MESH_TEST_FAIL_IF(mesh_ui_line_width(&line) != 4U,
                      "an emoji should measure one column, not three");

    /* Clipping happens on a cell boundary, so the star survives whole or not at all. */
    mesh_ui_line_fit(&line, 3U);
    MESH_TEST_FAIL_IF(strcmp(mesh_ui_line_text(&line), "ab" STAR) != 0,
                      "fit should cut on a cell boundary");
    record_success(test_name);
}

/*
 * The bug this exists to prevent: "%-4s" pads to four *bytes*, so a one-emoji short name comes
 * out of it one column wide and the value column beside it no longer lines up.
 */
MESH_TEST_CASE(layout_line_column_pads_by_cells, unit) {
    struct mesh_ui_line line;

    mesh_ui_line_reset(&line);
    mesh_ui_line_column(&line, STAR, 4U);
    MESH_TEST_FAIL_IF(mesh_ui_line_width(&line) != 4U,
                      "a one-cell emoji should be padded to the full column");
    MESH_TEST_FAIL_IF(strcmp(mesh_ui_line_text(&line), STAR "   ") != 0,
                      "padding should be three spaces after a one-cell name");

    /* Short of the column it pads; past it, it clips - both measured in cells. */
    mesh_ui_line_reset(&line);
    mesh_ui_line_column(&line, "Battery", 4U);
    MESH_TEST_FAIL_IF(strcmp(mesh_ui_line_text(&line), "Batt") != 0,
                      "a label wider than its column should be clipped to it");

    /* A column appended after existing text is measured from where it starts, not from zero. */
    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", "* ");
    mesh_ui_line_column(&line, "ab", 4U);
    mesh_ui_line_printf(&line, "%s", "|");
    MESH_TEST_FAIL_IF(strcmp(mesh_ui_line_text(&line), "* ab  |") != 0,
                      "a column should be relative to the text already in the line");
    record_success(test_name);
}

/* A metric on the right-hand end of a row has to land at the last column exactly, whatever the
   name beside it is spelled with. */
MESH_TEST_CASE(layout_line_right_aligns_by_cells, unit) {
    struct mesh_ui_line line;

    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", "Andy");
    mesh_ui_line_right(&line, 20U, "-7.5dB 3m");
    MESH_TEST_FAIL_IF(mesh_ui_line_width(&line) != 20U,
                      "a right-aligned row should fill exactly the columns it was given");
    MESH_TEST_FAIL_IF(strcmp(mesh_ui_line_text(&line), "Andy       -7.5dB 3m") != 0,
                      "the metric should sit flush against the right edge");

    /* Nine emoji are nine columns and twenty-seven bytes: aligning on the byte count would
       push the metric a long way off the panel. */
    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", STAR STAR STAR STAR STAR STAR STAR STAR STAR);
    mesh_ui_line_right(&line, 20U, "3m");
    MESH_TEST_FAIL_IF(mesh_ui_line_width(&line) != 20U,
                      "an emoji name should not push a right-aligned metric off the row");

    /* A name too long for the row is what clips, not the metric. */
    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", "a name far longer than the row is wide");
    mesh_ui_line_right(&line, 20U, "3m");
    MESH_TEST_FAIL_IF(mesh_ui_line_width(&line) != 20U, "an over-long name should be clipped");
    MESH_TEST_FAIL_IF(strcmp(mesh_ui_line_text(&line) + 18, "3m") != 0,
                      "the metric should survive an over-long name");

    /* No metric is just a clip. */
    mesh_ui_line_reset(&line);
    mesh_ui_line_printf(&line, "%s", "abcdef");
    mesh_ui_line_right(&line, 4U, "");
    MESH_TEST_FAIL_IF(strcmp(mesh_ui_line_text(&line), "abcd") != 0,
                      "an empty right half should leave a plain clip");
    record_success(test_name);
}

/*
 * Overflowing the builder must not leave half a UTF-8 sequence behind. snprintf truncates on a
 * byte, and a stray lead byte would draw as a replacement box and would be malformed on the
 * paths that also log or serialise the line.
 */
MESH_TEST_CASE(layout_line_overflow_stays_valid_utf8, unit) {
    struct mesh_ui_line line;
    mesh_ui_line_reset(&line);
    for (int i = 0; i < 400; ++i) {
        mesh_ui_line_printf(&line, "%s", STAR);
    }

    const size_t bytes = strlen(mesh_ui_line_text(&line));
    MESH_TEST_FAIL_IF(bytes >= MESH_UI_LINE_MAX, "the builder should stay inside its buffer");
    MESH_TEST_FAIL_IF(bytes % 3U != 0U, "a truncated append should not leave a partial sequence");
    MESH_TEST_FAIL_IF(mesh_ui_text_cells(mesh_ui_line_text(&line)) != bytes / 3U,
                      "every byte left in the line should belong to a whole star");

    /* Padding cannot overflow it either. */
    mesh_ui_line_pad_to(&line, 4096U);
    MESH_TEST_FAIL_IF(strlen(mesh_ui_line_text(&line)) >= MESH_UI_LINE_MAX,
                      "padding should stop at the end of the buffer");
    record_success(test_name);
}

/* The window arithmetic every screen used to write out by hand. */
MESH_TEST_CASE(layout_list_window_follows_cursor, unit) {
    /* Everything fits: no scrolling, whatever the cursor is doing. */
    MESH_TEST_FAIL_IF(mesh_ui_list_first_visible(4U, 5U, 10U) != 0U,
                      "a list that fits should never scroll");
    /* The cursor is inside the first window. */
    MESH_TEST_FAIL_IF(mesh_ui_list_first_visible(2U, 20U, 5U) != 0U,
                      "a cursor inside the first window should not scroll it");
    /* Past it, the window follows so the cursor sits on the last visible row. */
    MESH_TEST_FAIL_IF(mesh_ui_list_first_visible(5U, 20U, 5U) != 1U,
                      "the window should follow the cursor by one row at a time");
    /* At the end, the window stops rather than running off the list. */
    MESH_TEST_FAIL_IF(mesh_ui_list_first_visible(19U, 20U, 5U) != 15U,
                      "the last window should end on the last item");
    /* A body with no room draws nothing rather than dividing by zero. */
    MESH_TEST_FAIL_IF(mesh_ui_list_first_visible(3U, 20U, 0U) != 0U,
                      "a zero-row body should not scroll");
    record_success(test_name);
}

MESH_TEST_CASE(layout_list_iterates_its_window, unit) {
    /* A cursor past the end is clamped here, which is the check every renderer repeated. */
    struct mesh_ui_list list = mesh_ui_list_begin(3U, 99U, 10U);
    MESH_TEST_FAIL_IF(list.cursor != 2U,
                      "an out-of-range cursor should be clamped to the last row");

    uint32_t index = 0U;
    uint32_t seen = 0U;
    while (mesh_ui_list_next(&list, &index)) {
        MESH_TEST_FAIL_IF(index != seen, "iteration should hand back consecutive indices");
        seen++;
    }
    MESH_TEST_FAIL_IF(seen != 3U, "a short list should yield every item");

    /* A window smaller than the list yields only what is on screen, starting where it should. */
    list = mesh_ui_list_begin(20U, 19U, 5U);
    MESH_TEST_FAIL_IF(!mesh_ui_list_is_cursor(&list, 19U), "the cursor should be the last item");
    seen = 0U;
    uint32_t first = 0U;
    while (mesh_ui_list_next(&list, &index)) {
        if (seen == 0U) {
            first = index;
        }
        seen++;
    }
    MESH_TEST_FAIL_IF(seen != 5U, "only the visible rows should be yielded");
    MESH_TEST_FAIL_IF(first != 15U, "iteration should start at the top of the window");

    /* An empty list is not a special case at the call site: it simply yields nothing. */
    list = mesh_ui_list_begin(0U, 0U, 10U);
    MESH_TEST_FAIL_IF(mesh_ui_list_next(&list, &index), "an empty list should yield nothing");
    MESH_TEST_FAIL_IF(mesh_ui_list_is_cursor(&list, 0U), "an empty list has no cursor row");
    record_success(test_name);
}
