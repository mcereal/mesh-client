#ifndef MESH_UI_LAYOUT_H
#define MESH_UI_LAYOUT_H

/*
 * The layout primitives every list-and-rows UI needs, with no backend in them.
 *
 * They exist because the same mistakes kept being made by hand in the screen renderers:
 *
 *   - Laying a line out in *bytes*. A node named with one emoji is four bytes and one column,
 *     so "%-4s" pads it to nothing and a right-aligned figure computed from strlen() walks off
 *     the edge of the panel. `struct mesh_ui_line` only ever measures in drawn cells, so the
 *     mistake is no longer expressible: there is no byte-counting entry point.
 *   - Re-deriving the scroll window. Every screen wrote the same clamp-the-cursor,
 *     find-the-first-visible-row, loop-while-it-fits three-liner. `struct mesh_ui_list` is
 *     that arithmetic once.
 *   - Wrapping text twice. A chat bubble measures itself and then draws itself, and the two
 *     walks have to agree to the row or bubbles overlap. `struct mesh_ui_wrap` is the one walk
 *     both passes make, and `mesh_ui_transcript_window` is the bottom-anchored scroll window
 *     that variable-height items need.
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
 * Word wrapping, measured in cells and cut on cell boundaries.
 *
 * The point of the iterator shape is that measuring and drawing are the *same walk*: a chat
 * bubble has to know how many rows it will take before it is placed, and a bubble that
 * measured five rows and drew six would overwrite the message under it. Both passes call
 * mesh_ui_wrap_next(), so they cannot disagree.
 *
 * Breaks at the last space inside the window when there is one, at a hard newline always, and
 * mid-cell never. Leading spaces never open a line and trailing spaces never close one.
 */
struct mesh_ui_wrap {
    const char *rest;            /* what has not been emitted yet */
    size_t cols;                 /* window width in cells; 0 is read as 1 */
    char line[MESH_UI_LINE_MAX]; /* the line the last _next() produced */
};

void mesh_ui_wrap_begin(struct mesh_ui_wrap *wrap, const char *text, size_t cols);

/* Fills `wrap->line` with the next line; false once the text is spent. */
bool mesh_ui_wrap_next(struct mesh_ui_wrap *wrap);

/* Rows `text` needs at this width. Empty text needs none. */
uint32_t mesh_ui_wrap_lines(const char *text, size_t cols);

/* Cells the widest of those rows occupies - what a bubble sizes itself to, so a two-word
   message does not draw a full-width box. */
size_t mesh_ui_wrap_widest(const char *text, size_t cols);

/*
 * A bottom-anchored window onto items of differing heights: a transcript.
 *
 * struct mesh_ui_list cannot do this - it assumes every item is one row (or a fixed number of
 * them), and a wrapped message is however many rows its text needs. The two rules that make it
 * read like a messenger rather than like a list are here rather than in a backend:
 *
 *   - the newest item sits on the *bottom* row, with the slack above it, so a thread with two
 *     messages in it opens where a thread with forty does;
 *   - scrolling up puts the cursor at the top of the window and fills downward, so the item
 *     being read is never the one half off the edge.
 *
 * `heights` is one row count per item, oldest first. Heights of 0 are legal (nothing draws).
 */
struct mesh_ui_transcript {
    uint32_t first; /* first item drawn */
    uint32_t count; /* items drawn, starting at `first` */
    uint32_t pad;   /* blank rows above `first`, so the newest lands on the last row */
};

struct mesh_ui_transcript mesh_ui_transcript_window(const uint8_t *heights, uint32_t count,
                                                    uint32_t cursor, uint32_t rows);

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
