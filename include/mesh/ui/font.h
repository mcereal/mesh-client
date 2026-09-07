#ifndef MESH_UI_FONT_H
#define MESH_UI_FONT_H

/*
 * The font seam.
 *
 * A renderer asks a font how wide a cell is and what a codepoint looks like; it never names
 * 5x7. That is the whole of what a second font needs from this layer - drop in another
 * `struct mesh_ui_font` and every measurement above it (columns per line, button widths,
 * bubble heights, the scroll window) follows, because all of them derive from
 * mesh_ui_font_advance()/mesh_ui_font_line() rather than from a constant.
 *
 * A glyph is *coverage*, not a bitmask: `alpha` holds one value per master pixel, 0 for
 * nothing and MESH_UI_GLYPH_MAX_ALPHA for solid, exactly as an icon sprite does. A 1-bit
 * mask is what welds the UI to a retro look - at the body scale one source pixel is a 4x4
 * block, and no amount of Material chrome survives text made of visible squares. Coverage
 * lets a face rasterised from a real outline keep its curves, and costs the pixel-art font
 * nothing: 5x7 stores 0 or 15 and draws exactly the spans it always drew.
 *
 * The master is resampled into the cell the theme asks for, which is why a font declares
 * both its cell (`width`/`height`, in scale steps, what every measurement is derived from)
 * and its master (`master_w`/`master_h`, the resolution the coverage is actually stored at).
 * For 5x7 the two are the same and the sampling is nearest, so a glyph is block-replicated
 * the way it always was. For a rasterised face the master is bigger than the cell at most
 * scales and the sampling is bilinear, the same trade fb_draw_icon() already makes: nearest
 * neighbour on a 2 px stroke is the difference between a smooth diagonal and a staircase.
 *
 * The overhang - `master_top` - is where a diacritic goes when the cell has no room for it.
 * Every font this small has the problem: 5x7's capitals fill all seven of its rows, and a face
 * rasterised to fill its cell puts an acute above the cap height by definition. So the master
 * is allowed to be taller than the cell, its top `master_top` rows are drawn in the gap the
 * line advance leaves above, and a font sizes that gap by its own `line_gap`. One mechanism
 * for both fonts, rather than the single hard-coded accent row this replaced.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest *cell* this layer will render, in pixels at scale 1. Buffers and bounds sized
   off these are the reason a font cannot simply declare any size it likes; raise them when a
   font needs it. */
#define MESH_UI_GLYPH_MAX_WIDTH 8
#define MESH_UI_GLYPH_MAX_HEIGHT 16

/* The largest coverage *master* a font may store a glyph at. Independent of the cell: a face
   is rasterised once at a resolution that survives being drawn small, and the cell it lands
   in is whatever the theme's scale works out to. */
#define MESH_UI_GLYPH_MASTER_MAX_WIDTH 24
#define MESH_UI_GLYPH_MASTER_MAX_HEIGHT 40

/* Solid. Coverage runs 0..this inclusive, 4 bits, the same range an icon sprite carries. */
#define MESH_UI_GLYPH_MAX_ALPHA 15

/*
 * How a master is resampled into the cell.
 *
 * Not a preference: pixel art scaled up bilinearly reads as a smudge, and an outline scaled
 * down with nearest neighbour drops every fourth row. A font knows which of the two it is,
 * so it says so rather than leaving the renderer to guess from its dimensions.
 */
enum mesh_ui_font_sampling {
    MESH_UI_FONT_PIXEL = 0, /* nearest neighbour: block-replicated, edges stay hard */
    MESH_UI_FONT_SMOOTH,    /* bilinear: an outline keeps its curves at any cell size */
};

/* One character's coverage, row-major over the font's master. */
struct mesh_ui_glyph {
    uint8_t alpha[MESH_UI_GLYPH_MASTER_MAX_WIDTH * MESH_UI_GLYPH_MASTER_MAX_HEIGHT];
};

/*
 * A bitmap font as the UI sees it.
 *
 * `advance_gap` and `line_gap` are in pixels *per scale step*, so a font keeps its proportions
 * when the glyph multiplier changes: the 5x7 font asks for one column of gap and two rows,
 * which at scale 4 is the 4 px and 8 px the Brick's panel has always drawn.
 */
struct mesh_ui_font {
    const char *id;   /* what a theme names it by */
    const char *name; /* what a menu would show */
    uint8_t width;    /* cell width in pixels at scale 1 */
    uint8_t height;   /* cell height in pixels at scale 1 */
    uint8_t advance_gap;
    uint8_t line_gap;
    uint8_t master_w; /* coverage master width; the cell width when the font is pixel art */
    uint8_t master_h; /* coverage master height, overhang rows included */
    /* How many of the master's top rows hang *above* the cell rather than inside it.
       See the note on the overhang below. */
    uint8_t master_top;
    /*
     * How many master rows the capitals stand on the baseline, which is not the cell.
     *
     * Anything sized to match the text - an icon in a row slot, above all - wants the height of
     * a capital beside it, and for 5x7 that is the cell, because its capitals fill it. A face
     * with real ascenders and descenders keeps both inside the cell, so its capitals are a good
     * bit shorter than it - and an icon sized off the cell there comes out overbearing.
     */
    uint8_t cap_rows;
    enum mesh_ui_font_sampling sampling;
    /* Fills `out` with the glyph for `codepoint`. Returns false when it fell back to the
       replacement box, which is still a drawable glyph. */
    bool (*glyph)(uint32_t codepoint, struct mesh_ui_glyph *out);
    /* Whether the font has a real glyph for `codepoint`, without building it. */
    bool (*has_glyph)(uint32_t codepoint);
};

/* Pixels from one cell's origin to the next, and from one baseline to the next. */
int mesh_ui_font_advance(const struct mesh_ui_font *font, int scale);
int mesh_ui_font_line(const struct mesh_ui_font *font, int scale);

/* How tall a capital is drawn, in pixels - what anything standing beside the text matches. */
int mesh_ui_font_cap(const struct mesh_ui_font *font, int scale);

/* Safe wrappers: a NULL font, or one missing a hook, resolves to the default. */
bool mesh_ui_font_glyph(const struct mesh_ui_font *font, uint32_t codepoint,
                        struct mesh_ui_glyph *out);
bool mesh_ui_font_has_glyph(const struct mesh_ui_font *font, uint32_t codepoint);

/* The registry a theme's `font_id` is resolved against. */
size_t mesh_ui_font_count(void);
const struct mesh_ui_font *mesh_ui_font_at(size_t index);
const struct mesh_ui_font *mesh_ui_font_by_id(const char *id); /* NULL when unknown */
const struct mesh_ui_font *mesh_ui_font_default(void);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_FONT_H */
