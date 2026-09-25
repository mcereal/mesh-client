/*
 * A form's rows, read through the application's table - see mesh/ui/form_field.h.
 */

#include "mesh/ui/form_field.h"

const struct mesh_ui_form_field *mesh_ui_form_field(const struct mesh_ui_form *form, uint16_t id) {
    /* Row 0 answers for every id the table does not have, which is what makes every accessor
       below safe to call with whatever a caller is holding. */
    if (id >= form->count) {
        id = 0U;
    }
    /* Through void: the stride is a whole row of the application's struct, so the address is
       as aligned as the table itself, and the byte pointer is only the arithmetic. */
    const void *row = (const unsigned char *)form->fields + (size_t)id * form->stride;
    return (const struct mesh_ui_form_field *)row;
}

bool mesh_ui_form_section_has_fields(const struct mesh_ui_form *form, uint16_t section) {
    for (uint16_t id = 1U; id < form->count; ++id) {
        if (mesh_ui_form_field(form, id)->section == section) {
            return true;
        }
    }
    return false;
}

uint32_t mesh_ui_form_bit(const struct mesh_ui_form *form, uint16_t id) {
    const struct mesh_ui_form_field *field = mesh_ui_form_field(form, id);
    return field->kind == MESH_UI_SETTING_FLAG ? field->limit : 0U;
}

uint32_t mesh_ui_form_enum_count(const struct mesh_ui_form *form, uint16_t id) {
    const struct mesh_ui_form_field *field = mesh_ui_form_field(form, id);
    return field->kind == MESH_UI_SETTING_ENUM ? field->limit : 0U;
}

const char *mesh_ui_form_enum_name(const struct mesh_ui_form *form, uint16_t id, uint32_t value) {
    const struct mesh_ui_form_field *field = mesh_ui_form_field(form, id);
    if (field->kind != MESH_UI_SETTING_ENUM || field->enum_name == NULL) {
        return inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT);
    }
    return field->enum_name(value);
}

/*
 * The field's own limit, unclamped. An edit buffer is best measured from these limits rather
 * than the other way round: clamped to a buffer, a field wider than it would be offered a
 * shorter keyboard cap and nothing anywhere would say the value had been cut.
 */
uint32_t mesh_ui_form_text_max(const struct mesh_ui_form *form, uint16_t id) {
    const struct mesh_ui_form_field *field = mesh_ui_form_field(form, id);
    if (field->kind != MESH_UI_SETTING_TEXT && field->kind != MESH_UI_SETTING_KEY) {
        return 0U;
    }
    return field->limit;
}

uint32_t mesh_ui_form_key_choices(const struct mesh_ui_form *form, uint16_t id) {
    const struct mesh_ui_form_field *field = mesh_ui_form_field(form, id);
    return field->kind == MESH_UI_SETTING_KEY ? field->choices : 0U;
}

uint32_t mesh_ui_form_number_step(const struct mesh_ui_form *form, uint16_t id, uint32_t value,
                                  int delta) {
    const struct mesh_ui_form_field *field = mesh_ui_form_field(form, id);
    if (field->kind != MESH_UI_SETTING_NUMBER) {
        return value;
    }
    return inkstand_form_presets_step(&field->presets, value, delta);
}

bool mesh_ui_form_number_track(const struct mesh_ui_form *form, uint16_t id, uint32_t value,
                               struct inkstand_form_track *out) {
    const struct mesh_ui_form_field *field = mesh_ui_form_field(form, id);
    if (field->kind != MESH_UI_SETTING_NUMBER) {
        return false;
    }
    return inkstand_form_presets_track(&field->presets, value, out);
}
