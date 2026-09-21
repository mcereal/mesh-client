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
 * What one editable field is: its label, how it is edited, and what values it will take.
 *
 * `label` and `zero_label` are catalog ids rather than text, so the table describes what a row
 * *is* and src/i18n answers what it is called. INKCELL_STR_NONE in zero_label means the field has
 * no special name for 0 and the seconds formatter handles it.
 */
struct field_spec {
    inkcell_str_id label;
    enum mesh_ui_setting_kind kind;
    enum mesh_ui_settings_section section;
    /*
     * What values this field will take, which is a different number for each kind that has an
     * opinion: TEXT the byte cap, ENUM how many values there are, FLAG *which bit* of its
     * group's word the row is.
     *
     * The third reading is the one worth arguing for. A mask is not a limit in English - but it
     * is the same thing to this table, which is "what this field's values are", and the
     * alternative is an eleventh member that every one of the rows below would have to state
     * for the sake of the ten that mean anything by it. See the note on `note` for why a member
     * is not free here.
     */
    uint32_t limit;
    const char *(*enum_name)(uint32_t value);
    const uint32_t *presets; /* NUMBER */
    size_t preset_count;
    /*
     * NUMBER: whether the presets *measure* something or *name* something.
     *
     * The difference is not in the numbers and cannot be derived from them - {0, 1, 2, 3, 4, 5,
     * 6, 7} is a hop limit in one row and a GPIO pin in another, and a length drawn across the
     * second says a pin is two thirds of the way to being a pin. So every field states which it
     * is, through SCALE_PRESETS() or NAMED_PRESETS(), and only a scale is offered as a slider.
     */
    bool preset_scale;
    /*
     * NUMBER: whether the first preset is a *word* standing outside that scale.
     *
     * Most of these lists open with a 0 the field reads as "whatever the firmware picks", and
     * one of them - LoRa's transmit power - reads it as "as much as this radio has". Neither is
     * a quantity, and both were drawn at the bottom of the track by the first version of the
     * slider: "max" with its handle hard left, which is not merely unhelpful but backwards.
     *
     * So the scale is the presets *after* it, stated with SCALE_PRESETS_AFTER_ZERO(), and a
     * value of 0 on such a field is off the track rather than at the start of it.
     */
    bool preset_zero_aside;
    inkcell_str_id zero_label; /* NUMBER: what 0 means (seconds formatting) */
    /*
     * NUMBER: overrides the seconds default. `imperial` is the radio's display units, decoded
     * by mesh_ui_units_imperial(), and every formatter takes it whether or not it has anything
     * to say with it - a length row that quietly kept metres while the map beside it read in
     * miles is the whole reason this argument exists, and a signature that made it optional
     * would let the next one do the same.
     */
    void (*format)(uint32_t value, bool imperial, char *out, size_t out_len);
    uint32_t choices; /* KEY: MESH_UI_PSK_CHOICE_BIT mask Left/Right walk */
    /*
     * What this setting does, in a sentence or two, for the help screen - and INKCELL_STR_NONE
     * for a row whose label is already the whole explanation.
     *
     * Last in the struct rather than beside `label`, where it belongs by meaning, because
     * k_fields is initialised *positionally*: a member added in the middle would renumber
     * every one of the hundred and fifty rows below it, silently and in a way the compiler
     * would only catch where the types happened to disagree.
     *
     * Every row states it, INKCELL_STR_NONE included, which is what makes that renumbering a
     * diagnostic rather than a silence. Leaving it off and relying on the trailing member
     * being zeroed says the same thing to the language and nothing at all to a reader - and
     * it is a -Wmissing-field-initializers on each such row under clang, which is fifty-two
     * warnings standing between a CI log and the one that would matter.
     *
     * See docs/help.md for what a note may say and how long it may be.
     */
    inkcell_str_id note;
};

/* The spec for `field`, never NULL - an unknown field yields the MESH_UI_FIELD_NONE row. */
const struct field_spec *field_spec(enum mesh_ui_setting_field field);

#endif /* MESH_UI_SETTINGS_INTERNAL_H */
