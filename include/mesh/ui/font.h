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
 * Glyphs are column-major bitmaps: `columns[c]` bit N is the pixel at column c, row N, row 0
 * at the top. `above` is the single extra row a font may draw immediately above the cell for
 * accents that do not fit inside it - see src/ui/font5x7.c for why that row exists.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest cell this layer will render. Buffers sized off these are the reason a font
   cannot simply declare any size it likes; raise them when a font needs it. */
#define MESH_UI_GLYPH_MAX_WIDTH 8
#define MESH_UI_GLYPH_MAX_HEIGHT 16

struct mesh_ui_glyph {
    uint16_t columns[MESH_UI_GLYPH_MAX_WIDTH];
    uint16_t above[MESH_UI_GLYPH_MAX_WIDTH];
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
    /* Fills `out` with the glyph for `codepoint`. Returns false when it fell back to the
       replacement box, which is still a drawable glyph. */
    bool (*glyph)(uint32_t codepoint, struct mesh_ui_glyph *out);
    /* Whether the font has a real glyph for `codepoint`, without building it. */
    bool (*has_glyph)(uint32_t codepoint);
};

/* Pixels from one cell's origin to the next, and from one baseline to the next. */
int mesh_ui_font_advance(const struct mesh_ui_font *font, int scale);
int mesh_ui_font_line(const struct mesh_ui_font *font, int scale);

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
