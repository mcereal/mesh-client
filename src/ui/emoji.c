#include "mesh/ui/emoji.h"

#include "mesh/text.h"
#include "mesh/ui/font5x7.h"

#include <string.h>

/* Deepest sequence the generator emits is nine codepoints; the walker never needs more. */
#define EMOJI_MAX_LOOKAHEAD 16

static const struct mesh_emoji_single *emoji_find_single(uint32_t codepoint) {
    const struct mesh_emoji_table *table = &mesh_emoji_table;
    uint32_t low = 0;
    uint32_t high = table->single_count;
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2U;
        if (table->singles[mid].codepoint < codepoint) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    if (low < table->single_count && table->singles[low].codepoint == codepoint) {
        return &table->singles[low];
    }
    return NULL;
}

/* First entry whose leading codepoint is `codepoint`, or the count when there is none. */
static uint32_t emoji_sequence_lower_bound(uint32_t codepoint) {
    const struct mesh_emoji_table *table = &mesh_emoji_table;
    uint32_t low = 0;
    uint32_t high = table->sequence_count;
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2U;
        if (table->sequences[mid].first < codepoint) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    return low;
}

size_t mesh_emoji_match(const uint32_t *codepoints, size_t count, uint16_t *sprite) {
    if (codepoints == NULL || count == 0U) {
        return 0U;
    }

    const struct mesh_emoji_table *table = &mesh_emoji_table;

    /* Sequences are stored longest first within a leading codepoint, so the first one that
       fits is the greediest match: a regional-indicator pair is a flag, not two letters. */
    for (uint32_t i = emoji_sequence_lower_bound(codepoints[0]);
         i < table->sequence_count && table->sequences[i].first == codepoints[0]; ++i) {
        const struct mesh_emoji_sequence *entry = &table->sequences[i];
        if (entry->length > count) {
            continue;
        }
        const uint32_t *tail = &table->sequence_tail[entry->tail];
        bool matched = true;
        for (uint8_t part = 1; part < entry->length; ++part) {
            if (codepoints[part] != tail[part - 1U]) {
                matched = false;
                break;
            }
        }
        if (matched) {
            if (sprite != NULL) {
                *sprite = entry->sprite;
            }
            return entry->length;
        }
    }

    const struct mesh_emoji_single *single = emoji_find_single(codepoints[0]);
    if (single != NULL) {
        if (sprite != NULL) {
            *sprite = single->sprite;
        }
        return 1U;
    }
    return 0U;
}

void mesh_emoji_decode(uint16_t sprite, uint8_t out[MESH_EMOJI_SIZE * MESH_EMOJI_SIZE]) {
    const size_t pixels = (size_t)MESH_EMOJI_SIZE * MESH_EMOJI_SIZE;
    if (out == NULL) {
        return;
    }
    memset(out, MESH_EMOJI_TRANSPARENT, pixels);

    const struct mesh_emoji_table *table = &mesh_emoji_table;
    const uint32_t start = table->run_offsets[sprite];
    const uint32_t end = table->run_offsets[sprite + 1U];

    size_t written = 0;
    for (uint32_t run = start; run < end && written < pixels; ++run) {
        const uint8_t count = table->runs[run * 2U];
        const uint8_t index = table->runs[run * 2U + 1U];
        for (uint8_t i = 0; i < count && written < pixels; ++i) {
            out[written++] = index;
        }
    }
}

bool mesh_emoji_color(uint8_t index, uint8_t rgb[3]) {
    if (index == MESH_EMOJI_TRANSPARENT || rgb == NULL) {
        return false;
    }
    const struct mesh_emoji_table *table = &mesh_emoji_table;
    const uint16_t slot = (uint16_t)(index - 1U);
    if (slot >= table->palette_size) {
        return false;
    }
    rgb[0] = table->palette[slot][0];
    rgb[1] = table->palette[slot][1];
    rgb[2] = table->palette[slot][2];
    return true;
}

bool mesh_emoji_is_zero_width(uint32_t codepoint) {
    return codepoint == 0x200DU ||                              /* zero-width joiner */
           (codepoint >= 0x200BU && codepoint <= 0x200FU) ||     /* zero-width space, marks */
           (codepoint >= 0xFE00U && codepoint <= 0xFE0FU) ||     /* variation selectors */
           (codepoint >= 0x1F3FBU && codepoint <= 0x1F3FFU) ||   /* skin tone modifiers */
           (codepoint >= 0x0300U && codepoint <= 0x036FU) ||     /* combining diacritics */
           (codepoint >= 0x20D0U && codepoint <= 0x20F0U);       /* combining symbols, keycap */
}

struct mesh_ui_text_cell mesh_ui_text_cell_next(const char *text) {
    struct mesh_ui_text_cell cell = {0};
    if (text == NULL || *text == '\0') {
        return cell;
    }

    /* Read ahead far enough for the longest sequence the table holds. */
    uint32_t codepoints[EMOJI_MAX_LOOKAHEAD];
    size_t widths[EMOJI_MAX_LOOKAHEAD];
    size_t count = 0;
    size_t offset = 0;
    while (count < EMOJI_MAX_LOOKAHEAD) {
        const size_t step = mesh_text_utf8_next(&text[offset], &codepoints[count]);
        if (step == 0U) {
            break;
        }
        widths[count++] = step;
        offset += step;
    }

    /*
     * Match against the codepoints with the variation selectors taken out, and remember where
     * each survivor came from.
     *
     * The font spells its keycap ligature U+0039 U+20E3, but the keycap as people actually
     * type it is U+0039 U+FE0F U+20E3 - the selector in the middle is what makes it emoji
     * rather than a digit. Matching the raw sequence misses every one of them.
     */
    uint32_t filtered[EMOJI_MAX_LOOKAHEAD];
    size_t source[EMOJI_MAX_LOOKAHEAD];
    size_t filtered_count = 0;
    for (size_t i = 0; i < count; ++i) {
        if (codepoints[i] >= 0xFE00U && codepoints[i] <= 0xFE0FU) {
            continue;
        }
        filtered[filtered_count] = codepoints[i];
        source[filtered_count] = i;
        filtered_count++;
    }

    /* A stray selector with nothing to attach to: hand back a blank cell rather than a box. */
    if (filtered_count == 0U) {
        cell.codepoint = (uint32_t)' ';
        for (size_t i = 0; i < count; ++i) {
            cell.bytes += widths[i];
        }
        return cell;
    }

    uint16_t sprite = 0;
    size_t matched = mesh_emoji_match(filtered, filtered_count, &sprite);
    /*
     * The emoji font claims '#', '*' and the ten digits, because those lead the keycap
     * sequences. Letting it have them turns the 9 of "Dog Tracker K9" into a tiny grey keycap
     * digit, so a single codepoint the text font can draw is drawn by the text font.
     *
     * A sequence always wins: the extra codepoints are precisely what says "this is the
     * emoji, not the character".
     */
    if (matched == 1U && mesh_font5x7_has_glyph(filtered[0])) {
        matched = 0U;
    }

    /* Consume through the last codepoint the match covered, selectors between them included. */
    size_t consumed;
    if (matched > 0U) {
        cell.is_emoji = true;
        cell.sprite = sprite;
        consumed = source[matched - 1U] + 1U;
    } else {
        cell.codepoint = filtered[0];
        consumed = source[0] + 1U;
    }

    /* Whatever follows and has no width of its own belongs to this cell too: a skin tone or
       selector left over from a sequence the table does not have, or a combining mark. */
    while (consumed < count && mesh_emoji_is_zero_width(codepoints[consumed])) {
        consumed++;
    }
    for (size_t i = 0; i < consumed; ++i) {
        cell.bytes += widths[i];
    }

    return cell;
}

size_t mesh_ui_text_cells(const char *text) {
    if (text == NULL) {
        return 0U;
    }
    size_t cells = 0;
    size_t offset = 0;
    for (;;) {
        const struct mesh_ui_text_cell cell = mesh_ui_text_cell_next(&text[offset]);
        if (cell.bytes == 0U) {
            break;
        }
        offset += cell.bytes;
        cells++;
    }
    return cells;
}

size_t mesh_ui_text_cell_offset(const char *text, size_t index) {
    if (text == NULL) {
        return 0U;
    }
    size_t offset = 0;
    for (size_t i = 0; i < index; ++i) {
        const struct mesh_ui_text_cell cell = mesh_ui_text_cell_next(&text[offset]);
        if (cell.bytes == 0U) {
            break;
        }
        offset += cell.bytes;
    }
    return offset;
}

void mesh_ui_text_cell_truncate(char *text, size_t cells) {
    if (text == NULL) {
        return;
    }
    text[mesh_ui_text_cell_offset(text, cells)] = '\0';
}
