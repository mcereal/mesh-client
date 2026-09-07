#ifndef MESH_UI_FONT_UI_H
#define MESH_UI_FONT_UI_H

/*
 * The UI face: a real monospace outline, rasterised into coverage.
 *
 * The 5x7 font beside it is pixel art, and pixel art is an aesthetic before it is a typeface -
 * at the body scale one of its pixels is a 4x4 block, so every screen reads as a handheld from
 * 1989 whatever the palette and the components do. That is a fine thing to want and a bad thing
 * to be stuck with, which is why this is a second entry in the font registry rather than a
 * replacement: a theme names the one it wants.
 *
 * Everything here is generated. scripts/gen-font.py rasterises the face at the exact cell the
 * device draws - so on the Brick a glyph is blitted 1:1 and resampled only when a theme asks
 * for another scale - and writes src/ui/font_ui_glyphs.c, which is committed. The build
 * rasterises nothing and never reaches the network, the same arrangement the icon set has.
 *
 * The cell is 5x8 at scale 1 against 5x7's 5x7, and the gaps are one and one against one and
 * two. Both work out to the same 24 px advance and 36 px line at the body scale, which is not a
 * coincidence: the column and row counts every screen is laid out against are the same under
 * either font, so switching between them is a change of typeface and nothing else.
 */

#include "mesh/ui/font.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The cell and the overhang above it, in pixels at scale 1. The master is four times each,
   because the generator rasterises at the size the body scale draws - so on the device a glyph
   is blitted 1:1 and resampled only when a theme asks for another scale. */
#define MESH_UI_FONT_UI_CELL_W 5
#define MESH_UI_FONT_UI_CELL_H 8
#define MESH_UI_FONT_UI_OVERHANG 1

/* The same, in master rows: what the generator rasterises and what the table below holds. */
#define MESH_UI_FONT_UI_MASTER_W (MESH_UI_FONT_UI_CELL_W * 4)
#define MESH_UI_FONT_UI_MASTER_TOP (MESH_UI_FONT_UI_OVERHANG * 4)
#define MESH_UI_FONT_UI_MASTER_H ((MESH_UI_FONT_UI_CELL_H + MESH_UI_FONT_UI_OVERHANG) * 4)

/*
 * One glyph: where its ink sits in the master, and where its pixels start.
 *
 * Only the ink box is stored, not the whole master. The icon set run-length encodes instead,
 * and the difference is what the data is: a symbol is long runs of solid and long runs of
 * nothing, where a letter at this size is a small dense patch of varying coverage inside a
 * mostly empty cell. Runs over the whole master came to 95 KB of table; the box and four bits a
 * pixel is 50, and decodes with less arithmetic.
 *
 * `x`/`y` are the box's corner in the master, `w`/`h` its size - all zero for a glyph with no
 * ink at all, which is what a space is. `offset` indexes `pixels`, where the box is packed two
 * coverage values to a byte, low nibble first, row by row.
 */
struct mesh_ui_font_ui_glyph {
    uint32_t codepoint;
    uint32_t offset;
    uint8_t x, y, w, h;
};

/* The generated table. `glyphs` is ascending by codepoint, so a lookup bisects rather than
   scanning three hundred entries per character drawn. */
struct mesh_ui_font_ui_table {
    const uint8_t *pixels;
    const struct mesh_ui_font_ui_glyph *glyphs;
    uint32_t count;
    /* Master rows from the baseline to the cap top, measured by the generator off the face it
       rasterised. Here rather than in a constant so that pointing gen-font.py at another face
       brings that face's proportions with it. */
    uint8_t cap_rows;
};

extern const struct mesh_ui_font_ui_table mesh_ui_font_ui_table;

/* This face as a `struct mesh_ui_font`, which is how a theme reaches it. */
const struct mesh_ui_font *mesh_ui_font_ui(void);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_FONT_UI_H */
