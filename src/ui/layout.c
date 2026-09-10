/*
 * The cell-measured line builder and the scroll window.
 *
 * Everything here counts in cells (mesh_ui_text_cells) and cuts on cell boundaries
 * (mesh_ui_text_cell_truncate / mesh_ui_text_cell_offset). There is deliberately no entry
 * point that takes or returns a byte count: the whole reason this file exists is that byte
 * arithmetic in layout code is always wrong the moment a name holds an emoji.
 */

#include "mesh/ui/layout.h"

#include "mesh/ui/anim.h"
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

/* Steps item `index` occupies, read off whichever of the two shapes the list was opened with.
   A height of 0 is read as 1: an item occupying nothing could never be scrolled onto, so the
   cursor would walk into a row that is not on screen and the window would never move. */
static uint32_t list_height(const struct mesh_ui_list *list, uint32_t index) {
    if (index >= list->count) {
        return 0U;
    }
    const uint8_t h = list->heights != NULL ? list->heights[index] : list->step;
    return h > 0U ? (uint32_t)h : 1U;
}

/* Steps items [from, to) occupy. */
static uint32_t list_sum(const struct mesh_ui_list *list, uint32_t from, uint32_t to) {
    if (to > list->count) {
        to = list->count;
    }
    if (from >= to) {
        return 0U;
    }
    if (list->heights == NULL) {
        return (to - from) * (list->step > 0U ? (uint32_t)list->step : 1U);
    }
    uint32_t steps = 0U;
    for (uint32_t i = from; i < to; ++i) {
        steps += list_height(list, i);
    }
    return steps;
}

/*
 * How many items starting at `from` fit in `capacity` steps.
 *
 * The one item that is taller than the whole window is taken anyway. It draws clipped, which
 * is wrong-looking; refusing it draws nothing at all and leaves the cursor sitting on a row the
 * screen does not contain, which is wrong and also invisible.
 */
static uint32_t list_fits_forward(const struct mesh_ui_list *list, uint32_t from, uint32_t capacity,
                                  uint32_t *used) {
    uint32_t taken = 0U;
    uint32_t steps = 0U;
    for (uint32_t i = from; i < list->count; ++i) {
        const uint32_t h = list_height(list, i);
        if (steps + h > capacity && taken > 0U) {
            break;
        }
        steps += h;
        taken += 1U;
        if (steps >= capacity) {
            break;
        }
    }
    if (used != NULL) {
        *used = steps;
    }
    return taken;
}

/* The lowest `first` that still keeps `last` on screen: the window filled upward from it. The
   counterpart of list_fits_forward(), and what puts the cursor on the last line that fits. */
static uint32_t list_fits_backward(const struct mesh_ui_list *list, uint32_t last,
                                   uint32_t capacity) {
    uint32_t first = last;
    uint32_t steps = list_height(list, last);
    while (first > 0U) {
        const uint32_t h = list_height(list, first - 1U);
        if (steps + h > capacity) {
            break;
        }
        steps += h;
        first -= 1U;
    }
    return first;
}

/*
 * The common core of the three entry points.
 *
 * `heights` and `step` are already on `list`; everything else is derived here, in the order the
 * window is actually decided: where it starts, then what fits from there. Deriving `visible` by
 * walking forward from `first` rather than by reusing the backward walk's count is deliberate -
 * the two agree in every case but the clipped one, and the forward walk is the one the iterator
 * and the draw loop both follow.
 */
static void list_settle(struct mesh_ui_list *list, uint32_t cursor, uint32_t capacity) {
    list->capacity = capacity;
    if (list->count == 0U) {
        return;
    }
    list->cursor = cursor < list->count ? cursor : list->count - 1U;
    /* A uniform list is multiplication rather than a walk. It matters: the Nodes tab holds every
       node the radio has ever mentioned, and the two totals below are the only places here that
       would otherwise be linear in the whole list rather than in the window. */
    list->total = list_sum(list, 0U, list->count);
    if (capacity == 0U) {
        return;
    }
    /* Everything fits, so nothing scrolls - the check `count <= visible` used to make, said in
       the unit the window is measured in. */
    list->first = list->total <= capacity ? 0U : list_fits_backward(list, list->cursor, capacity);
    list->first_step = list_sum(list, 0U, list->first);
    list->visible = list_fits_forward(list, list->first, capacity, &list->used);
    list->next = list->first;
    /* Where the scroll rail's thumb runs out of travel: the steps above the window that ends on
       the last item. It is derived here, with the rest of the window, rather than inside
       mesh_ui_list_scroll() - a rail asks for it once per list and this is the walk that is
       already bounded by the window rather than by the list. */
    if (list->total > capacity) {
        list->last_first_step =
            list_sum(list, 0U, list_fits_backward(list, list->count - 1U, capacity));
    }
}

struct mesh_ui_list mesh_ui_list_begin(uint32_t count, uint32_t cursor, uint32_t visible) {
    return mesh_ui_list_begin_step(count, cursor, visible, 1U);
}

struct mesh_ui_list mesh_ui_list_begin_step(uint32_t count, uint32_t cursor, uint32_t capacity,
                                            uint8_t step) {
    struct mesh_ui_list list;
    memset(&list, 0, sizeof list);
    list.count = count;
    list.step = step > 0U ? step : 1U;
    list_settle(&list, cursor, capacity);
    return list;
}

struct mesh_ui_list mesh_ui_list_begin_heights(uint32_t count, uint32_t cursor, uint32_t capacity,
                                               const uint8_t *heights) {
    struct mesh_ui_list list;
    memset(&list, 0, sizeof list);
    list.count = count;
    list.step = 1U;
    list.heights = heights;
    list_settle(&list, cursor, capacity);
    return list;
}

/*
 * Asked of the model rather than derived a second time.
 *
 * It used to be the closed form for a list of one-row items, and mesh_ui_list_begin() called
 * it. Once a window can be a sum of heights rather than a multiplication, a closed form beside
 * it is a second opinion about where a list starts - and the two would agree until the day one
 * of them was taught about a taller row, which is the day nobody would look here.
 */
uint32_t mesh_ui_list_first_visible(uint32_t cursor, uint32_t count, uint32_t visible) {
    return mesh_ui_list_begin(count, cursor, visible).first;
}

uint8_t mesh_ui_list_item_height(const struct mesh_ui_list *list, uint32_t index) {
    if (list == NULL) {
        return 0U;
    }
    const uint32_t h = list_height(list, index);
    return h > 0xFFU ? 0xFFU : (uint8_t)h;
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

    /*
     * Measured in steps rather than in items, which is the same arithmetic while every item is
     * one step and the only honest one once they are not: a list of forty rows where four of
     * them are twice as tall has a thumb that is shorter than four-fortieths, and an item count
     * cannot say so.
     */
    const uint32_t total = list->total;
    /* Clamped, for the one item taller than the whole window: it is drawn clipped, so the steps
       it occupies on screen are the window and not its own height. Unclamped, a list holding
       one such item reported most of itself on screen. */
    const uint32_t used = list->used < list->capacity ? list->used : list->capacity;
    /* Nothing off screen is nothing to report. */
    if (total == 0U || used == 0U || total <= used) {
        return scroll;
    }

    /* The fraction on screen, floored at something findable: on a list of two hundred a true
       proportion is a pixel or two, which is an indicator reporting a position nobody can see. */
    int64_t length = ((int64_t)used * (int64_t)track) / (int64_t)total;
    if (minimum > 0 && length < (int64_t)minimum) {
        length = minimum;
    }
    if (length > (int64_t)track) {
        length = track;
    }

    /*
     * Where the thumb runs out of track: the steps above the *last* window rather than
     * `total - used`, because on a list of mixed heights the window at the bottom need not be
     * the same number of steps as the one being drawn. Measuring it against the window we are
     * in makes the thumb overshoot the end of the rail by the difference, which reads as a
     * scroll indicator that is a few pixels wrong exactly where the eye checks it.
     */
    const uint32_t last_first = list->last_first_step;
    const int64_t travel = (int64_t)track - length;
    int64_t offset = 0;
    if (last_first > 0U && travel > 0) {
        uint32_t first = list->first_step;
        if (first > last_first) {
            first = last_first;
        }
        offset = ((int64_t)first * travel) / (int64_t)last_first;
    }

    scroll.offset = (int)offset;
    scroll.length = (int)length;
    return scroll;
}

int32_t mesh_ui_scale_permille(struct mesh_ui_scale scale, int32_t value) {
    int64_t permille;
    if (scale.min == scale.max) {
        /* The identity domain: the caller already holds a fraction. */
        permille = value;
    } else {
        /*
         * Both terms carry the scale's direction, so a descending domain divides two negatives
         * and comes out reading backwards - which is the whole of what "descending" has to
         * mean. 64-bit because the ends are a caller's and nothing stops a domain being wide.
         */
        const int64_t span = (int64_t)scale.max - (int64_t)scale.min;
        const int64_t offset = (int64_t)value - (int64_t)scale.min;
        permille = (offset * MESH_UI_ANIM_ONE) / span;
    }
    if (permille < 0) {
        return 0;
    }
    if (permille > MESH_UI_ANIM_ONE) {
        return MESH_UI_ANIM_ONE;
    }
    return (int32_t)permille;
}

int32_t mesh_ui_percent_permille(float percent) {
    if (!(percent > 0.0f)) { /* also catches NaN, which no comparison the other way round does */
        return 0;
    }
    if (percent >= 100.0f) {
        return MESH_UI_ANIM_ONE;
    }
    /* Rounded rather than truncated: a reading of 3.99% that drew as 3.9 would be a bar that is
       consistently short of the figure printed beside it. */
    return (int32_t)(percent * 10.0f + 0.5f);
}

uint8_t mesh_ui_signal_level(float snr) {
    /*
     * A ladder of `>=` walked from the top, so a NaN - which compares false against everything
     * - falls all the way through to the bottom rung rather than matching a middle one. A radio
     * that has reported nothing usable should read as a bad link, not as a middling one.
     */
    if (snr >= (float)MESH_UI_SNR_EXCELLENT) {
        return 4U;
    }
    if (snr >= (float)MESH_UI_SNR_GOOD) {
        return 3U;
    }
    if (snr >= (float)MESH_UI_SNR_FAIR) {
        return 2U;
    }
    if (snr >= (float)MESH_UI_SNR_POOR) {
        return 1U;
    }
    return 0U;
}

/* ---- a composition -------------------------------------------------------------------------- */

uint32_t mesh_ui_proportion_split(const uint32_t *values, uint32_t count, int32_t extent,
                                  int32_t *out) {
    if (values == NULL || out == NULL || count == 0U || count > MESH_UI_PROPORTION_PARTS ||
        extent <= 0) {
        return 0U;
    }

    uint64_t total = 0U;
    uint32_t present = 0U; /* parts that are actually there, and so owe a unit each */
    for (uint32_t i = 0; i < count; ++i) {
        total += (uint64_t)values[i];
        if (values[i] > 0U) {
            present++;
        }
    }
    if (total == 0U) {
        return 0U;
    }

    /*
     * The floor of each part's share, and what it lost to that floor.
     *
     * In 64 bits because the numerator is a packet counter times a pixel width, and a radio
     * that has been up for a month reports counters in the millions - the multiply is where a
     * 32-bit version would wrap, silently, into a bar drawn from a negative share.
     */
    uint64_t remainder[MESH_UI_PROPORTION_PARTS];
    int64_t used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t numerator = (uint64_t)values[i] * (uint64_t)extent;
        out[i] = (int32_t)(numerator / total);
        remainder[i] = numerator % total;
        used += out[i];
    }

    /* Largest remainder. The shortfall is strictly less than `count`, so no part is ever handed
       a second unit and one pass over the list per unit is the whole of it. */
    for (int64_t left = (int64_t)extent - used; left > 0; --left) {
        uint32_t best = count;
        for (uint32_t i = 0; i < count; ++i) {
            if (remainder[i] > 0U && (best == count || remainder[i] > remainder[best])) {
                best = i;
            }
        }
        if (best == count) {
            break; /* nothing lost anything: the split was exact and `left` is already 0 */
        }
        out[best]++;
        remainder[best] = 0U;
    }

    /*
     * And the part that is there but too small to see.
     *
     * Only when the bar is long enough to give every part a unit - below that there is no
     * honest picture to draw and the proportions are left alone. Above it there is always a
     * donor: if one part came out at nothing, the rest are sharing at least as many units as
     * there are of them, so one of them has two.
     */
    if ((uint64_t)extent >= (uint64_t)present) {
        for (uint32_t i = 0; i < count; ++i) {
            if (values[i] == 0U || out[i] > 0) {
                continue;
            }
            uint32_t donor = count;
            for (uint32_t j = 0; j < count; ++j) {
                if (out[j] > 1 && (donor == count || out[j] > out[donor])) {
                    donor = j;
                }
            }
            if (donor == count) {
                break;
            }
            out[donor]--;
            out[i]++;
        }
    }

    return count;
}

/* ---- a series ------------------------------------------------------------------------------ */

void mesh_ui_series_reset(struct mesh_ui_series *series, uint32_t gap_ms) {
    if (series == NULL) {
        return;
    }
    memset(series, 0, sizeof *series);
    series->gap_ms = gap_ms;
}

void mesh_ui_series_push(struct mesh_ui_series *series, uint32_t time, int32_t value) {
    if (series == NULL) {
        return;
    }
    const struct mesh_ui_sample *newest = mesh_ui_series_newest(series);
    if (newest != NULL && time < newest->time) {
        /* The clock went backwards, so nothing here knows when any of it happened any more. The
           readings are still true and a trend over them is not, which is the difference between
           dropping the series and keeping it. See the header. */
        const uint32_t gap_ms = series->gap_ms;
        memset(series, 0, sizeof *series);
        series->gap_ms = gap_ms;
    }
    uint32_t slot;
    if (series->count < MESH_UI_SERIES_MAX) {
        slot = (series->first + series->count) % MESH_UI_SERIES_MAX;
        ++series->count;
    } else {
        /* Full: the oldest is what the newest costs. */
        slot = series->first;
        series->first = (series->first + 1U) % MESH_UI_SERIES_MAX;
    }
    series->items[slot].time = time;
    series->items[slot].value = value;
    /* A break belongs to the sample that *starts* the new segment, so it is spent here rather
       than remembered against the series - a second push must not inherit it. */
    series->items[slot].gap = series->pending_break;
    series->pending_break = false;
}

void mesh_ui_series_break(struct mesh_ui_series *series) {
    if (series == NULL) {
        return;
    }
    series->pending_break = true;
}

const struct mesh_ui_sample *mesh_ui_series_at(const struct mesh_ui_series *series,
                                               uint32_t index) {
    if (series == NULL || index >= series->count) {
        return NULL;
    }
    return &series->items[(series->first + index) % MESH_UI_SERIES_MAX];
}

const struct mesh_ui_sample *mesh_ui_series_newest(const struct mesh_ui_series *series) {
    if (series == NULL || series->count == 0U) {
        return NULL;
    }
    return mesh_ui_series_at(series, series->count - 1U);
}

/*
 * Whether `sample` starts a segment rather than continuing the one before it.
 *
 * The first sample continues nothing; neither does one that arrived after a silence the series
 * calls a break, nor one the source itself broke before - a discontinuity the clock cannot see,
 * which is what mesh_ui_series_break() exists for.
 *
 * One function because two callers ask it: the projection, which lifts the pen, and
 * mesh_ui_series_has_segment(), which decides whether there is a line to offer at all. Written
 * twice they can disagree, and the way they disagree is a screen that offers a picture the
 * renderer then declines to draw.
 */
static bool series_breaks_at(const struct mesh_ui_series *series,
                             const struct mesh_ui_sample *sample, uint32_t index,
                             uint32_t previous) {
    return index == 0U || sample->gap ||
           (series->gap_ms > 0U && (sample->time - previous) > series->gap_ms);
}

bool mesh_ui_series_has_segment(const struct mesh_ui_series *series) {
    if (series == NULL || series->count < 2U) {
        return false; /* one reading is a level, and there is a component for that */
    }
    uint32_t previous = 0U;
    for (uint32_t i = 0U; i < series->count; ++i) {
        const struct mesh_ui_sample *sample = mesh_ui_series_at(series, i);
        if (!series_breaks_at(series, sample, i, previous)) {
            return true; /* one unbroken pair is a line, wherever in the ring it sits */
        }
        previous = sample->time;
    }
    return false;
}

void mesh_ui_series_project(const struct mesh_ui_series *series, struct mesh_ui_scale scale,
                            struct mesh_ui_polyline *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (series == NULL || series->count == 0U) {
        return;
    }

    /* Its own two ends, which is the right window for a line drawn by itself: one series is
       measured against nothing else, so the shape is all of the reading and the box is all of
       the room. A picture with a second line on it wants mesh_ui_series_window() instead. */
    const struct mesh_ui_sample *oldest = mesh_ui_series_at(series, 0U);
    const struct mesh_ui_sample *newest = mesh_ui_series_newest(series);
    mesh_ui_series_project_over(series, scale, oldest->time, newest->time, out);
}

bool mesh_ui_series_window(const struct mesh_ui_series *const *series, uint32_t count,
                           uint32_t *out_from, uint32_t *out_to) {
    if (series == NULL || count == 0U) {
        return false;
    }
    uint32_t from = 0U;
    uint32_t to = 0U;
    bool any = false;
    for (uint32_t i = 0U; i < count; ++i) {
        const struct mesh_ui_series *one = series[i];
        if (one == NULL || one->count == 0U) {
            continue; /* a series with nothing in it frames nothing, and is not an error */
        }
        const uint32_t first = mesh_ui_series_at(one, 0U)->time;
        const uint32_t last = mesh_ui_series_newest(one)->time;
        if (!any || first < from) {
            from = first;
        }
        if (!any || last > to) {
            to = last;
        }
        any = true;
    }
    /* Both ends, and two of them: everything the client keeps is stamped by one monotonic clock,
       so a window of zero width means every reading landed inside one tick rather than that the
       series disagree. There is nothing to lay an axis along either way. */
    if (!any || to <= from) {
        return false;
    }
    if (out_from != NULL) {
        *out_from = from;
    }
    if (out_to != NULL) {
        *out_to = to;
    }
    return true;
}

void mesh_ui_series_project_over(const struct mesh_ui_series *series, struct mesh_ui_scale scale,
                                 uint32_t from, uint32_t to, struct mesh_ui_polyline *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (series == NULL || series->count == 0U) {
        return;
    }

    const uint32_t span = to > from ? to - from : 0U;
    const uint32_t last = series->count > 1U ? series->count - 1U : 1U;

    uint32_t previous = 0U;
    for (uint32_t i = 0U; i < series->count; ++i) {
        const struct mesh_ui_sample *sample = mesh_ui_series_at(series, i);
        struct mesh_ui_point *point = &out->items[i];
        /* Across the window - or evenly, when it has no width at all, which is the one case the
           sample number is the axis and is what `span == 0` means here. */
        int32_t x = (int32_t)(((uint64_t)i * MESH_UI_ANIM_ONE) / last);
        if (span > 0U) {
            /* Held at the edge it fell off, rather than wrapped: `sample->time - from` is
               unsigned, so a reading older than the window would otherwise come out as a point
               most of a picture to the right of everything it happened before. */
            if (sample->time <= from) {
                x = 0;
            } else if (sample->time >= to) {
                x = MESH_UI_ANIM_ONE;
            } else {
                x = (int32_t)(((uint64_t)(sample->time - from) * (uint64_t)MESH_UI_ANIM_ONE) /
                              span);
            }
        }
        point->x = (int16_t)x;
        point->y = (int16_t)mesh_ui_scale_permille(scale, sample->value);
        point->gap = series_breaks_at(series, sample, i, previous);
        previous = sample->time;
    }
    out->count = series->count;
}
