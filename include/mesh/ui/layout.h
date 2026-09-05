#ifndef MESH_UI_LAYOUT_H
#define MESH_UI_LAYOUT_H

/*
 * Two primitives every list-and-rows UI needs, with no backend in them.
 *
 * They exist because the same two mistakes kept being made by hand in the screen renderers:
 *
 *   - Laying a line out in *bytes*. A node named with one emoji is four bytes and one column,
 *     so "%-4s" pads it to nothing and a right-aligned figure computed from strlen() walks off
 *     the edge of the panel. `struct mesh_ui_line` only ever measures in drawn cells, so the
 *     mistake is no longer expressible: there is no byte-counting entry point.
 *   - Re-deriving the scroll window. Every screen wrote the same clamp-the-cursor,
 *     find-the-first-visible-row, loop-while-it-fits three-liner. `struct mesh_ui_list` is
 *     that arithmetic once.
 *
 * Neither touches a framebuffer, a snapshot or a font, so both are unit tested directly
 * (tests/suites/ui_layout.c) and both are as useful to the CLI backend as to the fb one.
 *
 * A cell is what `mesh_ui_text_cell_next` says it is - one character, or one emoji however
 * many codepoints it is spelled with. See include/mesh/ui/emoji.h.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Long enough for the widest thing drawn: a 233-byte message plus a peer name and a tag. */
#define MESH_UI_LINE_MAX 400U

/*
 * A line under construction.
 *
 * Every call appends, and every call is safe against a full buffer - it truncates on a cell
 * boundary rather than splitting a UTF-8 sequence, so a line that overflows still draws (and
 * still logs, and still serialises) as valid text. Build with mesh_ui_line_reset(), read with
 * mesh_ui_line_text().
 */
struct mesh_ui_line {
    char text[MESH_UI_LINE_MAX];
    size_t len; /* bytes used, excluding the terminator */
};

void mesh_ui_line_reset(struct mesh_ui_line *line);

/* Append formatted text. */
void mesh_ui_line_printf(struct mesh_ui_line *line, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void mesh_ui_line_vprintf(struct mesh_ui_line *line, const char *fmt, va_list args);

/*
 * Append `text` occupying exactly `cols` cells: clipped if it is wider, space-padded if it is
 * narrower. This is "%-*.*s" done in cells, and it is what makes a label column line up when
 * the labels are not all ASCII.
 */
void mesh_ui_line_column(struct mesh_ui_line *line, const char *text, size_t cols);

/* Pad with spaces until the line occupies `cols` cells. A no-op once it is that wide. */
void mesh_ui_line_pad_to(struct mesh_ui_line *line, size_t cols);

/*
 * Right-align `right` at column `cols`, clipping what is already in the line to make room.
 *
 * This is the shape a list row with a metric on the end has - "Andy      -7.5dB 3m" - and
 * doing it by hand is where the byte/cell confusion did the most damage. `right` may be empty,
 * in which case the line is simply clipped to `cols`.
 */
void mesh_ui_line_right(struct mesh_ui_line *line, size_t cols, const char *right);

/* Clip to `cols` cells. */
void mesh_ui_line_fit(struct mesh_ui_line *line, size_t cols);

/* Cells the line occupies once drawn. */
size_t mesh_ui_line_width(const struct mesh_ui_line *line);

const char *mesh_ui_line_text(const struct mesh_ui_line *line);

/*
 * A window onto `count` items with the cursor kept on screen.
 *
 * Stateless between frames on purpose: the window is derived from the cursor every time rather
 * than remembered, so there is no scroll position to get out of step with a list that changed
 * underneath it. Iterate with mesh_ui_list_next().
 */
struct mesh_ui_list {
    uint32_t count;   /* items in the list */
    uint32_t cursor;  /* clamped into [0, count) - count == 0 leaves it 0 */
    uint32_t first;   /* index of the first item on screen */
    uint32_t visible; /* items that fit */
    uint32_t next;    /* iterator position */
};

/*
 * `cursor` is taken raw from the nav state and clamped here, which is the check every screen
 * used to write out. `visible` of 0 yields a list that draws nothing rather than one that
 * divides by zero.
 */
struct mesh_ui_list mesh_ui_list_begin(uint32_t count, uint32_t cursor, uint32_t visible);

/* Hands back each visible index in turn, false when the window is exhausted. */
bool mesh_ui_list_next(struct mesh_ui_list *list, uint32_t *index);

bool mesh_ui_list_is_cursor(const struct mesh_ui_list *list, uint32_t index);

/* Where the window starts so that `cursor` is inside it. */
uint32_t mesh_ui_list_first_visible(uint32_t cursor, uint32_t count, uint32_t visible);

#endif /* MESH_UI_LAYOUT_H */
