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
