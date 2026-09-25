#ifndef MESH_UI_FORM_FIELD_H
#define MESH_UI_FORM_FIELD_H

/*
 * One row of a form, described: what it is called, what kind of row it is, which section it sits
 * in, and what values it will take.
 *
 * A form is a table of these, and the table is the application's - its rows, its ids, its
 * sections. What this file owns is the *shape* of a row and the questions every caller asks of
 * one, so that "is this a number, and what are its presets" is answered in one place rather than
 * by each screen that draws the row and each handler that edits it.
 *
 * Three decisions are the reason it is shaped the way it is:
 *
 *   - **A field id is an index into the application's table, and 0 is no field.** Row 0 is the
 *     one every out-of-range id resolves to, so a question about a field that does not exist gets
 *     the answers row 0 gives - an INFO row with no values - rather than a read past the table.
 *     Ids are 16 bits: a form of two hundred fields is most of the way to an 8-bit one already.
 *
 *   - **The application extends a row by embedding it.** A row here is the first member of the
 *     application's own row struct, and the form reads the table through a stride, so columns
 *     that are the application's alone - a formatter that knows its units, a word for what 0
 *     means - sit after it in the same table without this file naming them.
 *
 *   - **A kind describes what the row *is*, and every backend draws from it.** A kind is a
 *     description of the content, not of one way of drawing it, so a backend that cannot draw a
 *     switch, a checkbox or a bar shows the same row as a label and a value and loses nothing.
 *
 * Nothing here allocates, and the table is the application's, usually `static const`.
 */

#include "inkcell/i18n/strings.h"
#include "inkstand/form/scale.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mesh_ui_setting_kind {
    MESH_UI_SETTING_INFO = 0, /* a read-only fact */
    MESH_UI_SETTING_TOGGLE,
    /*
     * A boolean that is one *bit* of a larger value, rather than a value of its own.
     *
     * Edited exactly as a toggle is - it carries 0 or 1 like any other - so the edit handler, the
     * pending edits and the save need no new case. What is different is the two ends: whoever
     * builds the row reads its bit out of the word its group names, and whoever writes it back
     * sets or clears that bit in the same word rather than assigning the whole of it. The bit is
     * the row's `limit`.
     *
     * A kind of its own and not a flag on the row, because the *drawing* differs and the drawing
     * is what a kind is for. A switch is a boolean that acts: flick it and the thing it names is
     * on. A flag is one of a set, and the set is only readable as a set. That is the checkbox: a
     * square is "any of these" where a circle is "one of these".
     */
    MESH_UI_SETTING_FLAG,
    MESH_UI_SETTING_ENUM,
    MESH_UI_SETTING_TEXT,
    MESH_UI_SETTING_NUMBER,
    MESH_UI_SETTING_KEY,
    MESH_UI_SETTING_ACTION,
    /*
     * A verb that cannot be pressed right now, and why - "not connected", "not supported",
     * "nothing to drop".
     *
     * An INFO row is right about the press and wrong about the row: nothing happens on either,
     * but INFO is a stated fact and this is an offer that is currently withdrawn. A section whose
     * rows change shape when a link drops moves the cursor out from under the reader, and
     * collapsing a verb to a fact changes its shape in every way but its row count. Drawn as the
     * verb it is: the same symbol in the same column, dimmed, with the reason where the chevron
     * would be.
     *
     * Not pressable, and that is the whole reason it is a kind of its own rather than a flag on
     * ACTION. A handler that answers a press by looking for ACTION refuses a withdrawn verb by
     * construction; spelled as `action + disabled` it would be refused only by everywhere that
     * remembered to ask.
     */
    MESH_UI_SETTING_ACTION_OFF,
    /* A group title inside a long section: dimmed, no value column, and a press on it does
       nothing. A heading is never added or removed by an edit - a row count that moves under the
       cursor mid-edit moves the cursor. */
    MESH_UI_SETTING_HEADING,
    /*
     * A read-only quantity whose *level* is the point: how far a download has got, how much of
     * something is used up. The row's number is permille, or MESH_UI_METER_UNKNOWN while work is
     * happening whose extent cannot be known.
     *
     * The row's words carry the same fact, and that is deliberate rather than redundant: a
     * backend that cannot draw a bar shows it as an ordinary fact and loses nothing.
     */
    MESH_UI_SETTING_METER,
};

/* MESH_UI_SETTING_METER: the number for a step that is running with no fraction to report. */
#define MESH_UI_METER_UNKNOWN UINT32_MAX

/*
 * One field. The application's row struct starts with one of these; see the header.
 *
 * Positional initialisation is how a table of two hundred of these gets written, so the order of
 * the members is part of the interface: a member added in the middle renumbers every row after
 * it. `note` is last for that reason, and every row states it, INKCELL_STR_NONE included - which
 * is what turns a renumbering into a type error somewhere rather than a silence everywhere.
 */
struct mesh_ui_form_field {
    inkcell_str_id label;
    enum mesh_ui_setting_kind kind;
    /* The application's section id. The form compares it and never interprets it. */
    uint16_t section;
    /*
     * What values this field will take, which is a different number for each kind that has an
     * opinion: TEXT and KEY the byte cap, ENUM how many values there are, FLAG *which bit* of its
     * group's word the row is.
     *
     * A mask is not a limit in English, but it is the same thing to a table that is "what this
     * field's values are", and the alternative is another member every row would state for the
     * sake of the few that mean anything by it.
     */
    uint32_t limit;
    /* ENUM: the name of each value. */
    const char *(*enum_name)(uint32_t value);
    /* NUMBER: the values it steps through, and whether they are a scale. */
    struct inkstand_form_presets presets;
    /* KEY: a bitmask of the choices Left and Right walk. */
    uint32_t choices;
    /* What this field does, for a help screen, or INKCELL_STR_NONE for a row whose label is
       already the whole explanation. */
    inkcell_str_id note;
};

/*
 * A form: the application's table, read through a stride.
 *
 * `fields` is `count` rows of `stride` bytes each, and each row begins with a
 * struct mesh_ui_form_field. Row 0 is no field.
 */
struct mesh_ui_form {
    const void *fields;
    size_t stride;
    uint16_t count;
};

/* The row for `id`, never NULL: an id at or past `count` is row 0. */
const struct mesh_ui_form_field *mesh_ui_form_field(const struct mesh_ui_form *form, uint16_t id);

/* Whether any row but row 0 sits in `section`. A walk rather than a column, so a section whose
   last row is retired stops answering true without anybody remembering to say so. */
bool mesh_ui_form_section_has_fields(const struct mesh_ui_form *form, uint16_t section);

/* FLAG: which bit of its group's word the row is, and 0 for every other kind. */
uint32_t mesh_ui_form_bit(const struct mesh_ui_form *form, uint16_t id);
/* ENUM: how many values, and 0 for every other kind. */
uint32_t mesh_ui_form_enum_count(const struct mesh_ui_form *form, uint16_t id);
/* ENUM: the name of `value`, or inkcell's word for "unknown" for any other kind or a row with no
   names. Never NULL. */
const char *mesh_ui_form_enum_name(const struct mesh_ui_form *form, uint16_t id, uint32_t value);
/* TEXT and KEY: the longest value in bytes, without the NUL; 0 for every other kind. */
uint32_t mesh_ui_form_text_max(const struct mesh_ui_form *form, uint16_t id);
/* KEY: the choices Left and Right walk; 0 for every other kind. */
uint32_t mesh_ui_form_key_choices(const struct mesh_ui_form *form, uint16_t id);
/* NUMBER: inkstand_form_presets_step() over the row's presets, and `value` for any other kind. */
uint32_t mesh_ui_form_number_step(const struct mesh_ui_form *form, uint16_t id, uint32_t value,
                                  int delta);
/* NUMBER: inkstand_form_presets_track() over the row's presets, and false for any other kind. */
bool mesh_ui_form_number_track(const struct mesh_ui_form *form, uint16_t id, uint32_t value,
                               struct inkstand_form_track *out);

#ifdef __cplusplus
}
#endif

#endif
