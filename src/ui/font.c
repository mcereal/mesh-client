/*
 * The font registry and the metrics every measurement in the UI derives from.
 *
 * There is one font today. The seam is here anyway because the alternative - MESH_FONT_WIDTH
 * spelled into a dozen call sites - is what makes adding a second one a refactor rather than a
 * table entry, and a font is the half of "themes" that a palette does not cover.
 */

#include "mesh/ui/font.h"

#include "mesh/ui/font5x7.h"

#include <string.h>

/*
 * The registry.
 *
 * A switch rather than a table because a descriptor comes from a function call, which C will
 * not let a static table hold. Every accessor goes through it, so "there is always a font"
 * lives in one place.
 */
static const struct mesh_ui_font *font_slot(size_t index) {
    switch (index) {
    case 0:
        return mesh_ui_font5x7();
    default:
        return NULL;
    }
}

size_t mesh_ui_font_count(void) { return 1U; }

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
    memset(out->above, 0, font->master_w);
    return font->glyph(codepoint, out);
}

bool mesh_ui_font_has_glyph(const struct mesh_ui_font *font, uint32_t codepoint) {
    font = font_or_default(font);
    if (font == NULL || font->has_glyph == NULL) {
        return false;
    }
    return font->has_glyph(codepoint);
}
