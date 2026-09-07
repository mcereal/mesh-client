/*
 * The cell-measured line builder and the scroll window.
 *
 * Everything here counts in cells (mesh_ui_text_cells) and cuts on cell boundaries
 * (mesh_ui_text_cell_truncate / mesh_ui_text_cell_offset). There is deliberately no entry
 * point that takes or returns a byte count: the whole reason this file exists is that byte
 * arithmetic in layout code is always wrong the moment a name holds an emoji.
 */

#include "mesh/ui/layout.h"

#include "mesh/ui/emoji.h"

#include <stdio.h>
#include <string.h>

void mesh_ui_line_reset(struct mesh_ui_line *line) {
    line->text[0] = '\0';
    line->len = 0U;
}

/* Bytes still free for content, terminator excluded. */
static size_t mesh_ui_line_room(const struct mesh_ui_line *line) {
    return line->len < sizeof line->text ? sizeof line->text - line->len - 1U : 0U;
}

/*
 * Re-terminate after an append that may have been cut short.
 *
 * snprintf() truncates on a byte, which can land in the middle of a UTF-8 sequence, and the
 * cell walker does not clean that up for us - an incomplete sequence decodes as one invalid
 * cell per stray byte, so it would draw as a row of replacement boxes and would leave
 * malformed UTF-8 on the paths that also log or serialise the line. Drop the partial tail
 * instead, so every line in hand is valid UTF-8 by construction.
 */
static void mesh_ui_line_settle(struct mesh_ui_line *line) {
    line->len = strlen(line->text);

    /* At most three continuation bytes (10xxxxxx) can trail a lead byte. */
    size_t back = 0U;
    while (back < 3U && line->len > back &&
           ((uint8_t)line->text[line->len - 1U - back] & 0xC0U) == 0x80U) {
        back += 1U;
    }
    if (line->len <= back) {
        return;
    }

    const uint8_t lead = (uint8_t)line->text[line->len - 1U - back];
    size_t need;
    if ((lead & 0x80U) == 0x00U) {
        need = 1U;
    } else if ((lead & 0xE0U) == 0xC0U) {
        need = 2U;
    } else if ((lead & 0xF0U) == 0xE0U) {
        need = 3U;
    } else if ((lead & 0xF8U) == 0xF0U) {
        need = 4U;
    } else {
        return; /* a stray continuation byte or an invalid lead: not our truncation to undo */
    }
    if (need > back + 1U) {
        line->len -= back + 1U;
        line->text[line->len] = '\0';
    }
}

void mesh_ui_line_vprintf(struct mesh_ui_line *line, const char *fmt, va_list args) {
    const size_t room = mesh_ui_line_room(line);
    if (room == 0U) {
        return;
    }
    (void)vsnprintf(line->text + line->len, room + 1U, fmt, args);
    mesh_ui_line_settle(line);
}

void mesh_ui_line_printf(struct mesh_ui_line *line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mesh_ui_line_vprintf(line, fmt, args);
    va_end(args);
}

/* The i18n counterpart. mesh_str_vformat() is where the non-literal format is answered for;
   see include/mesh/i18n/strings.h. */
void mesh_ui_line_str(struct mesh_ui_line *line, enum mesh_str_id id, ...) {
    const size_t room = mesh_ui_line_room(line);
    if (room == 0U) {
        return;
    }
    va_list args;
    va_start(args, id);
    (void)mesh_str_vformat(line->text + line->len, room + 1U, id, args);
    va_end(args);
    mesh_ui_line_settle(line);
}

void mesh_ui_line_pad_to(struct mesh_ui_line *line, size_t cols) {
    size_t width = mesh_ui_text_cells(line->text);
    while (width < cols && mesh_ui_line_room(line) > 0U) {
        line->text[line->len] = ' ';
        line->len += 1U;
        line->text[line->len] = '\0';
        width += 1U;
    }
}

void mesh_ui_line_column(struct mesh_ui_line *line, const char *text, size_t cols) {
    const size_t start = mesh_ui_text_cells(line->text);
    mesh_ui_line_printf(line, "%s", text);
    mesh_ui_line_fit(line, start + cols);
    mesh_ui_line_pad_to(line, start + cols);
}

void mesh_ui_line_fit(struct mesh_ui_line *line, size_t cols) {
    mesh_ui_text_cell_truncate(line->text, cols);
    line->len = strlen(line->text);
}

void mesh_ui_line_right(struct mesh_ui_line *line, size_t cols, const char *right) {
    const size_t right_cols = mesh_ui_text_cells(right);
    if (right_cols == 0U) {
        mesh_ui_line_fit(line, cols);
        return;
    }
    /* Leave a column of gap between the two halves; below that there is nothing sensible to
       give the left side, so it keeps a readable minimum and the right half is what clips. */
    const size_t left_cols = cols > right_cols + 1U ? cols - right_cols - 1U : 8U;
    mesh_ui_line_fit(line, left_cols);
    mesh_ui_line_pad_to(line,
                        cols > right_cols ? cols - right_cols : mesh_ui_line_width(line) + 1U);
    mesh_ui_line_printf(line, "%s", right);
    mesh_ui_line_fit(line, cols);
}

size_t mesh_ui_line_width(const struct mesh_ui_line *line) {
    return mesh_ui_text_cells(line->text);
}

const char *mesh_ui_line_text(const struct mesh_ui_line *line) { return line->text; }

uint32_t mesh_ui_list_first_visible(uint32_t cursor, uint32_t count, uint32_t visible) {
    if (visible == 0U || count <= visible) {
        return 0U;
    }
    if (cursor + 1U > visible) {
        uint32_t first = cursor + 1U - visible;
        if (first + visible > count) {
            first = count - visible;
        }
        return first;
    }
    return 0U;
}

struct mesh_ui_list mesh_ui_list_begin(uint32_t count, uint32_t cursor, uint32_t visible) {
    struct mesh_ui_list list;
    memset(&list, 0, sizeof list);
    list.count = count;
    list.visible = visible;
    if (count == 0U) {
        return list;
    }
    list.cursor = cursor < count ? cursor : count - 1U;
    list.first = mesh_ui_list_first_visible(list.cursor, count, visible);
    list.next = list.first;
    return list;
}

bool mesh_ui_list_next(struct mesh_ui_list *list, uint32_t *index) {
    if (list->next >= list->count || list->next >= list->first + list->visible) {
        return false;
    }
    *index = list->next;
    list->next += 1U;
    return true;
}

bool mesh_ui_list_is_cursor(const struct mesh_ui_list *list, uint32_t index) {
    return list->count > 0U && index == list->cursor;
}

/* ---- word wrapping -------------------------------------------------------------------------- */

void mesh_ui_wrap_begin(struct mesh_ui_wrap *wrap, const char *text, size_t cols) {
    wrap->rest = text != NULL ? text : "";
    wrap->cols = cols > 0U ? cols : 1U;
    wrap->line[0] = '\0';
}

bool mesh_ui_wrap_next(struct mesh_ui_wrap *wrap) {
    const char *rest = wrap->rest;
    /* A run of spaces never opens a line: breaking after one would otherwise indent the
       continuation by however many the sender typed. */
    while (*rest == ' ') {
        ++rest;
    }
    if (*rest == '\0') {
        wrap->rest = rest;
        wrap->line[0] = '\0';
        return false;
    }

    /* Walk cells - not bytes - until the window is full, remembering the last place a word
       boundary would let us break. A space byte can never appear inside a multi-byte sequence,
       so testing the lead byte for ' ' is safe. */
    size_t taken = 0U;
    size_t cells = 0U;
    size_t last_space = 0U; /* bytes up to and including that space; 0 means none */
    bool hard = false;
    while (cells < wrap->cols) {
        const char lead = rest[taken];
        if (lead == '\0') {
            break;
        }
        if (lead == '\n') {
            hard = true;
            break;
        }
        const struct mesh_ui_text_cell cell = mesh_ui_text_cell_next(&rest[taken]);
        if (cell.bytes == 0U || taken + cell.bytes >= sizeof wrap->line) {
            break;
        }
        taken += cell.bytes;
        cells += 1U;
        if (lead == ' ') {
            last_space = taken;
        }
    }

    size_t cut = taken;
    if (!hard && rest[cut] != '\0' && rest[cut] != ' ' && last_space > 0U) {
        cut = last_space; /* the window ended mid-word, so give the whole word to the next line */
    }
    if (cut == 0U && !hard) {
        /* One cell spelled with more bytes than a whole line holds. Nothing sensible draws, but
           the walk still has to move or the caller loops forever. */
        const struct mesh_ui_text_cell cell = mesh_ui_text_cell_next(rest);
        wrap->line[0] = '\0';
        wrap->rest = rest + (cell.bytes > 0U ? cell.bytes : 1U);
        return true;
    }

    size_t len = cut;
    while (len > 0U && rest[len - 1U] == ' ') {
        len -= 1U;
    }
    memcpy(wrap->line, rest, len);
    wrap->line[len] = '\0';
    wrap->rest = rest + cut + (hard ? 1U : 0U); /* the newline itself is consumed, not drawn */
    return true;
}

uint32_t mesh_ui_wrap_lines(const char *text, size_t cols) {
    struct mesh_ui_wrap wrap;
    mesh_ui_wrap_begin(&wrap, text, cols);
    uint32_t lines = 0U;
    while (mesh_ui_wrap_next(&wrap)) {
        lines += 1U;
    }
    return lines;
}

size_t mesh_ui_wrap_widest(const char *text, size_t cols) {
    struct mesh_ui_wrap wrap;
    mesh_ui_wrap_begin(&wrap, text, cols);
    size_t widest = 0U;
    while (mesh_ui_wrap_next(&wrap)) {
        const size_t width = mesh_ui_text_cells(wrap.line);
        if (width > widest) {
            widest = width;
        }
    }
    return widest;
}

/* ---- the transcript window ------------------------------------------------------------------ */

struct mesh_ui_transcript mesh_ui_transcript_window(const uint8_t *heights, uint32_t count,
                                                    uint32_t cursor, uint32_t rows) {
    struct mesh_ui_transcript window;
    memset(&window, 0, sizeof window);
    if (heights == NULL || count == 0U || rows == 0U) {
        return window;
    }
    if (cursor >= count) {
        cursor = count - 1U;
    }

    uint32_t total = 0U;
    for (uint32_t i = 0; i < count; ++i) {
        total += heights[i];
    }
    if (total <= rows) {
        /* Everything fits, so the slack goes above it: a two-message thread opens with those two
           messages on the bottom rows, where a forty-message one leaves them. */
        window.first = 0U;
        window.count = count;
        window.pad = rows - total;
        return window;
    }

    /* Pinned to the newest: walk back from the end while the items still fit. */
    uint32_t first = count;
    uint32_t used = 0U;
    while (first > 0U && used + heights[first - 1U] <= rows) {
        first -= 1U;
        used += heights[first];
    }
    if (first == count) {
        /* The newest item alone is taller than the body. Draw it anyway and let it clip at the
           bottom, which is the one place the whole UI clips. */
        first = count - 1U;
        used = rows;
    }

    if (cursor >= first) {
        window.first = first;
        window.count = count - first;
        window.pad = rows - used;
        return window;
    }

    /* Scrolled up past the pinned window: the cursor tops it and the rest fills downward, so the
       item being read is whole rather than the one hanging off the top edge. */
    uint32_t last = cursor;
    used = 0U;
    while (last < count && used + heights[last] <= rows) {
        used += heights[last];
        last += 1U;
    }
    window.first = cursor;
    window.count = last > cursor ? last - cursor : 1U;
    return window;
}

struct mesh_ui_scroll mesh_ui_list_scroll(const struct mesh_ui_list *list, int track, int minimum) {
    struct mesh_ui_scroll scroll = {0, 0};
    if (list == NULL || track <= 0) {
        return scroll;
    }

    const uint32_t count = list->count;
    const uint32_t visible = list->visible;
    /* Nothing off screen is nothing to report. */
    if (count == 0U || visible == 0U || count <= visible) {
        return scroll;
    }

    /* The fraction on screen, floored at something findable: on a list of two hundred a true
       proportion is a pixel or two, which is an indicator reporting a position nobody can see. */
    int64_t length = ((int64_t)visible * (int64_t)track) / (int64_t)count;
    if (minimum > 0 && length < (int64_t)minimum) {
        length = minimum;
    }
    if (length > (int64_t)track) {
        length = track;
    }

    const uint32_t last_first = count - visible;
    const int64_t travel = (int64_t)track - length;
    int64_t offset = 0;
    if (last_first > 0U && travel > 0) {
        uint32_t first = list->first;
        if (first > last_first) {
            first = last_first;
        }
        offset = ((int64_t)first * travel) / (int64_t)last_first;
    }

    scroll.offset = (int)offset;
    scroll.length = (int)length;
    return scroll;
}
