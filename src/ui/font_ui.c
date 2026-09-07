/*
 * The UI face's descriptor: a lookup into the generated table, and the geometry above it.
 *
 * There is almost nothing here because there is almost nothing to decide - the shapes were
 * decided by scripts/gen-font.py, and the metrics are a table entry. That is the point of the
 * font seam: a second face is data plus a descriptor, not a renderer.
 */

#include "mesh/ui/font_ui.h"

#include <string.h>

/* The glyph `codepoint` is at, or the table's count when it has none. Bisection rather than a
   scan: a full screen of body text is several hundred lookups a frame. */
static uint32_t font_ui_find(uint32_t codepoint) {
    const struct mesh_ui_font_ui_table *table = &mesh_ui_font_ui_table;
    uint32_t low = 0U;
    uint32_t high = table->count;
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2U;
        if (table->glyphs[mid].codepoint < codepoint) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    return (low < table->count && table->glyphs[low].codepoint == codepoint) ? low : table->count;
}

static bool font_ui_has_glyph(uint32_t codepoint) {
    return font_ui_find(codepoint) < mesh_ui_font_ui_table.count;
}

/*
 * The replacement box: a hollow rectangle over the cap height, the "tofu" a reader already
 * knows means "a character this font does not have".
 *
 * Drawn rather than stored, because it is a rectangle and storing it would be a glyph in the
 * table that no codepoint maps to. The proportions are the 5x7 tofu's, scaled: a box inset from
 * the cell's sides, standing on the baseline and as tall as a capital.
 */
static void font_ui_tofu(uint8_t *out) {
    const int inset = MESH_UI_FONT_UI_MASTER_W / 8;
    const int left = inset;
    const int right = MESH_UI_FONT_UI_MASTER_W - inset - 1;
    const int top = MESH_UI_FONT_UI_MASTER_TOP + 4;
    const int bottom = MESH_UI_FONT_UI_MASTER_H - 5;
    const int stroke = 2;
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const bool edge = y < top + stroke || y > bottom - stroke || x < left + stroke ||
                              x > right - stroke;
            if (edge) {
                out[(size_t)y * MESH_UI_FONT_UI_MASTER_W + (size_t)x] = MESH_UI_GLYPH_MAX_ALPHA;
            }
        }
    }
}

/* Unpack one glyph's ink box into its place in the master, which mesh_ui_font_glyph() has
   already cleared. A glyph with no ink - a space - writes nothing and needs no test for it. */
static bool font_ui_glyph(uint32_t codepoint, struct mesh_ui_glyph *out) {
    const struct mesh_ui_font_ui_table *table = &mesh_ui_font_ui_table;
    const uint32_t index = font_ui_find(codepoint);
    if (index >= table->count) {
        font_ui_tofu(out->alpha);
        return false;
    }

    const struct mesh_ui_font_ui_glyph *glyph = &table->glyphs[index];
    for (uint32_t row = 0U; row < glyph->h; ++row) {
        uint8_t *dst = &out->alpha[((size_t)glyph->y + row) * MESH_UI_FONT_UI_MASTER_W + glyph->x];
        for (uint32_t col = 0U; col < glyph->w; ++col) {
            const size_t at = (size_t)row * glyph->w + col;
            const uint8_t packed = table->pixels[glyph->offset + at / 2U];
            dst[col] = (at & 1U) != 0U ? (uint8_t)(packed >> 4) : (uint8_t)(packed & 0x0FU);
        }
    }
    return true;
}

const struct mesh_ui_font *mesh_ui_font_ui(void) {
    /*
     * Not `static const`, unlike 5x7's, for one field: the cap height is a property of the face
     * the generator rasterised, so it travels with the generated data rather than being a
     * constant here that a regeneration could silently disagree with. Copying it on every call
     * is cheaper than the branch an initialised flag would need.
     */
    /*
     * No gap between cells, one row between lines.
     *
     * 5x7 asks for a column of gap because its glyphs fill their cell edge to edge; this face
     * carries its own sidebearings, and a 20 px cell holds 17 px of ink, so a gap on top of
     * that spaces the letters like a ransom note. The cell is the advance instead, which is
     * what a monospace face means by one.
     *
     * The line is 36 px at the body scale either way - the same rows per screen as 5x7 - because
     * the cell took the row the accent gap used to need: the diacritics live in the overhang
     * now rather than in the space between lines.
     */
    static struct mesh_ui_font font = {
        .id = "ui",
        .name = "JetBrains Mono",
        .width = MESH_UI_FONT_UI_CELL_W,
        .height = MESH_UI_FONT_UI_CELL_H,
        .advance_gap = 0U,
        .line_gap = 1U,
        .master_w = MESH_UI_FONT_UI_MASTER_W,
        .master_h = MESH_UI_FONT_UI_MASTER_H,
        .master_top = MESH_UI_FONT_UI_MASTER_TOP,
        .sampling = MESH_UI_FONT_SMOOTH,
        .glyph = font_ui_glyph,
        .has_glyph = font_ui_has_glyph,
    };
    font.cap_rows = mesh_ui_font_ui_table.cap_rows;
    return &font;
}
