#ifndef MESH_UI_FONT5X7_H
#define MESH_UI_FONT5X7_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_FONT_WIDTH 5
#define MESH_FONT_HEIGHT 7

/*
 * One character of the framebuffer font.
 *
 * `columns` is the 5x7 cell itself, column-major: `columns[c]` bit N is the pixel at column c,
 * row N, row 0 at the top.
 *
 * `above` is a single extra row drawn immediately above the cell, and is zero for all but
 * accented capitals. The cell has no room for their diacritic - this font's capitals and
 * ascenders occupy all seven rows, unlike its lowercase, which starts at row 2 - so the mark
 * goes into the top half of the two-pixel gap `fb_line_adv` leaves between lines. One row is
 * all a mark gets there, which is why every capital's mark collapses to a horizontal
 * silhouette: O-circumflex, O-macron and O-ring are all "O with something over it". The letter
 * underneath is always right, which is what makes the name readable.
 */
struct mesh_font_glyph {
    uint8_t columns[MESH_FONT_WIDTH];
    uint8_t above[MESH_FONT_WIDTH];
};

/*
 * Fill `out` with the glyph for `codepoint`.
 *
 * Returns true when the font has a real glyph for it, false when it fell back to the
 * replacement box - the "tofu" every text stack draws for a character it cannot render. A
 * caller that just wants pixels can ignore the return value; the box is always a valid glyph.
 */
bool mesh_font5x7_glyph(uint32_t codepoint, struct mesh_font_glyph *out);

/* Whether the font has a real glyph for `codepoint`, without building it. */
bool mesh_font5x7_has_glyph(uint32_t codepoint);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_FONT5X7_H */
