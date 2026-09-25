#ifndef MESH_UI_SETTINGS_INTERNAL_H
#define MESH_UI_SETTINGS_INTERNAL_H

/*
 * The one seam between the settings field model and the rows built from it.
 *
 * settings.c was 1700 lines covering three jobs that only meet here: describing what each field
 * is (settings.c), turning values into text and back (settings_codec.c), and building the rows a
 * screen draws (settings_rows.c). The codec is a leaf - it exports only public API - so this
 * header carries just the field table's accessor.
 *
 * Not public: include/mesh/ui/settings.h is. struct field_spec deliberately stays out of there,
 * because everything outside this directory should be asking through
 * mesh_ui_settings_field_label() and friends rather than reading the table.
 */

#include "inkstand/form/field.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Every TEXT and KEY field's byte limit, named so a `k_fields` row can state its own without
 * writing the number down a second time.
 *
 * The list itself is mesh/ui/settings_text.def, because the edit buffer is sized from the same
 * rows (MESH_UI_SETTING_TEXT_MAX, mesh/ui/nav.h) and a limit written in two places is a limit
 * that will eventually be raised in one of them. See that file for why the buffer is measured
 * rather than declared.
 */
enum mesh_ui_setting_text_limit {
#define MESH_UI_TEXT_FIELD(name, bytes) MESH_UI_TEXT_LIMIT_##name = (bytes),
#include "mesh/ui/settings_text.def"
#undef MESH_UI_TEXT_FIELD
};

/*
 * What one editable field is, in this client's table.
 *
 * `form` is the part any form has - label, kind, section, limits, presets, note - and is
 * inkstand's shape (inkstand's form/field.h), read through k_form's stride. What follows it is this
 * client's alone. k_fields is initialised positionally, so the order here is part of every row:
 * a column added anywhere but the end renumbers all of them, and the braces around `form` are
 * what make a row that forgets one a type error.
 */
struct field_spec {
    struct inkstand_form_field form;
    /* NUMBER: what 0 means, for the seconds formatter. INKCELL_STR_NONE means 0 has no special
       name. A catalog id rather than text, so the table says what a row *is* and src/i18n
       answers what it is called. */
    inkcell_str_id zero_label;
    /*
     * NUMBER: overrides the seconds default. `imperial` is the radio's display units, decoded
     * by mesh_ui_units_imperial(), and every formatter takes it whether or not it has anything
     * to say with it - a length row that quietly kept metres while the map beside it read in
     * miles is the whole reason this argument exists, and a signature that made it optional
     * would let the next one do the same.
     */
    void (*format)(uint32_t value, bool imperial, char *out, size_t out_len);
};

/* The form k_fields is: every mesh_ui_settings_* field question is asked of it. */
extern const struct inkstand_form mesh_ui_settings_form;

/* The spec for `field`, never NULL - an unknown field yields the MESH_UI_FIELD_NONE row. */
const struct field_spec *field_spec(enum mesh_ui_setting_field field);

#endif /* MESH_UI_SETTINGS_INTERNAL_H */
