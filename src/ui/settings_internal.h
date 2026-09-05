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

#include "mesh/ui/settings.h"

#include <stddef.h>
#include <stdint.h>

/* What one editable field is: its label, how it is edited, and what values it will take. */
struct field_spec {
    const char *label;
    enum mesh_ui_setting_kind kind;
    enum mesh_ui_settings_section section;
    uint32_t limit; /* TEXT: max bytes; ENUM: value count */
    const char *(*enum_name)(uint32_t value);
    const uint32_t *presets; /* NUMBER */
    size_t preset_count;
    const char *zero_label; /* NUMBER: what 0 means (seconds formatting) */
    void (*format)(uint32_t value, char *out, size_t out_len); /* NUMBER: overrides seconds */
    uint32_t choices; /* KEY: MESH_UI_PSK_CHOICE_BIT mask Left/Right walk */
};

/* The spec for `field`, never NULL - an unknown field yields the MESH_UI_FIELD_NONE row. */
const struct field_spec *field_spec(enum mesh_ui_setting_field field);

#endif /* MESH_UI_SETTINGS_INTERNAL_H */
