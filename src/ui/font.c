/*
 * The font registry and the metrics every measurement in the UI derives from.
 *
 * Two fonts: a rasterised monospace face and the 5x7 pixel one. A font is the half of "themes"
 * that a palette does not cover - a 1-bit 5x7 cell is an aesthetic that survives any number of
 * Material components laid over it - so which face a theme draws in is a property of the theme,
 * resolved here.
 *
 * Index order is menu order and slot 0 is the default, which is why the rasterised face is
 * first. The registry is a switch rather than a table because a descriptor comes from a
 * function call, which C will not let a static table hold.
 */

#include "mesh/ui/font.h"

#include "mesh/ui/font5x7.h"
#include "mesh/ui/font_ui.h"

#include <string.h>

/* Every accessor goes through this, so "there is always a font" lives in one place. */
static const struct mesh_ui_font *font_slot(size_t index) {
    switch (index) {
    case 0:
        return mesh_ui_font_ui();
    case 1:
        return mesh_ui_font5x7();
    default:
        return NULL;
    }
}

size_t mesh_ui_font_count(void) { return 2U; }

const struct mesh_ui_font *mesh_ui_font_at(size_t index) { return font_slot(index); }

const struct mesh_ui_font *mesh_ui_font_default(void) { return font_slot(0U); }

const struct mesh_ui_font *mesh_ui_font_by_id(const char *id) {
    if (id == NULL || id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < mesh_ui_font_count(); ++i) {
        const struct mesh_ui_font *font = font_slot(i);
        if (font != NULL && font->id != NULL && strcmp(font->id, id) == 0) {
            return font;
        }
    }
    return NULL;
}

static const struct mesh_ui_font *font_or_default(const struct mesh_ui_font *font) {
    return font != NULL ? font : mesh_ui_font_default();
}

int mesh_ui_font_advance(const struct mesh_ui_font *font, int scale) {
    font = font_or_default(font);
    if (font == NULL || scale <= 0) {
        return 1;
    }
    return (int)font->width * scale + (int)font->advance_gap * scale;
}

int mesh_ui_font_line(const struct mesh_ui_font *font, int scale) {
    font = font_or_default(font);
    if (font == NULL || scale <= 0) {
        return 1;
    }
    return (int)font->height * scale + (int)font->line_gap * scale;
}

/*
 * Clearing only the master the font actually uses, rather than the whole struct.
 *
 * `struct mesh_ui_glyph` is sized for the largest master any font may declare, and a full
 * frame of body text is several hundred of these calls - zeroing the unused tail of every one
 * would be most of the work of drawing a character. The font fills the rest.
 */
/*
 * The capitals' height in pixels, from the master rows they occupy.
 *
 * The cell is `master_h - master_top` master rows and is drawn `height * scale` pixels tall, so
 * a cap is that many pixels per master row times the rows it stands. For 5x7 the arithmetic
 * cancels back to `height * scale`, which is what it always was.
 */
int mesh_ui_font_cap(const struct mesh_ui_font *font, int scale) {
    font = font_or_default(font);
    if (font == NULL || scale <= 0) {
        return 1;
    }
    const int cell_rows = (int)font->master_h - (int)font->master_top;
    if (cell_rows <= 0 || font->cap_rows == 0U) {
        return (int)font->height * scale;
    }
    const int cap = (int)font->cap_rows * (int)font->height * scale / cell_rows;
    return cap > 0 ? cap : 1;
}

bool mesh_ui_font_glyph(const struct mesh_ui_font *font, uint32_t codepoint,
                        struct mesh_ui_glyph *out) {
    if (out == NULL) {
        return false;
    }
    font = font_or_default(font);
    if (font == NULL || font->glyph == NULL) {
        memset(out, 0, sizeof *out);
        return false;
    }
    memset(out->alpha, 0, (size_t)font->master_w * (size_t)font->master_h);
    return font->glyph(codepoint, out);
}

bool mesh_ui_font_has_glyph(const struct mesh_ui_font *font, uint32_t codepoint) {
    font = font_or_default(font);
    if (font == NULL || font->has_glyph == NULL) {
        return false;
    }
    return font->has_glyph(codepoint);
}
