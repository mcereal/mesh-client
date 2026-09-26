#define _POSIX_C_SOURCE 200809L

/*
 * The rows a settings screen draws.
 *
 * Every section is built the same way: an item_list is filled by a build_* function, with the
 * pending edits layered over the radio's own values so a row shows what the user has typed
 * rather than what the radio last said. Rows are rebuilt on every draw rather than cached,
 * which is what keeps "the radio just told us something new" from needing an invalidation path.
 */

#include "inkwell/base/array.h"
#include "inkwell/base/text.h"

#include "settings_internal.h"

#include "mesh/core/radio_settings.h"
/* store_forward.h for the request-state enum, on node_detail.c's terms: the UI struct carries
   it as a byte so store.h stays nanopb-free, and this file already pulls nanopb in through
   radio_settings.h - so naming the real enum here beats keeping a second copy of it in step. */
#include "mesh/core/firmware.h"
/* For the install's state ladder and the two functions that name it. Four symbols out of a
   header that also declares the job's own storage, which is a wide door for what is read here -
   but it is the same door mesh/core/firmware.h next to it already opens, and the alternative is
   the UI keeping its own copy of an enum whose whole purpose is that there is one of it. */
#include "mesh/core/firmware_update.h"
#include "mesh/core/store_forward.h"
#include "mesh/core/updater.h"
#include "mesh/core/version.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/units.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* "oKGio6Sl... (AES-128)", "default key", "no encryption"; `aes` names the size the way the
   channel list does, else it is plain bits. */
static void key_summary(const uint8_t *key, size_t len, bool aes, char *out, size_t out_len) {
    if (len == 0U) {
        snprintf(
            out, out_len, "%s",
            inkcell_str(aes ? MESH_STR_SETTINGS_KEY_NO_ENCRYPTION : MESH_STR_SETTINGS_KEY_NONE));
        return;
    }
    if (len == 1U) {
        if (key[0] == 1U) {
            snprintf(out, out_len, "%s", inkcell_str(MESH_STR_SETTINGS_KEY_DEFAULT));
        } else {
            inkcell_str_format(out, out_len, MESH_STR_SETTINGS_KEY_SIMPLE, (unsigned)key[0]);
        }
        return;
    }
    char text[48];
    mesh_ui_settings_key_text(key, len, text, sizeof text);
    inkcell_str_format(out, out_len, aes ? MESH_STR_SETTINGS_KEY_AES : MESH_STR_SETTINGS_KEY_BITS,
                       text, (unsigned)(len * 8U));
}

/* ---- item builders ------------------------------------------------------------------------ */

struct item_list {
    struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
    uint32_t count;
    const struct mesh_ui_setting_edit *edits;
    size_t edit_count;
    /*
     * Which system of units every length in this section reads in - the radio's own
     * DisplayConfig.units, taken once for the whole section rather than asked per row.
     *
     * It follows a pending edit of the Units row like any other value, which is what makes the
     * Display section show its own change before it is written: stepping to Imperial rewords the
     * rows under it immediately, and backing out of the section rewords them back.
     */
    bool imperial;
};

/*
 * Rows name their label with a catalog id; item_add_named() is the exception, for the two
 * labels that come off the wire rather than out of the catalog - a channel's own name and a
 * module's section title.
 */
static struct mesh_ui_settings_item *item_add_named(struct item_list *list, const char *label,
                                                    enum inkstand_form_kind kind) {
    if (list->count >= MESH_UI_SETTINGS_ITEMS_MAX) {
        return NULL;
    }
    struct mesh_ui_settings_item *item = &list->items[list->count++];
    memset(item, 0, sizeof *item);
    snprintf(item->label, sizeof item->label, "%s", label);
    item->kind = kind;
    return item;
}

static struct mesh_ui_settings_item *item_add(struct item_list *list, inkcell_str_id label,
                                              enum inkstand_form_kind kind) {
    return item_add_named(list, inkcell_str(label), kind);
}

static void item_text(struct item_list *list, inkcell_str_id label, enum inkstand_form_kind kind,
                      const char *value) {
    struct mesh_ui_settings_item *item = item_add(list, label, kind);
    if (item != NULL) {
        inkwell_str_copy(item->value, sizeof item->value, value);
    }
}

/* The common case: both halves of the row are catalog entries. */
static void item_str(struct item_list *list, inkcell_str_id label, enum inkstand_form_kind kind,
                     inkcell_str_id value) {
    item_text(list, label, kind, inkcell_str(value));
}

/* A toggle the radio reports and nobody here can change. `number` is set for the same reason
   an editable toggle sets it: it is what a renderer drawing a switch rather than the words
   reads, and a read-only row that left it at 0 would draw every such row off. */
static void item_toggle(struct item_list *list, inkcell_str_id label, bool value) {
    struct mesh_ui_settings_item *item = item_add(list, label, INKSTAND_FORM_TOGGLE);
    if (item == NULL) {
        return;
    }
    inkwell_str_copy(item->value, sizeof item->value,
                     inkcell_str(value ? MESH_STR_COMMON_ON : MESH_STR_COMMON_OFF));
    item->number = value ? 1U : 0U;
}

/*
 * A read-only quantity with a level: `permille` is 0..1000, or INKSTAND_FORM_METER_UNKNOWN for work
 * whose extent nobody can know.
 *
 * The words go in the value column as they would on any other read-only row, so a backend that
 * draws no bar shows a complete fact rather than a blank - see INKSTAND_FORM_METER.
 */
static void item_meter(struct item_list *list, inkcell_str_id label, const char *value,
                       uint32_t permille) {
    struct mesh_ui_settings_item *item = item_add(list, label, INKSTAND_FORM_METER);
    if (item == NULL) {
        return;
    }
    inkwell_str_copy(item->value, sizeof item->value, value);
    item->number = permille;
}

/*
 * A group title. No value, no field, nothing happens when A lands on it. Headings are emitted
 * unconditionally - never behind the group's own Enabled toggle - so an edit can never change
 * the row count under the cursor.
 *
 * **Every heading in this tab is this one**, and that is the rule rather than what the callers
 * happen to do. There was a second builder that hung a symbol on the heading, drawn by the fb
 * backend as a card *header* - the disc out at the card's own edge, where a plain heading's
 * words begin two cells further in, past the leading slot every row of an open section reserves.
 * Eight headings took it and thirty did not, and five of the eight were Radio actions, so that
 * one section announced its groups in a shape no other section used: walking LoRa -> Radio
 * actions moved the heading sideways and gave it a disc for a reason nothing on either screen
 * showed. The node detail and Status keep their card headers, and they are card screens; a
 * settings section is a list of fields, and a list has one kind of subheader.
 *
 * Optional-per-heading was what made the split invisible to write and visible to read: a group
 * whose subject has no honest symbol - "Sent with a position" is a sentence about ten bits - had
 * to say nothing, so the sections divided into the ones whose subjects happened to own a rune
 * and the ones that did not. `ui_settings_row_icons_are_all_or_nothing` holds it.
 */
static void item_heading(struct item_list *list, inkcell_str_id label) {
    item_add(list, label, INKSTAND_FORM_HEADING);
}

/* "30s", "5m", "2h"; `zero` says what 0 means for this field ("off", "default"). */
static void format_seconds(char *out, size_t out_len, uint32_t seconds, inkcell_str_id zero) {
    if (seconds == 0U) {
        snprintf(out, out_len, "%s", inkcell_str(zero));
    } else if (seconds % 3600U == 0U) {
        inkcell_str_format(out, out_len, MESH_STR_VALUE_HOURS, (unsigned)(seconds / 3600U));
    } else if (seconds % 60U == 0U) {
        inkcell_str_format(out, out_len, MESH_STR_VALUE_MINUTES, (unsigned)(seconds / 60U));
    } else {
        inkcell_str_format(out, out_len, MESH_STR_VALUE_SECONDS, (unsigned)seconds);
    }
}

/*
 * TEXT fields holding a credential. The row shows a fixed-width mask rather than the value -
 * a settings screen on a handheld is read over your shoulder, and the section is opened to
 * change one of the other rows far more often than to look at this one. The mask is a fixed
 * length so it does not leak how long the secret is.
 *
 * The keyboard still opens on the real text, exactly as the KEY rows do: it is the one place
 * a secret is revealed, and a credential you cannot see is a credential you cannot correct a
 * typo in. A predicate rather than another column in k_fields, the same way
 * mesh_ui_settings_section_needs_confirm() is a predicate.
 */
static bool field_is_secret(enum mesh_ui_setting_field field) {
    return field == MESH_UI_FIELD_MQTT_PASSWORD;
}

/*
 * The unit a typed number is in, for the value column only.
 *
 * A predicate beside field_is_secret() rather than a column in k_fields, and for the same
 * reason: it changes how the row is *drawn* and nothing about what the field is. The keyboard
 * still opens on `item->text`, which stays the bare number the parser wants - a row that
 * offered "906.8750 MHz" back to be edited would be a row you have to delete four characters
 * from before you can type.
 *
 * Only where two neighbouring rows would otherwise be indistinguishable. LoRa's override
 * frequency is megahertz and the trim below it is hertz, and "906.8750" over "-12.5" says
 * nothing about which is which; a latitude has no such neighbour and stays bare.
 */
static inkcell_str_id field_unit(enum mesh_ui_setting_field field) {
    switch (field) {
    case MESH_UI_FIELD_LORA_OVERRIDE_FREQ:
    case MESH_UI_FIELD_LORA_HAM_FREQUENCY:
        return MESH_STR_VALUE_MEGAHERTZ;
    case MESH_UI_FIELD_LORA_FREQUENCY_TRIM:
        return MESH_STR_VALUE_HERTZ;
    default:
        return INKCELL_STR_NONE;
    }
}

/*
 * An editable row: the field's spec supplies label and kind; a pending edit replaces the
 * radio's value and marks the row dirty. `text` is only read for TEXT fields.
 *
 * Answers the row it added, or NULL when the section is full, so a caller with something to say
 * about a row that the field table cannot say for it - which set of values this one will take
 * today, and whether the value it is showing disagrees with another row's - says it here rather
 * than through a second entry point. Every other caller ignores the answer, which is the whole
 * of what it costs them.
 */
static struct mesh_ui_settings_item *item_field(struct item_list *list,
                                                enum mesh_ui_setting_field field, uint32_t number,
                                                const char *text) {
    const struct field_spec *spec = field_spec(field);
    struct mesh_ui_settings_item *item = item_add(list, spec->form.label, spec->form.kind);
    if (item == NULL) {
        return NULL;
    }
    item->field = field;
    const struct mesh_ui_setting_edit *edit =
        mesh_ui_settings_find_edit(list->edits, list->edit_count, field);
    if (edit != NULL) {
        item->dirty = true;
        number = edit->number;
        text = edit->text;
    }
    item->number = number;
    switch (spec->form.kind) {
    /* A flag says "on" and "off" in the value column exactly as a toggle does. The two differ
       in the control drawn beside the words, which is the backend's choice to make. */
    case INKSTAND_FORM_TOGGLE:
    case INKSTAND_FORM_FLAG:
        snprintf(item->value, sizeof item->value, "%s",
                 inkcell_str(number != 0U ? MESH_STR_COMMON_ON : MESH_STR_COMMON_OFF));
        break;
    case INKSTAND_FORM_ENUM:
        snprintf(item->value, sizeof item->value, "%s", mesh_ui_settings_enum_name(field, number));
        break;
    case INKSTAND_FORM_NUMBER:
        /* A zero_label, where the field sets one, wins over the formatter: it is the field
           table's way of saying what 0 means for this row, and a formatter that also had an
           opinion silently overrode it - three Traffic management rows read "default" under a
           section whose whole convention is that 0 is off. */
        if (number == 0U && spec->zero_label != INKCELL_STR_NONE) {
            snprintf(item->value, sizeof item->value, "%s", inkcell_str(spec->zero_label));
        } else if (spec->format != NULL) {
            spec->format(number, list->imperial, item->value, sizeof item->value);
        } else {
            format_seconds(item->value, sizeof item->value, number,
                           spec->zero_label != INKCELL_STR_NONE ? spec->zero_label
                                                                : MESH_STR_VALUE_ZERO);
        }
        break;
    case INKSTAND_FORM_TEXT:
        snprintf(item->text, sizeof item->text, "%s", text != NULL ? text : "");
        if (item->text[0] == '\0') {
            snprintf(item->value, sizeof item->value, "%s",
                     inkcell_str(MESH_STR_SETTINGS_TEXT_EMPTY));
        } else if (field_is_secret(field)) {
            snprintf(item->value, sizeof item->value, "%s",
                     inkcell_str(MESH_STR_SETTINGS_TEXT_SECRET));
        } else if (field_unit(field) != INKCELL_STR_NONE) {
            inkcell_str_format(item->value, sizeof item->value, field_unit(field), item->text);
        } else {
            inkwell_str_copy(item->value, sizeof item->value, item->text);
        }
        break;
    default:
        break;
    }
    return item;
}

/*
 * A group of FLAG rows: one word of the radio's, drawn as the set of bits it is.
 *
 * The masks are not written here. Each row asks the field table for its own bit, so the run
 * below is "every flag of this group, in the order the enum declares them" and adding a bit is
 * a row in that table rather than a line here. Every one of them is listed whatever the others
 * say - the rule the LoRa trio follows, so an edit cannot move the row count under the cursor.
 */
static void item_flag_group(struct item_list *list, enum mesh_ui_setting_field_group group,
                            uint32_t word) {
    const uint32_t count = mesh_ui_settings_group_count(group);
    for (uint32_t i = 0; i < count; ++i) {
        const enum mesh_ui_setting_field field = mesh_ui_settings_group_field(group, i);
        const uint32_t bit = mesh_ui_settings_field_bit(field);
        item_field(list, field, (word & bit) != 0U ? 1U : 0U, NULL);
    }
}

/*
 * A group of repeated *records*: the same run of rows, once per entry, each under its own title.
 *
 * The flag group above walks one word and this walks four records, and they are the same walk -
 * a group is a contiguous run of fields repeating one shape, which a set of bits was only the
 * first thing to be. What a record group has that a flag group does not is a record length, so
 * the run can be cut, and a numbered heading, so a reader can tell the third copy from the
 * fourth.
 *
 * `values` is the group's run flattened in the field enum's own order, which is what keeps this
 * free of any opinion about what a record holds: the caller reads its own struct and this
 * builds rows. Every record is listed whether or not the radio sent one - an empty one is how a
 * target is added, exactly as an empty slot is how a channel is - so the count never moves,
 * which is the rule every group in this file follows.
 *
 * `rows` is the same run of pointers back, for the caller with something to say about a row
 * that this cannot say for it - which one field of a record constrains another. It is the same
 * bargain item_field() itself makes by answering the row it added: the shape is built here and
 * the meaning stays with the caller. NULL for a caller that has nothing to add.
 */
static void item_record_group(struct item_list *list, enum mesh_ui_setting_field_group group,
                              uint32_t fields_per_record, inkcell_str_id title,
                              const uint32_t *values, size_t value_count,
                              struct mesh_ui_settings_item **rows) {
    const uint32_t count = mesh_ui_settings_group_count(group);
    if (fields_per_record == 0U || values == NULL || value_count < count) {
        return;
    }
    char label[MESH_UI_SETTINGS_LABEL_MAX];
    for (uint32_t i = 0; i < count; ++i) {
        if (i % fields_per_record == 0U) {
            inkcell_str_format(label, sizeof label, title, (unsigned)(i / fields_per_record + 1U));
            item_add_named(list, label, INKSTAND_FORM_HEADING);
        }
        struct mesh_ui_settings_item *row =
            item_field(list, mesh_ui_settings_group_field(group, i), values[i], NULL);
        if (rows != NULL) {
            rows[i] = row;
        }
    }
}

/* A KEY row. `key`/`len` is the radio's current key; an edit is a choice, or typed text. The
   text carried is what the keyboard should open on: the typed text if there is one, else the
   current key as base64 (an explicit reveal, never shown in the row). */
static void item_key_field(struct item_list *list, enum mesh_ui_setting_field field,
                           const uint8_t *key, size_t len) {
    const struct field_spec *spec = field_spec(field);
    /* A channel's PSK, wherever the submessage holding it happens to be embedded: the beacon's
       offered channel is a ChannelSettings and its key is named in the same words. */
    const bool aes =
        (field == MESH_UI_FIELD_CHANNEL_KEY || field == MESH_UI_FIELD_BEACON_OFFER_KEY);
    struct mesh_ui_settings_item *item = item_add(list, spec->form.label, spec->form.kind);
    if (item == NULL) {
        return;
    }
    item->field = field;
    /* A KEY row's set is the field table's and never moves; it is on the row for the same
       reason an enum's is, which is that Left and Right read the row rather than the table. */
    item->choices = spec->form.choices;
    mesh_ui_settings_key_text(key, len, item->text, sizeof item->text);
    const struct mesh_ui_setting_edit *edit =
        mesh_ui_settings_find_edit(list->edits, list->edit_count, field);
    item->number = edit != NULL ? edit->number : (uint32_t)MESH_UI_PSK_KEEP;
    item->dirty = edit != NULL;
    switch ((enum mesh_ui_psk_choice)item->number) {
    case MESH_UI_PSK_DEFAULT:
        snprintf(item->value, sizeof item->value, "%s", inkcell_str(MESH_STR_SETTINGS_KEY_DEFAULT));
        break;
    case MESH_UI_PSK_RANDOM_128:
        snprintf(item->value, sizeof item->value, "%s",
                 inkcell_str(MESH_STR_SETTINGS_KEY_NEW_AES128));
        break;
    case MESH_UI_PSK_RANDOM_256:
        snprintf(
            item->value, sizeof item->value, "%s",
            inkcell_str(aes ? MESH_STR_SETTINGS_KEY_NEW_AES256 : MESH_STR_SETTINGS_KEY_NEW_KEY));
        break;
    case MESH_UI_PSK_NONE:
        snprintf(
            item->value, sizeof item->value, "%s",
            inkcell_str(aes ? MESH_STR_SETTINGS_KEY_NO_ENCRYPTION : MESH_STR_SETTINGS_KEY_CLEAR));
        break;
    case MESH_UI_PSK_TYPED: {
        uint8_t typed[MESH_UI_PSK_MAX];
        size_t typed_len = 0U;
        snprintf(item->text, sizeof item->text, "%s", edit->text);
        if (mesh_ui_settings_key_parse(edit->text, typed, sizeof typed, &typed_len)) {
            key_summary(typed, typed_len, aes, item->value, sizeof item->value);
        } else {
            snprintf(item->value, sizeof item->value, "%s",
                     inkcell_str(MESH_STR_SETTINGS_KEY_INVALID));
        }
        break;
    }
    case MESH_UI_PSK_KEEP:
    default:
        key_summary(key, len, aes, item->value, sizeof item->value);
        break;
    }
}

/* Keys are shown as a short fingerprint: enough to compare against the phone app's view,
   not enough to leak the key to someone reading over your shoulder. */
static void item_key(struct item_list *list, inkcell_str_id label, const uint8_t *key, size_t len) {
    struct mesh_ui_settings_item *item = item_add(list, label, INKSTAND_FORM_KEY);
    if (item == NULL) {
        return;
    }
    if (len == 0U) {
        snprintf(item->value, sizeof item->value, "%s", inkcell_str(MESH_STR_SETTINGS_KEY_NONE));
        return;
    }
    size_t shown = len < 4U ? len : 4U;
    char hex[9] = {0};
    for (size_t i = 0; i < shown; ++i) {
        snprintf(hex + 2U * i, sizeof hex - 2U * i, "%02x", key[i]);
    }
    inkcell_str_format(item->value, sizeof item->value, MESH_STR_SETTINGS_KEY_FINGERPRINT, hex,
                       (unsigned)len);
}

/*
 * An ACTION row: activated with A, carrying what it does in `number` so the nav can raise the
 * action without knowing about updates.
 *
 * The symbol and the weight come off the verb rather than off the call site, which is what
 * makes this one function the whole of "a settings row is a verb": every action row in every
 * section gets the leading disc and the tone the two tables in settings.c answer with, and a
 * new verb is a row in those tables rather than a change here or in a renderer.
 *
 * `value` is empty on almost all of them and that is the change this shape is really about.
 * Every verb here used to carry "press A" in its value column, so Radio actions was eight rows
 * of one instruction with the labels - the only part that differs - read past it. It said
 * nothing the frame was not already saying: the action bar names A three rows below, and it
 * names it for this screen rather than once per row. What is left in the column is the handful
 * of rows with a real value to state, which is what a value column is for: the count a forget
 * would remove.
 *
 * No backend loses by it. The CLI backend draws the Messages, Nodes and Status screens and has
 * never drawn a settings section at all, so the instruction was only ever reaching the one
 * renderer that also draws the keycap.
 */
static void item_action_named(struct item_list *list, const char *label, const char *value,
                              enum mesh_ui_settings_action action) {
    struct mesh_ui_settings_item *item = item_add_named(list, label, INKSTAND_FORM_ACTION);
    if (item == NULL) {
        return;
    }
    inkwell_str_copy(item->value, sizeof item->value, value);
    item->number = (uint32_t)action;
    item->tone = mesh_ui_settings_action_tone(action);
    /*
     * A press that steps this row's own value is not a verb, and the whole of what that costs
     * is here: no disc, and the row keeps its place among the fields instead of being floated
     * off the card with the presses. The nav is untouched - A still reaches the same action -
     * because what changed is what the row *is*, not what the press does.
     */
    if (mesh_ui_settings_action_is_cycle(action)) {
        item->cycle = true;
        return;
    }
    item->icon = mesh_ui_settings_action_icon(action);
    item->verb = true;
}

/*
 * The same verb, withdrawn, and why - the shape INKSTAND_FORM_ACTION_OFF is for.
 *
 * It keeps the verb's symbol and loses its weight: a row that cannot be pressed is not a
 * dangerous row, it is an absent one, so the disc goes quiet rather than staying red. What it
 * does *not* lose is its place in the column, which is the whole point - the section keeps its
 * length and its shape whatever the transport is doing, and the cursor does not move under
 * somebody because a link dropped.
 */
static void item_action_off_named(struct item_list *list, const char *label, const char *reason,
                                  enum mesh_ui_settings_action action) {
    struct mesh_ui_settings_item *item = item_add_named(list, label, INKSTAND_FORM_ACTION_OFF);
    if (item != NULL) {
        inkwell_str_copy(item->value, sizeof item->value, reason);
        item->number = (uint32_t)action;
        item->icon = mesh_ui_settings_action_icon(action);
        item->tone = INKCELL_TONE_DIM;
        item->verb = true;
    }
}

static void item_action_off(struct item_list *list, inkcell_str_id label, inkcell_str_id reason,
                            enum mesh_ui_settings_action action) {
    item_action_off_named(list, inkcell_str(label), inkcell_str(reason), action);
}

static void item_action(struct item_list *list, inkcell_str_id label, const char *value,
                        enum mesh_ui_settings_action action) {
    item_action_named(list, inkcell_str(label), value, action);
}

/*
 * The common shape: a verb with nothing to state.
 *
 * Named rather than spelled as an empty value at a dozen call sites, because the empty string is
 * the *point* here - it is what replaced "press A" - and a column of calls passing "" reads as a
 * value somebody forgot to fill in rather than as a row that has none.
 */
static void item_verb(struct item_list *list, inkcell_str_id label,
                      enum mesh_ui_settings_action action) {
    item_action_named(list, inkcell_str(label), "", action);
}

static void item_verb_named(struct item_list *list, const char *label,
                            enum mesh_ui_settings_action action) {
    item_action_named(list, label, "", action);
}

/* One of the two rows that drop cached nodes. The value column is the count the press would
   remove, so a row with nothing to remove is a fact rather than a press that does nothing -
   and the two can never disagree, because the count came through the same predicate the
   forget itself uses. */
static void forget_row(struct item_list *list, inkcell_str_id label, uint32_t forgettable,
                       enum mesh_ui_settings_action action) {
    if (forgettable == 0U) {
        item_action_off(list, label, MESH_STR_ACTION_NOTHING_TO_DROP, action);
        return;
    }
    char value[MESH_UI_SETTINGS_VALUE_MAX];
    inkcell_str_format_plural(value, sizeof value, MESH_STR_ACTION_FORGET_COUNT_ONE, forgettable,
                              forgettable);
    item_action(list, label, value, action);
}

/* An action the radio has to be reachable for. Without a link it becomes the same row saying
   why, so the section keeps its shape whatever the transport is doing. */
static void item_radio_action(struct item_list *list, inkcell_str_id label,
                              enum mesh_ui_settings_action action, bool connected) {
    if (connected) {
        item_verb(list, label, action);
    } else {
        item_action_off(list, label, MESH_STR_SETTINGS_NOT_CONNECTED, action);
    }
}

/*
 * About: what this client is, and the self-update rows. The only section that renders with no
 * radio connected, and the only one whose values come from the app rather than the air.
 *
 * Every row that responds to A carries a verb in its value column, not a bare "A". The button
 * hint on its own read as data - "Check for updates > A" looks like a setting whose value is
 * the letter A - and left no clue that anything would happen.
 *
 * The update rows are deliberately a check and a separate install rather than one button. The
 * install downloads and replaces the running binary, so it is worth a second, deliberate press
 * once the user can see which version they are about to move to.
 */
static void build_about(const struct mesh_ui_settings *s, struct item_list *list) {
    const struct mesh_ui_client_info *client = &s->client;
    item_text(list, MESH_STR_ABOUT_VERSION, INKSTAND_FORM_INFO,
              client->version[0] != '\0' ? client->version
                                         : inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT));
    if (client->backend[0] != '\0') {
        item_text(list, MESH_STR_ABOUT_UI_BACKEND, INKSTAND_FORM_INFO, client->backend);
    }
    if (client->data_dir[0] != '\0') {
        item_text(list, MESH_STR_ABOUT_DATA, INKSTAND_FORM_INFO, client->data_dir);
    }

    /*
     * The crash report, when a previous run left one.
     *
     * High up on purpose. Every row below this is something somebody came here to read at their
     * leisure; this one is the reason the banner sent them, and About is a screen that runs off
     * the bottom of the panel on a device this size. It also sits above the three early returns
     * further down - a row placed after those would be missing on exactly the builds where the
     * updater is unavailable, which is every hand-deployed one.
     *
     * Two rows because they do two things: the path is for copying off the screen, the verb is
     * what makes the banner resolvable. The path is shown as a plain fact even when there is no
     * report, so the question "where would it go" has an answer before there is anything to
     * find - the same reason the data directory above is always drawn.
     */
    if (client->crash_report_waiting) {
        /*
         * The file's name, not its path.
         *
         * The value column is about two dozen cells and an absolute path on the Brick is three
         * times that, so the row drew "/mnt/SDCARD/.userdata/tg50" - a location cut off exactly
         * where it stops being a location, which is worse than not saying it. It is the firmware
         * rows' rule ("short values, never sentences") reached from the other direction.
         *
         * What makes the short answer sufficient is the Data row directly above, which already
         * points at the directory. That row is clipped too - it is shown for orientation and has
         * always accepted that - but a *filename* is short enough to be whole at any scale, and
         * "it is called crash.txt and it is in the data directory" is what somebody needs in
         * order to go and find it. Two clipped halves of one path would have been neither.
         *
         * Derived from the published path rather than printed as a constant, so a report that
         * moved would take the row with it.
         */
        const char *name = client->crash_report_path;
        const char *slash = strrchr(client->crash_report_path, '/');
        if (slash != NULL && slash[1] != '\0') {
            name = slash + 1;
        }
        item_text(list, MESH_STR_ABOUT_CRASH_REPORT, INKSTAND_FORM_INFO, name);
        item_verb(list, MESH_STR_ABOUT_CRASH_DISCARD, MESH_UI_SETTINGS_ACTION_DISCARD_CRASH_REPORT);
    }
    /* Keep each language's own name visible so users can always find their way back. */
    if (client->language_name[0] != '\0') {
        if (client->language_from_env) {
            item_text(list, MESH_STR_ABOUT_LANGUAGE_ENV, INKSTAND_FORM_INFO, client->language_name);
        } else {
            item_action(list, MESH_STR_ABOUT_LANGUAGE, client->language_name,
                        MESH_UI_SETTINGS_ACTION_CYCLE_LANGUAGE);
        }
    }

    /*
     * The look. Above the update rows on purpose: those return early in three places - no
     * updater, a check in flight, an install ready - and a row placed after them would
     * disappear exactly when somebody sat in the sun wanted it.
     *
     * Cycling rather than a submenu because the screen is its own preview: pressing A steps to
     * the next theme and the frame it draws is the answer. Held as a fact when the environment
     * named one, the same way the dev-updates switch is.
     */
    if (client->theme_name[0] != '\0' || client->theme[0] != '\0') {
        const char *const name = client->theme_name[0] != '\0' ? client->theme_name : client->theme;
        if (client->theme_from_env) {
            /* The note goes in the label, not the value. The value column is about eighteen
               cells at the device scale, so "High contrast (environment)" clipped to "High
               contrast (env" - a note that reads as a bug. The label column has room for the
               note whatever the theme is called, and the value stays the plain name. */
            item_text(list, MESH_STR_ABOUT_THEME_ENV, INKSTAND_FORM_INFO, name);
        } else {
            item_action(list, MESH_STR_ABOUT_THEME, name, MESH_UI_SETTINGS_ACTION_CYCLE_THEME);
        }
        /* Beside the theme and on its terms: the screen is its own preview. */
        const char *const size = inkcell_str(mesh_ui_text_size_name(client->text_size));
        if (client->text_size_from_env) {
            item_text(list, MESH_STR_ABOUT_TEXT_SIZE_ENV, INKSTAND_FORM_INFO, size);
        } else {
            item_action(list, MESH_STR_ABOUT_TEXT_SIZE, size,
                        MESH_UI_SETTINGS_ACTION_CYCLE_TEXT_SIZE);
        }
    }

    if (!client->update_supported) {
        item_text(list, MESH_STR_ABOUT_UPDATES, INKSTAND_FORM_INFO,
                  client->update_message[0] != '\0'
                      ? client->update_message
                      : inkcell_str(MESH_STR_ABOUT_UPDATES_UNAVAILABLE));
        return;
    }

    /*
     * The channel is a setting, so its value column is the setting rather than a verb; that it
     * responds to A is what the marker says. It comes before the status because it decides
     * which question a check will ask.
     *
     * While a child is running it drops to a plain fact: switching channel mid-download would
     * pull the asset out from under it, so the updater refuses, and a row that refuses is
     * worse than one that never invited the press.
     */
    const char *const channel = client->update_channel[0] != '\0'
                                    ? client->update_channel
                                    : inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT);
    if (client->update_busy) {
        item_text(list, MESH_STR_ABOUT_UPDATE_CHANNEL, INKSTAND_FORM_INFO, channel);
    } else {
        item_action(list, MESH_STR_ABOUT_UPDATE_CHANNEL, channel,
                    MESH_UI_SETTINGS_ACTION_CYCLE_UPDATE_CHANNEL);
    }

    /*
     * The dev-updates switch, on a build that is not a release. It exists because the guard it
     * lifts is the only thing standing between a hand-deployed build and the install path, and
     * the alternative way in - an environment variable - needs a computer and an ssh session,
     * which is exactly what a handheld does not have. A release build never sees this row:
     * there is no guard on it to lift.
     */
    if (!client->update_is_release) {
        if (client->update_allow_dev_from_env) {
            /* Held on by MESHCLIENT_UPDATE_ALLOW_DEV. Shown as a fact rather than a switch,
               because a toggle that sprang back would look broken. */
            item_str(list, MESH_STR_ABOUT_DEV_UPDATES, INKSTAND_FORM_INFO,
                     MESH_STR_ABOUT_DEV_UPDATES_ENV);
        } else if (client->update_busy) {
            item_str(list, MESH_STR_ABOUT_DEV_UPDATES, INKSTAND_FORM_INFO,
                     client->update_allow_dev ? MESH_STR_COMMON_ON : MESH_STR_COMMON_OFF);
        } else {
            item_action(
                list, MESH_STR_ABOUT_DEV_UPDATES,
                inkcell_str(client->update_allow_dev ? MESH_STR_COMMON_ON : MESH_STR_COMMON_OFF),
                MESH_UI_SETTINGS_ACTION_TOGGLE_DEV_UPDATES);
        }
    }

    const enum mesh_update_state state = (enum mesh_update_state)client->update_state;
    item_text(list, MESH_STR_ABOUT_UPDATE_STATUS, INKSTAND_FORM_INFO,
              client->update_message[0] != '\0' ? client->update_message
                                                : mesh_update_state_name(state));

    /*
     * While a child is running neither update row does anything, so both say so rather than
     * inviting a press that would be swallowed.
     *
     * It is a meter rather than a fact because "downloading..." answers whether something is
     * happening and nothing else - and the question anybody watching a four-megabyte download
     * over a handheld's wifi actually has is whether it is *still* happening. A bar answers
     * that without being read. Which of the two bars it is comes from whether the step has a
     * fraction at all: the download does, once GitHub has told us the asset's size; the check
     * is one request whose reply has no length until it arrives.
     */
    if (client->update_busy) {
        char working[MESH_UI_SETTINGS_VALUE_MAX];
        uint32_t level = INKSTAND_FORM_METER_UNKNOWN;
        if (client->update_progress_known) {
            level = client->update_progress;
            inkcell_str_format(working, sizeof working, MESH_STR_ABOUT_WORKING_PERCENT,
                               (unsigned)(level / 10U));
        } else {
            inkwell_str_copy(working, sizeof working,
                             inkcell_str(state == MESH_UPDATE_DOWNLOADING
                                             ? MESH_STR_ABOUT_WORKING_DOWNLOAD
                                             : MESH_STR_ABOUT_WORKING_CHECK));
        }
        item_meter(list, MESH_STR_ABOUT_WORKING, working, level);
        return;
    }
    if (state == MESH_UPDATE_READY) {
        item_str(list, MESH_STR_ABOUT_INSTALLED, INKSTAND_FORM_INFO,
                 MESH_STR_ABOUT_INSTALLED_RELAUNCH);
        return;
    }

    item_verb(list, MESH_STR_ABOUT_CHECK_UPDATES, MESH_UI_SETTINGS_ACTION_CHECK_UPDATE);
    if (state == MESH_UPDATE_AVAILABLE) {
        /* The version goes in the label so the value column can say how to act on it: the row
           the user has to find is the one that names what it will install. The label is
           bounded by its own column, not by what the release named itself. */
        char label[MESH_UI_SETTINGS_LABEL_MAX];
        inkcell_str_format(label, sizeof label, MESH_STR_ABOUT_INSTALL_VERSION,
                           (int)(sizeof label - 9U), client->update_latest);
        item_verb_named(list, label, MESH_UI_SETTINGS_ACTION_INSTALL_UPDATE);
    } else if (!client->update_can_install) {
        /* Nothing here will offer an install, so say so once - and name the row that changes
           it, rather than leaving the user hunting for one that is never coming. */
        item_str(list, MESH_STR_ABOUT_INSTALLING, INKSTAND_FORM_INFO, MESH_STR_ABOUT_TURN_ON_DEV);
    }
}

/*
 * About radio: what the radio *is*, as About MeshClient is what this client is. Every row is
 * read-only, and that is the whole distinction the two names carry - a row that can be changed
 * lives in the section that owns it, never here.
 *
 * Which is why the LoRa region is not on this screen although it is the one radio fact people
 * look for. It is editable, so it belongs to LoRa, and a read-only copy here is a row that
 * answers "what region is this radio on" in the one place that cannot answer "change it".
 */
/* "192.168.1.40". The wire carries the address as a fixed32 in network byte order, which is
   what the firmware puts on it; the octets are read out of it rather than through htonl so the
   row reads the same on either endianness. */
static void format_ipv4(uint32_t address, char *out, size_t out_len) {
    inkcell_str_format(out, out_len, MESH_STR_VALUE_IPV4, (unsigned)(address & 0xFFU),
                       (unsigned)((address >> 8) & 0xFFU), (unsigned)((address >> 16) & 0xFFU),
                       (unsigned)((address >> 24) & 0xFFU));
}

/*
 * What the radio's own interfaces are doing, under a heading each.
 *
 * Only the interfaces the radio reported are drawn: a board with no WiFi says nothing about
 * WiFi, and four headings of "not present" is a screen telling you about hardware that does
 * not exist. Nothing here is editable - it is a reading, not a setting, which is the rule the
 * About sections follow - and the settings behind it live under Network and Bluetooth.
 */
static void build_connection(const struct mesh_ui_connection_status *conn, struct item_list *list) {
    char buffer[48];
    if (conn == NULL || !conn->valid) {
        return;
    }
    if (conn->has_wifi) {
        item_heading(list, MESH_STR_HEAD_CONN_WIFI);
        item_toggle(list, MESH_STR_CONN_CONNECTED, conn->wifi_connected);
        item_text(list, MESH_STR_CONN_NETWORK, INKSTAND_FORM_INFO,
                  conn->wifi_ssid[0] != '\0' ? conn->wifi_ssid : inkcell_str(MESH_STR_COMMON_NONE));
        format_ipv4(conn->wifi_ip, buffer, sizeof buffer);
        item_text(list, MESH_STR_CONN_IP, INKSTAND_FORM_INFO, buffer);
        inkcell_str_format(buffer, sizeof buffer, MESH_STR_VALUE_DBM, (int)conn->wifi_rssi);
        item_text(list, MESH_STR_CONN_SIGNAL, INKSTAND_FORM_INFO, buffer);
        item_toggle(list, MESH_STR_CONN_MQTT, conn->wifi_mqtt);
        item_toggle(list, MESH_STR_CONN_SYSLOG, conn->wifi_syslog);
    }
    if (conn->has_ethernet) {
        item_heading(list, MESH_STR_HEAD_CONN_ETHERNET);
        item_toggle(list, MESH_STR_CONN_CONNECTED, conn->ethernet_connected);
        format_ipv4(conn->ethernet_ip, buffer, sizeof buffer);
        item_text(list, MESH_STR_CONN_IP, INKSTAND_FORM_INFO, buffer);
        item_toggle(list, MESH_STR_CONN_MQTT, conn->ethernet_mqtt);
        item_toggle(list, MESH_STR_CONN_SYSLOG, conn->ethernet_syslog);
    }
    if (conn->has_bluetooth) {
        item_heading(list, MESH_STR_HEAD_CONN_BLUETOOTH);
        item_toggle(list, MESH_STR_CONN_CONNECTED, conn->bluetooth_connected);
        /* The PIN the radio is currently asking for, which is the answer when pairing has just
           failed - and 0 when it is not asking for one at all. */
        if (conn->bluetooth_pin != 0U) {
            inkcell_str_format(buffer, sizeof buffer, MESH_STR_VALUE_PLAIN,
                               (unsigned)conn->bluetooth_pin);
            item_text(list, MESH_STR_CONN_PAIRING_PIN, INKSTAND_FORM_INFO, buffer);
        }
        inkcell_str_format(buffer, sizeof buffer, MESH_STR_VALUE_DBM, (int)conn->bluetooth_rssi);
        item_text(list, MESH_STR_CONN_SIGNAL, INKSTAND_FORM_INFO, buffer);
    }
    if (conn->has_serial) {
        item_heading(list, MESH_STR_HEAD_CONN_SERIAL);
        item_toggle(list, MESH_STR_CONN_CONNECTED, conn->serial_connected);
        inkcell_str_format(buffer, sizeof buffer, MESH_STR_VALUE_PLAIN,
                           (unsigned)conn->serial_baud);
        item_text(list, MESH_STR_CONN_BAUD, INKSTAND_FORM_INFO, buffer);
    }
}

/*
 * An install already running, which takes the whole section over.
 *
 * Nothing else here is pressable while it does: the channel decides which question a *check*
 * asks and the check would take the fetcher, and there is no second install to start. So the
 * section drops to one row that says where the job has got to, plus its detail where the radio
 * or the loader said something in its own words.
 *
 * A meter rather than a plain row, and only two of the states carry a level: `firmware_update.c`
 * answers 0 for the steps that have no fraction, and a bar drawn at 0% through a forty-second
 * wait for a bootloader is a bar that says the work stalled. INKSTAND_FORM_METER_UNKNOWN is the
 * honest reading there - the same one About's own check row uses for a reply with no length.
 */
static bool build_radio_firmware_running(const struct mesh_ui_settings *s, struct item_list *list) {
    const enum mesh_firmware_update_state state =
        (enum mesh_firmware_update_state)s->fw_update_state;
    if (!mesh_firmware_update_state_busy(state)) {
        return false;
    }

    char value[MESH_UI_SETTINGS_VALUE_MAX];
    uint32_t level = INKSTAND_FORM_METER_UNKNOWN;
    if (s->fw_update_progress > 0U) {
        level = (uint32_t)s->fw_update_progress * 10U;
        snprintf(value, sizeof value, "%s %u%%", mesh_firmware_update_state_name(state),
                 (unsigned)s->fw_update_progress);
    } else {
        inkwell_str_copy(value, sizeof value, mesh_firmware_update_state_name(state));
    }
    item_meter(list, MESH_STR_FW_INSTALLING, value, level);
    /* What is being installed, so the one row that is left still names the release. The
       version is a version and is not translated. */
    if (s->fw_latest[0] != '\0') {
        item_text(list, MESH_STR_FW_NEWER, INKSTAND_FORM_INFO, s->fw_latest);
    }
    return true;
}

/*
 * What newer firmware exists for the radio, under the version row that prompted the question.
 *
 * Three rows at most, and every one of them is a fact rather than an offer: this client installs
 * nothing yet, so the rows say what is out there and why it cannot be had from here. That last part
 * is the one worth keeping - a radio behaving oddly is often a radio on old firmware, and "connect
 * it by USB" is a thing somebody can go and do, where a missing row is not.
 *
 * **Every value here is a value, never a sentence.** The column is about two dozen cells at the
 * device scale and a settings row has no supporting line to wrap onto, so "%s available (radio
 * has %s)" came out as "2.7.26.54e0d8d available (" - a row that reads as a bug. What carries
 * the difference instead is the row's *name*: once the answer is a version worth having, the
 * label says "Newer firmware" and the value is just the version. About's own "Install %s" row
 * made the same move for the same reason.
 *
 * The status row is a meter while a check runs, for the reason About's own is: "checking"
 * answers whether something is happening and not whether it is *still* happening, and these two
 * documents are 200 KB over whatever wifi a handheld has. Neither has a fraction - the reply
 * has no length until it arrives - so it is the indeterminate bar both times.
 */
static void build_radio_firmware(const struct mesh_ui_settings *s, struct item_list *list) {
    if (!s->fw_supported) {
        /* A build with no TLS. Said once rather than offering a press that cannot run. */
        item_str(list, MESH_STR_FW_LATEST, INKSTAND_FORM_INFO, MESH_STR_ABOUT_UPDATES_UNAVAILABLE);
        return;
    }
    if (build_radio_firmware_running(s, list)) {
        return;
    }
    /*
     * Which of upstream's two lists a check will read. Above the status row for the reason
     * About's own channel row sits above its status: it decides which question the press below
     * will ask, so it reads first.
     *
     * While a check runs it drops to a plain fact - the module refuses a switch with a document
     * in flight, and a row that refuses is worse than one that never invited the press.
     */
    if (s->fw_busy) {
        item_text(list, MESH_STR_FW_CHANNEL, INKSTAND_FORM_INFO, s->fw_channel);
        item_meter(list, MESH_STR_FW_LATEST,
                   mesh_firmware_state_name((enum mesh_firmware_state)s->fw_state),
                   INKSTAND_FORM_METER_UNKNOWN);
        return;
    }
    item_action(list, MESH_STR_FW_CHANNEL, s->fw_channel,
                MESH_UI_SETTINGS_ACTION_CYCLE_FIRMWARE_CHANNEL);

    const bool newer = s->fw_state == (uint8_t)MESH_FIRMWARE_AVAILABLE;
    /*
     * The one thing the check concluded. Before a check has run the module has no line and the
     * state's own word stands in - which is also why the press below is offered whatever state
     * this is in: a check is worth repeating, and a failed one is worth retrying.
     */
    item_text(list, newer ? MESH_STR_FW_NEWER : MESH_STR_FW_LATEST, INKSTAND_FORM_INFO,
              s->fw_message[0] != '\0'
                  ? s->fw_message
                  : mesh_firmware_state_name((enum mesh_firmware_state)s->fw_state));
    item_verb(list, MESH_STR_FW_CHECK, MESH_UI_SETTINGS_ACTION_CHECK_RADIO_FIRMWARE);

    /*
     * Why it cannot be installed, once there is something to install. Not shown before a check
     * or after a failed one: the reason would be about a board nothing has looked up yet, and a
     * refusal is only useful next to the thing being refused.
     */
    if (!newer && s->fw_state != (uint8_t)MESH_FIRMWARE_UP_TO_DATE) {
        return;
    }
    if (!s->fw_can_install) {
        /*
         * An empty reason is not a refusal, and must not be drawn as one.
         *
         * `firmware_blocker()` answers NONE when every question about the board came back
         * right - it is attached, it resolved to exactly one entry, and that entry's path is
         * the bus it is on - so a radio that is simply already on the newest release reaches
         * here with nothing to say. `fw_can_install` is false because its first condition is
         * MESH_FIRMWARE_AVAILABLE, not because anything is in the way.
         *
         * This used to fall through to a "not built yet" placeholder left over from before the
         * install press existed, so an up-to-date radio on its own correct bus - the ordinary
         * case, a few seconds after any flash - was told the installer had not been written.
         * The status row above already says "up to date", which is the whole of what a row
         * here could add.
         */
        if (s->fw_blocker_reason[0] == '\0') {
            return;
        }
        item_text(list, MESH_STR_FW_INSTALLING, INKSTAND_FORM_INFO, s->fw_blocker_reason);
        return;
    }
    /*
     * How the last one went, above the press that would try again.
     *
     * Only after one has run this session - MESH_FIRMWARE_UPDATE_IDLE draws nothing, because a
     * row saying "not started" under a row offering to start it is the same sentence twice. A
     * failure shows the module's word for what broke plus, where the radio or the loader said
     * something, its own words; those stay untranslated, like a log line.
     */
    if (s->fw_update_state == (uint8_t)MESH_FIRMWARE_UPDATE_FAILED) {
        item_text(list, MESH_STR_FW_INSTALLING, INKSTAND_FORM_INFO,
                  s->fw_update_detail[0] != '\0'
                      ? s->fw_update_detail
                      : mesh_firmware_update_error_name(
                            (enum mesh_firmware_update_error)s->fw_update_error));
    } else if (s->fw_update_state == (uint8_t)MESH_FIRMWARE_UPDATE_DONE) {
        item_text(list, MESH_STR_FW_INSTALLING, INKSTAND_FORM_INFO,
                  mesh_firmware_update_state_name(MESH_FIRMWARE_UPDATE_DONE));
    }
    /*
     * The press, and which bus it is over. The row is the only place in the UI that has to know
     * - the confirm sheet, the action bar and the app all read the action it emits - which is
     * why the bus is baked into the action rather than looked up again downstream.
     */
    item_verb(list, MESH_STR_FW_INSTALL,
              s->fw_bus == (uint8_t)MESH_FIRMWARE_PATH_BLE
                  ? MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE
                  : MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB);
}

static void build_radio(const struct mesh_ui_settings *s, const struct mesh_ui_handshake_state *hs,
                        struct item_list *list) {
    char buffer[48];
    /*
     * Whose radio the rest of this section - and the rest of this tab - is about.
     *
     * First, because every row under it changes meaning with it: the firmware version, the
     * capabilities and the admin session below all describe the node named here, and reading
     * them in the belief that they described the radio in your hand is the one mistake this
     * section can cause. Drawn only while there is a remote target, so the ordinary case is the
     * section exactly as it was.
     */
    if (s->admin_dest != 0U) {
        item_heading(list, MESH_STR_RADIO_ADMIN_REMOTE_HEAD);
        item_text(list, MESH_STR_RADIO_ADMIN_REMOTE_NODE, INKSTAND_FORM_INFO, s->admin_dest_name);
        item_verb(list, MESH_STR_RADIO_ADMIN_REMOTE_RETURN, MESH_UI_SETTINGS_ACTION_ADMIN_LOCAL);
    }
    /* Whether the firmware group below belongs on this screen at all. It is about the radio on
       the end of the link - the check reads that radio's model and the install writes down that
       cable - and while this section is describing somebody else's node it would be three rows
       answering a question nobody asked here, under a heading that has just named another
       radio. There is no remote firmware install to offer in its place: an image crosses a
       cable or a BLE link, never a mesh. */
    const bool link_firmware = (s->admin_dest == 0U);
    if (s->has_metadata) {
        item_text(list, MESH_STR_RADIO_FIRMWARE, INKSTAND_FORM_INFO,
                  s->firmware_version[0] != '\0' ? s->firmware_version
                                                 : inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT));
        item_text(list, MESH_STR_RADIO_HARDWARE, INKSTAND_FORM_INFO,
                  /* The board upstream's hardware list identified, when a check has run: it is
                     the name that decides which image this radio takes, and "Heltec Mesh Node
                     T114" says more than the HardwareModel enum's own spelling of it. Falls
                     back to that spelling, which is what every radio has before a check - and
                     always falls back for a node being administered over the mesh, because the
                     check that produced that name was about a different radio. */
                  (link_firmware && s->fw_board[0] != '\0')
                      ? s->fw_board
                      : mesh_radio_hw_model_name(s->hw_model, buffer, sizeof buffer));
    }
    if (link_firmware) {
        build_radio_firmware(s, list);
    }
    /*
     * The node number, which is the one row here that does not come from the admin path at all:
     * `my_info` is the handshake's, so it is our own radio's whatever this section is
     * describing. With a remote target it is the target's number that belongs here, and the
     * reboot count is simply dropped - MyNodeInfo is a thing a radio tells the client attached
     * to it, and there is no admin verb that asks a node over the mesh how many times it has
     * restarted.
     */
    if (s->admin_dest != 0U) {
        inkcell_str_format(buffer, sizeof buffer, MESH_STR_NODE_VAL_USER_ID_HEX, s->admin_dest);
        item_text(list, MESH_STR_RADIO_NODE_NUMBER, INKSTAND_FORM_INFO, buffer);
    } else if (hs != NULL && hs->has_my_info) {
        inkcell_str_format(buffer, sizeof buffer, MESH_STR_NODE_VAL_USER_ID_HEX,
                           hs->my_info.node_num);
        item_text(list, MESH_STR_RADIO_NODE_NUMBER, INKSTAND_FORM_INFO, buffer);
        inkcell_str_format(buffer, sizeof buffer, MESH_STR_VALUE_PLAIN, hs->my_info.reboot_count);
        item_text(list, MESH_STR_RADIO_REBOOTS, INKSTAND_FORM_INFO, buffer);
    }
    if (s->has_metadata) {
        snprintf(buffer, sizeof buffer, "%s%s%s%s",
                 s->has_bluetooth_radio ? inkcell_str(MESH_STR_RADIO_CAP_BLE) : "",
                 s->has_wifi ? inkcell_str(MESH_STR_RADIO_CAP_WIFI) : "",
                 s->has_ethernet ? inkcell_str(MESH_STR_RADIO_CAP_ETHERNET) : "",
                 s->has_pkc ? inkcell_str(MESH_STR_RADIO_CAP_PKC) : "");
        item_text(list, MESH_STR_RADIO_CAPABILITIES, INKSTAND_FORM_INFO,
                  buffer[0] != '\0' ? buffer : inkcell_str(MESH_STR_RADIO_CAP_NONE));
        item_toggle(list, MESH_STR_RADIO_CAN_SHUT_DOWN, s->can_shutdown);
        /* DeviceMetadata.has_xeddsa: the firmware either compiled packet signature verification
           in or it did not. Off here is why Security's "Packet signing" row can be set to
           Strict and change nothing. */
        item_toggle(list, MESH_STR_RADIO_SIGNATURE_CHECKS, s->has_xeddsa);
    }
    if (s->admin_ok) {
        inkcell_str_format(buffer, sizeof buffer, MESH_STR_RADIO_ADMIN_OK,
                           (unsigned)s->admin_replies,
                           s->write_pending ? inkcell_str(MESH_STR_RADIO_ADMIN_SAVING)
                           : s->admin_busy  ? inkcell_str(MESH_STR_RADIO_ADMIN_REFRESHING)
                                            : "");
    } else {
        snprintf(buffer, sizeof buffer, "%s",
                 inkcell_str(s->admin_busy ? MESH_STR_RADIO_ADMIN_WAITING
                                           : MESH_STR_RADIO_ADMIN_NO_REPLY));
    }
    item_text(list, MESH_STR_RADIO_ADMIN_SESSION, INKSTAND_FORM_INFO, buffer);
    build_connection(&s->connection, list);
}

static void build_user(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_USER_LONG_NAME, 0U, s->long_name);
    item_field(list, MESH_UI_FIELD_USER_SHORT_NAME, 0U, s->short_name);
    item_field(list, MESH_UI_FIELD_USER_LICENSED, s->is_licensed ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_USER_UNMESSAGEABLE, s->is_unmessagable ? 1U : 0U, NULL);

    /*
     * Contact sharing, under the fields it is made of.
     *
     * Here rather than on the Nodes tab because this section *is* the record a contact carries:
     * the name above, the number and the key the radio keeps beside them. The pair sits under a
     * heading because they are a subject of their own rather than two more properties of this
     * radio's owner - the second one is about somebody else's.
     *
     * The show row appears only when there is a link to show, which is what a non-empty
     * `contact_url` means: the owner reply has arrived and had a public key in it. The add row
     * waits on the radio answering admin at all, because that is the whole of what an
     * add_contact needs - unlike a channel import it overwrites no table and so needs no
     * settled reading of one.
     */
    if (s->contact_url[0] != '\0' || s->admin_ok) {
        item_heading(list, MESH_STR_USER_CONTACT_HEAD);
    }
    if (s->contact_url[0] != '\0') {
        item_verb(list, MESH_STR_USER_SHARE_CONTACT_ROW, MESH_UI_SETTINGS_ACTION_SHARE_CONTACT);
    }
    if (s->admin_ok) {
        item_verb(list, MESH_STR_USER_ADD_CONTACT_ROW, MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT);
    }
}

static void build_device(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_DEVICE_ROLE, s->role, NULL);
    item_field(list, MESH_UI_FIELD_DEVICE_TZDEF, 0U, s->tzdef);
    item_field(list, MESH_UI_FIELD_DEVICE_REBROADCAST, s->rebroadcast_mode, NULL);
    item_field(list, MESH_UI_FIELD_DEVICE_NODEINFO_SECS, s->node_info_broadcast_secs, NULL);
    /* The protobuf field is led_heartbeat_disabled; the row is the plain statement. */
    item_field(list, MESH_UI_FIELD_DEVICE_LED_HEARTBEAT, s->led_heartbeat_disabled ? 0U : 1U, NULL);
    item_field(list, MESH_UI_FIELD_DEVICE_DOUBLE_TAP, s->double_tap_as_button_press ? 1U : 0U,
               NULL);
}

static void build_display(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_DISPLAY_SCREEN_ON, s->screen_on_secs, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_CAROUSEL, s->carousel_secs, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_COMPASS, s->compass_orientation, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_12H, s->use_12h_clock ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_UNITS, s->units, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_FLIP, s->flip_screen ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_OLED, s->oled, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_MODE, s->displaymode, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_HEADING_BOLD, s->heading_bold ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_WAKE_ON_MOTION, s->wake_on_tap_or_motion ? 1U : 0U,
               NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_LONG_NAMES, s->use_long_node_name ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_DISPLAY_MESSAGE_BUBBLES, s->enable_message_bubbles ? 1U : 0U,
               NULL);
}

/*
 * A preset row constrained by the region it will be transmitted in.
 *
 * The firmware sends its own table of which modem presets each region will take
 * (`FromRadio.region_presets`) precisely so a client can stop offering an illegal pair, and the
 * LoRa section has read it since phase 14 item 8. The beacon's offered channel and its four
 * targets are the same pair one section over, so the rule lives here rather than being written
 * out five more times.
 *
 * `region` is the *effective* one and the caller works it out, because what "no region named"
 * means is the caller's to know: for a target it is the running config's region, which is what
 * the firmware falls back to; for an offer it is nothing at all, and the caller passes 0 so the
 * row keeps every value.
 *
 * `shift` is how far the row's values sit above the presets they name - 0 for LoRa, 1 for the
 * beacon's rows, whose 0 means "absent". Absent is always legal, which is the bit the shifted
 * mask puts back at the bottom.
 */
static void constrain_preset_row(const struct mesh_ui_settings *s, uint32_t region,
                                 struct mesh_ui_settings_item *preset_row, uint32_t shift) {
    if (preset_row == NULL || region == 0U) {
        return;
    }
    const struct mesh_ui_region_preset *legal = mesh_ui_settings_region_preset(s, region);
    if (legal == NULL) {
        return; /* a firmware that predates the message constrains nothing */
    }
    const uint32_t choices =
        shift == 0U ? legal->presets : (legal->presets << shift) | ((1U << shift) - 1U);
    preset_row->choices = choices;
    preset_row->conflict = !mesh_ui_settings_choice_allowed(
        choices, mesh_ui_settings_enum_count(preset_row->field), preset_row->number);
}

static void build_lora(const struct mesh_ui_settings *s, struct item_list *list) {
    /*
     * The region and the preset are one pair, and this is the only place in the tab where one
     * row's value decides what another row will take.
     *
     * The firmware sends its own table of which modem presets are legal in each region
     * (FromRadio.region_presets) precisely so a client can stop offering an illegal pair. So
     * the preset row carries that region's set, Left and Right step inside it, and a preset
     * that is not in it - which is what a radio configured on another continent arrives holding
     * - is still shown, marked as the disagreement it is. A row that hid the setting the node
     * is actually on would be worse than one that shows it and says so.
     *
     * A radio whose firmware predates the message says nothing, mesh_ui_settings_region_preset()
     * answers NULL, and both rows behave exactly as they did before the table existed.
     */
    struct mesh_ui_settings_item *region_row =
        item_field(list, MESH_UI_FIELD_LORA_REGION, s->region, NULL);
    /* The row's value rather than the radio's, which is the whole reason the region is built
       before the preset: a region edited a moment ago is the one the preset has to be legal in,
       not the one the radio is still sitting on. */
    const uint32_t region = region_row != NULL ? region_row->number : s->region;
    const struct mesh_ui_region_preset *legal = mesh_ui_settings_region_preset(s, region);
    const struct mesh_ui_settings_item *use_preset_row =
        item_field(list, MESH_UI_FIELD_LORA_USE_PRESET, s->use_preset ? 1U : 0U, NULL);
    const bool preset_on = use_preset_row != NULL ? use_preset_row->number != 0U : s->use_preset;
    struct mesh_ui_settings_item *preset_row =
        item_field(list, MESH_UI_FIELD_LORA_PRESET, s->modem_preset, NULL);
    constrain_preset_row(s, region, preset_row, 0U);
    if (preset_row != NULL) {
        preset_row->inactive = !preset_on;
        preset_row->inactive_note = preset_on ? INKCELL_STR_NONE : MESH_STR_TOAST_USED_WITH_PRESET;
    }
    /*
     * A licensed band on a node that does not claim a licence. The firmware marks the amateur
     * regions itself, and the pair it disagrees with is one section over: `Licensed operator`
     * in User, which is what the ham rows at the bottom of this section set.
     *
     * A warning rather than a refusal, and only when the owner record says the operator is not
     * licensed: an operator who is gets no mark, because for them this is simply the band they
     * are on.
     *
     * `has_owner` is what makes that read of the flag honest, and it is load-bearing rather
     * than defensive. The preset map arrives *early* in the handshake - before the channel
     * table, and so before our own NodeInfo carries the owner record - so there is a real
     * window in which the band is known and the licence is not. Through it `is_licensed` is
     * merely a zeroed member, and a mark drawn from that would be this client telling an
     * operator it does not yet know anything about that they are unlicensed.
     */
    if (region_row != NULL && legal != NULL && legal->licensed_only && s->has_owner &&
        !s->is_licensed) {
        region_row->conflict = true;
    }
    /* The manual trio only applies with the preset off; they stay listed so the row count
       does not move under the cursor as the toggle is edited. */
    const enum mesh_ui_setting_field manual[] = {
        MESH_UI_FIELD_LORA_BANDWIDTH, MESH_UI_FIELD_LORA_SPREAD, MESH_UI_FIELD_LORA_CODING};
    const uint32_t manual_value[] = {s->bandwidth, s->spread_factor, s->coding_rate};
    for (size_t m = 0; m < sizeof manual / sizeof manual[0]; ++m) {
        struct mesh_ui_settings_item *row = item_field(list, manual[m], manual_value[m], NULL);
        if (row != NULL) {
            row->inactive = preset_on;
            row->inactive_note = preset_on ? MESH_STR_TOAST_USED_WITHOUT_PRESET : INKCELL_STR_NONE;
        }
    }
    item_field(list, MESH_UI_FIELD_LORA_HOPS, s->hop_limit, NULL);
    item_field(list, MESH_UI_FIELD_LORA_TX_ENABLED, s->tx_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_LORA_TX_POWER, (uint32_t)(uint8_t)s->tx_power, NULL);
    item_field(list, MESH_UI_FIELD_LORA_IGNORE_MQTT, s->ignore_mqtt ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_LORA_OK_TO_MQTT, s->config_ok_to_mqtt ? 1U : 0U, NULL);

    /*
     * The advanced group: what the radio does with the band rather than which band it is on.
     * Three of the five are numbers typed as text, pre-filled with what the radio reported, so
     * a row nobody edits keeps what it had - the rule the coordinate rows follow.
     */
    char typed[MESH_UI_SETTINGS_VALUE_MAX];
    item_heading(list, MESH_STR_HEAD_LORA_ADVANCED);
    item_field(list, MESH_UI_FIELD_LORA_BOOST_GAIN, s->sx126x_rx_boosted_gain ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_LORA_OVERRIDE_DUTY, s->override_duty_cycle ? 1U : 0U, NULL);
    snprintf(typed, sizeof typed, "%u", (unsigned)s->channel_num);
    item_field(list, MESH_UI_FIELD_LORA_CHANNEL_NUM, 0U, typed);
    mesh_ui_settings_decimal_text(s->override_frequency_scaled, MESH_UI_FREQUENCY_DIGITS,
                                  MESH_UI_FREQUENCY_DIGITS, typed, sizeof typed);
    item_field(list, MESH_UI_FIELD_LORA_OVERRIDE_FREQ, 0U, typed);
    mesh_ui_settings_decimal_text(s->frequency_offset_scaled, MESH_UI_HERTZ_DIGITS,
                                  MESH_UI_HERTZ_DIGITS, typed, sizeof typed);
    item_field(list, MESH_UI_FIELD_LORA_FREQUENCY_TRIM, 0U, typed);

    /*
     * ignore_incoming's three slots. Listed empty, the way a channel slot is: an empty row is
     * where the next one goes, and a list that only showed the full slots would have nowhere
     * to put a fourth press.
     */
    item_heading(list, MESH_STR_HEAD_LORA_IGNORED);
    for (uint32_t i = 0; i < 3U; ++i) {
        mesh_ui_settings_node_id_text(s->ignore_incoming[i], typed, sizeof typed);
        item_field(list, (enum mesh_ui_setting_field)(MESH_UI_FIELD_LORA_IGNORE_NODE_0 + i), 0U,
                   typed);
    }

    /*
     * Ham mode. Three rows and the press that reads them, which is the fixed-position shape:
     * `set_ham_mode` is one verb over the owner's names, the primary channel's key and this
     * section's own frequency and power, so Y cannot be what sends it.
     *
     * The call sign is pre-filled from the long name only when the radio already says it is
     * licensed - on an unlicensed node the long name is a nickname, and offering it as a call
     * sign would be this client putting words in an operator's mouth.
     */
    item_heading(list, MESH_STR_HEAD_LORA_HAM);
    item_field(list, MESH_UI_FIELD_LORA_HAM_CALL_SIGN, 0U, s->is_licensed ? s->long_name : "");
    mesh_ui_settings_decimal_text(s->override_frequency_scaled, MESH_UI_FREQUENCY_DIGITS,
                                  MESH_UI_FREQUENCY_DIGITS, typed, sizeof typed);
    item_field(list, MESH_UI_FIELD_LORA_HAM_FREQUENCY, 0U, typed);
    item_field(list, MESH_UI_FIELD_LORA_HAM_TX_POWER, (uint32_t)(uint8_t)s->tx_power, NULL);
    item_verb(list, MESH_STR_SETTINGS_SET_HAM_MODE, MESH_UI_SETTINGS_ACTION_SET_HAM_MODE);
}

static void build_bluetooth(const struct mesh_ui_settings *s, struct item_list *list) {
    char pin[8];
    snprintf(pin, sizeof pin, "%06u", (unsigned)(s->fixed_pin % 1000000U));
    item_field(list, MESH_UI_FIELD_BT_ENABLED, s->bluetooth_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_BT_MODE, s->pairing_mode, NULL);
    item_field(list, MESH_UI_FIELD_BT_PIN, 0U, pin);
}

/*
 * Which bit of NetworkConfig.enabled_protocols is UDP broadcast.
 *
 * A literal for the reason the section table's excluded-module bits are literals - this side of
 * the fence has no nanopb - and pinned against meshtastic_Config_NetworkConfig_ProtocolFlags by
 * a test rather than by a reader.
 */
#define NETWORK_PROTOCOL_UDP_BROADCAST 0x0001U

/*
 * What the radio was told to do about WiFi and Ethernet. Read-only, every row of it.
 *
 * The section that was fetched on every refresh and read by nothing until now. It answers the
 * half of "why is this radio not reaching the broker" that About radio cannot: that screen
 * shows DeviceConnectionStatus, which is what the interface is *doing*, and this is what it was
 * *told*. The two disagreeing - configured for a network it never joined, or holding a static
 * address on a subnet that is not there - is the answer often enough to be worth the screen.
 *
 * The key is not here and is not masked either: `wifi_psk` is never published out of the radio
 * record, so there is no row to mask. The four static rows are drawn only under Static, because
 * under DHCP they are whatever somebody last typed into a phone and say nothing about this
 * radio - a read-only list may do that, where an editable one may not: nothing here can move a
 * row count under a cursor mid-edit, because nothing here edits.
 */
static void build_network(const struct mesh_ui_settings *s, struct item_list *list) {
    char buffer[48];
    item_toggle(list, MESH_STR_NETWORK_WIFI, s->wifi_enabled);
    item_text(list, MESH_STR_NETWORK_SSID, INKSTAND_FORM_INFO,
              s->wifi_ssid[0] != '\0' ? s->wifi_ssid : inkcell_str(MESH_STR_COMMON_NONE));
    item_toggle(list, MESH_STR_NETWORK_ETHERNET, s->eth_enabled);
    item_toggle(list, MESH_STR_NETWORK_IPV6, s->ipv6_enabled);
    item_str(list, MESH_STR_NETWORK_ADDRESS, INKSTAND_FORM_INFO,
             s->address_mode == 1U ? MESH_STR_ENUM_ADDRESS_STATIC : MESH_STR_ENUM_ADDRESS_DHCP);
    if (s->address_mode == 1U) {
        item_heading(list, MESH_STR_HEAD_NETWORK_STATIC);
        format_ipv4(s->ipv4_ip, buffer, sizeof buffer);
        item_text(list, MESH_STR_CONN_IP, INKSTAND_FORM_INFO, buffer);
        format_ipv4(s->ipv4_gateway, buffer, sizeof buffer);
        item_text(list, MESH_STR_NETWORK_GATEWAY, INKSTAND_FORM_INFO, buffer);
        format_ipv4(s->ipv4_subnet, buffer, sizeof buffer);
        item_text(list, MESH_STR_NETWORK_SUBNET, INKSTAND_FORM_INFO, buffer);
        format_ipv4(s->ipv4_dns, buffer, sizeof buffer);
        item_text(list, MESH_STR_NETWORK_DNS, INKSTAND_FORM_INFO, buffer);
    }
    item_text(list, MESH_STR_NETWORK_NTP, INKSTAND_FORM_INFO,
              s->ntp_server[0] != '\0' ? s->ntp_server : inkcell_str(MESH_STR_NETWORK_NTP_DEFAULT));
    item_text(list, MESH_STR_NETWORK_SYSLOG, INKSTAND_FORM_INFO,
              s->rsyslog_server[0] != '\0' ? s->rsyslog_server : inkcell_str(MESH_STR_COMMON_NONE));
    item_toggle(list, MESH_STR_NETWORK_UDP_BROADCAST,
                (s->enabled_protocols & NETWORK_PROTOCOL_UDP_BROADCAST) != 0U);
}

static void channel_label(uint8_t index, const char *name, char *out, size_t out_len) {
    inkcell_str_format(out, out_len, MESH_STR_CHANNELS_SLOT, (unsigned)index,
                       name[0] != '\0'
                           ? name
                           : inkcell_str(index == 0U ? MESH_STR_ENUM_CHANNEL_PRIMARY
                                                     : INKCELL_STR_COMMON_UNKNOWN_SHORT));
}

/*
 * What a slot says in the list, which is what it is and how it is keyed - and not its two MQTT
 * bits.
 *
 * It used to be four facts ("%s, %s, up %s, down %s"), and a settings row has about two dozen
 * cells of value column: every row of the list came out cut mid-word - "primary, default key,"
 * and "secondary, AES-128, up" - so the two bits at the end were never once readable and the
 * clipping took a comma's worth of the two that were. That is the radio-firmware rows' rule
 * reached from the other direction: a settings value is short or it is nothing.
 *
 * The uplink and downlink toggles are the first thing under this row, in the slot's own section,
 * which is where a bit that is a *setting* belongs. What is left here is the pair a reader is
 * scanning the list for - which slot is the primary, and what each one is encrypted with.
 */
static void channel_summary(uint8_t role, uint8_t psk_len, char *out, size_t out_len) {
    const char *key = inkcell_str(psk_len == 0U    ? MESH_STR_CHANNELS_KEY_NONE
                                  : psk_len == 1U  ? MESH_STR_CHANNELS_KEY_DEFAULT
                                  : psk_len == 16U ? MESH_STR_CHANNELS_KEY_AES128
                                  : psk_len == 32U ? MESH_STR_CHANNELS_KEY_AES256
                                                   : MESH_STR_CHANNELS_KEY_ODD);
    inkcell_str_format(
        out, out_len, MESH_STR_CHANNELS_SUMMARY,
        inkcell_str(role == 1U ? MESH_STR_CHANNELS_ROLE_PRIMARY : MESH_STR_CHANNELS_ROLE_SECONDARY),
        key);
}

/* The channel list. With the radio's full table held every slot is listed, disabled ones
   included, and A opens it: that is how a channel is added (set up an empty slot) or removed
   (set its role to Disabled). Without the table only the handshake summary of the enabled
   slots is shown, read-only. */
static void build_channels(const struct mesh_ui_settings *s,
                           const struct mesh_ui_handshake_state *hs, struct item_list *list) {
    char label[MESH_UI_SETTINGS_LABEL_MAX];
    if (s->has_channels) {
        for (uint32_t i = 0; i < MESH_UI_MAX_CHANNELS; ++i) {
            const struct mesh_ui_channel_detail *channel = &s->channels[i];
            if (!channel->present) {
                continue;
            }
            if (channel->role == 0U) {
                inkcell_str_format(label, sizeof label, MESH_STR_CHANNELS_SLOT_EMPTY,
                                   (unsigned)channel->index);
            } else {
                channel_label(channel->index, channel->name, label, sizeof label);
            }
            struct mesh_ui_settings_item *item = item_add_named(list, label, INKSTAND_FORM_ACTION);
            if (item == NULL) {
                continue;
            }
            item->number = channel->index;
            if (channel->role == 0U) {
                snprintf(item->value, sizeof item->value, "%s",
                         inkcell_str(MESH_STR_CHANNELS_SET_UP));
            } else {
                channel_summary(channel->role, channel->psk_len, item->value, sizeof item->value);
            }
        }
    } else if (hs != NULL) {
        for (uint32_t i = 0; i < hs->channel_count && i < MESH_UI_MAX_CHANNELS; ++i) {
            const struct mesh_ui_channel *channel = &hs->channels[i];
            if (channel->role == 0U) {
                continue;
            }
            channel_label(channel->index, channel->name, label, sizeof label);
            struct mesh_ui_settings_item *item = item_add_named(list, label, INKSTAND_FORM_INFO);
            if (item != NULL) {
                channel_summary(channel->role, channel->psk_len, item->value, sizeof item->value);
                item->number = channel->index;
            }
        }
    }
    if (list->count == 0U) {
        item_str(list, MESH_STR_SETTINGS_CHANNELS_ROW, INKSTAND_FORM_INFO,
                 MESH_STR_CHANNELS_NONE_KNOWN);
    }

    /*
     * Sharing, under the slots, and both rows wait for the same moment: the radio having
     * finished answering for every slot and for its LoRa config (`channels_settled`, which is
     * mesh_channel_share_settled() published).
     *
     * `has_channels` would be the obvious condition and is the wrong one - it is true from the
     * *first* channel reply, and neither row can keep its promise against a table that is still
     * arriving. A code built then is missing the secondaries that have not landed; an import
     * then reads every unseen slot as free and writes over one.
     *
     * One condition for both rather than one each, so they appear together: two conditions a
     * reply apart would put the import row on screen and then push it down when the share row
     * arrived under the user's cursor. The share row needs the extra half - there has to be a
     * link, which a radio with no primary does not have - and that is what the empty test is.
     */
    if (s->channels_settled) {
        if (s->share_url[0] != '\0') {
            item_verb(list, MESH_STR_CHANNELS_SHARE_ROW, MESH_UI_SETTINGS_ACTION_SHARE_CHANNELS);
        }
        item_verb(list, MESH_STR_CHANNELS_IMPORT_ROW, MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS);
    }
}

/* One channel's rows. The primary slot's role is shown but not offered: a mesh with two
   primaries or none is not something to reach by accident. */
static void build_channel(const struct mesh_ui_settings *s, uint8_t slot, struct item_list *list) {
    if (slot >= MESH_UI_MAX_CHANNELS || !s->channels[slot].present) {
        return;
    }
    const struct mesh_ui_channel_detail *channel = &s->channels[slot];
    item_field(list, MESH_UI_FIELD_CHANNEL_NAME, 0U, channel->name);
    if (channel->role == 1U) {
        item_str(list, MESH_STR_SETTINGS_ROLE_ROW, INKSTAND_FORM_INFO,
                 MESH_STR_ENUM_CHANNEL_PRIMARY);
    } else {
        item_field(list, MESH_UI_FIELD_CHANNEL_ROLE, channel->role == 2U ? 1U : 0U, NULL);
    }
    item_key_field(list, MESH_UI_FIELD_CHANNEL_KEY, channel->psk, channel->psk_len);
    item_field(list, MESH_UI_FIELD_CHANNEL_UPLINK, channel->uplink_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_CHANNEL_DOWNLINK, channel->downlink_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_CHANNEL_POSITION, channel->position_precision, NULL);
    item_field(list, MESH_UI_FIELD_CHANNEL_MUTED, channel->is_muted ? 1U : 0U, NULL);
    /*
     * Emptying the slot, under the rows that describe it and offered only where it would do
     * something.
     *
     * Not on the primary, for the reason its role is not offered either: there is one of it and
     * a radio without it is off its own mesh. Not on a slot that is already empty, because a
     * verb that changes nothing is the thing the action table exists to prevent.
     *
     * "Empty" is asked of every field the write resets, and it has to be: the obvious three -
     * role, name, key - leave a disabled slot carrying a position precision, a mute, an MQTT
     * flag or an `id` reading as nothing to clear, so the one row that would erase them is the
     * row withheld. Those states are reachable: saving the MQTT and module rows on a disabled
     * slot sets them, and a slot disabled elsewhere with no name and no key still holds the id
     * the firmware gave it. The test to keep the two in step is the predicate *being* the
     * field list - a field added to a channel is added here.
     */
    if (channel->role != 1U &&
        (channel->role != 0U || channel->name[0] != '\0' || channel->psk_len != 0U ||
         channel->id != 0U || channel->uplink_enabled || channel->downlink_enabled ||
         channel->position_precision != 0U || channel->is_muted)) {
        item_verb(list, MESH_STR_CHANNELS_CLEAR_ROW, MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL);
    }
}

int mesh_ui_settings_channel_at_row(const struct mesh_ui_settings *settings,
                                    const struct mesh_ui_handshake_state *handshake, uint32_t row) {
    struct mesh_ui_settings_item item;
    if (settings == NULL || !settings->has_channels ||
        !mesh_ui_settings_item(settings, handshake, NULL, 0U, MESH_UI_SETTINGS_CHANNELS,
                               MESH_UI_SETTINGS_NO_CHANNEL, row, &item) ||
        item.kind != INKSTAND_FORM_ACTION) {
        return -1;
    }
    /* The two sharing rows at the foot of the list are ACTION rows too, and carry an
       enum mesh_ui_settings_action rather than a slot. A slot is 0 to 7 and nothing else, which
       is the invariant that keeps one kind of row from being read as the other. */
    return item.number < MESH_UI_MAX_CHANNELS ? (int)item.number : -1;
}

static void build_security(const struct mesh_ui_settings *s, struct item_list *list) {
    item_key(list, MESH_STR_SETTINGS_PUBLIC_KEY_ROW, s->public_key, s->public_key_len);
    item_key_field(list, MESH_UI_FIELD_SECURITY_PRIVATE_KEY, s->private_key, s->private_key_len);
    for (unsigned i = 0; i < 3U; ++i) {
        item_key_field(list, (enum mesh_ui_setting_field)(MESH_UI_FIELD_SECURITY_ADMIN_KEY_0 + i),
                       s->admin_keys[i], s->admin_key_lens[i]);
    }
    item_field(list, MESH_UI_FIELD_SECURITY_SIGNATURE_POLICY, s->packet_signature_policy, NULL);
    item_field(list, MESH_UI_FIELD_SECURITY_MANAGED, s->is_managed ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_SECURITY_ADMIN_CHANNEL, s->admin_channel_enabled ? 1U : 0U,
               NULL);
    item_field(list, MESH_UI_FIELD_SECURITY_SERIAL, s->serial_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_SECURITY_DEBUG_LOG, s->debug_log_api_enabled ? 1U : 0U, NULL);
}

static void build_position(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_POSITION_GPS_MODE, s->gps_mode, NULL);
    item_field(list, MESH_UI_FIELD_POSITION_BROADCAST_SECS, s->position_broadcast_secs, NULL);
    item_field(list, MESH_UI_FIELD_POSITION_SMART, s->position_broadcast_smart_enabled ? 1U : 0U,
               NULL);
    /* Listed whatever the toggle says, so the row count does not move under the cursor while
       smart broadcast is being turned on and off; the same rule the LoRa trio follows. */
    item_field(list, MESH_UI_FIELD_POSITION_SMART_DISTANCE, s->smart_minimum_distance, NULL);
    item_field(list, MESH_UI_FIELD_POSITION_SMART_INTERVAL, s->smart_minimum_interval_secs, NULL);
    item_field(list, MESH_UI_FIELD_POSITION_GPS_INTERVAL, s->gps_update_interval, NULL);

    /*
     * What a position packet carries, which is ten bits of one word on the wire and ten rows
     * here: the settings are "send the fix time" and "send how good the fix was", and nobody
     * has an opinion about 0x0281. Under a heading because they are a set rather than ten
     * separate switches, which is also why the fb backend draws them as checkboxes.
     */
    item_heading(list, MESH_STR_HEAD_POSITION_CARRIES);
    item_flag_group(list, MESH_UI_FIELD_GROUP_POSITION_FLAGS, s->position_flags);

    /*
     * Fixed position. The flag is shown rather than offered: the firmware sets it itself as
     * part of set_fixed_position, and a toggle here could only turn it on with no coordinates
     * behind it - which leaves the radio broadcasting whatever it last had.
     *
     * The three coordinate rows are edited like any other text, but they are not saved with
     * the section: Y writes PositionConfig, and these go out through the action row below
     * them. They are pre-filled with where the radio says it is, so a fix that came from a
     * GPS can be pinned down by opening the section and pressing one row.
     */
    item_toggle(list, MESH_STR_SETTINGS_FIXED_POSITION, s->fixed_position);
    char coord[MESH_UI_SETTINGS_VALUE_MAX];
    mesh_ui_settings_coord_text(s->has_own_position ? s->own_latitude_i : 0, coord, sizeof coord);
    item_field(list, MESH_UI_FIELD_POSITION_LATITUDE, 0U, coord);
    mesh_ui_settings_coord_text(s->has_own_position ? s->own_longitude_i : 0, coord, sizeof coord);
    item_field(list, MESH_UI_FIELD_POSITION_LONGITUDE, 0U, coord);
    snprintf(coord, sizeof coord, "%d", s->has_own_altitude ? (int)s->own_altitude : 0);
    item_field(list, MESH_UI_FIELD_POSITION_ALTITUDE, 0U, coord);
    item_verb(list, MESH_STR_SETTINGS_SET_FIXED_POS, MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION);
    /* Only offered when there is one to clear; the row would otherwise do nothing twice. */
    if (s->fixed_position) {
        item_verb(list, MESH_STR_SETTINGS_CLEAR_FIXED_POS,
                  MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION);
    }
}

static void build_power(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_POWER_SAVING, s->is_power_saving ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_POWER_LS_SECS, s->ls_secs, NULL);
    item_field(list, MESH_UI_FIELD_POWER_MIN_WAKE, s->min_wake_secs, NULL);
    item_field(list, MESH_UI_FIELD_POWER_WAIT_BT, s->wait_bluetooth_secs, NULL);
    item_field(list, MESH_UI_FIELD_POWER_SHUTDOWN, s->on_battery_shutdown_after_secs, NULL);
}

/*
 * The Modules list: every ModuleConfig variant this client keeps, with its enabled state as
 * the value, and A on a row opening that module's own section.
 *
 * ACTION rows carrying the target section in `number`, which is the shape the channel list
 * already uses - the nav intercepts A on them ahead of the radio-action handling rather than
 * either list needing a screen of its own. A module the radio has not sent says so instead of
 * being hidden: "which of these has not arrived" is most of what this screen is for.
 */
/*
 * The radio's own screen (DeviceUIConfig), which is not the Display section: that one is the
 * panel - how long it stays lit, which way up, metric or imperial - and this is the graphical
 * UI drawn on it.
 *
 * Three rows are shown and not offered, under a heading that says who does set them. The two
 * locks are a door this client has no key to: `store_ui_config` can turn them on and there is
 * no verb that turns them off, and the PIN behind them is not on the wire at all. Language is
 * refused for a different reason - see MESH_UI_FIELD_UI_THEME's neighbours - and shown for the
 * same one: it is the answer to "why is my radio in Swedish".
 */
static void build_radio_ui(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_UI_THEME, s->ui_theme, NULL);
    item_field(list, MESH_UI_FIELD_UI_BRIGHTNESS, s->ui_brightness, NULL);
    item_field(list, MESH_UI_FIELD_UI_SCREEN_TIMEOUT, s->ui_screen_timeout, NULL);
    item_field(list, MESH_UI_FIELD_UI_CLOCKFACE, s->ui_clockface_analog ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_UI_COMPASS_MODE, s->ui_compass_mode, NULL);
    item_field(list, MESH_UI_FIELD_UI_GPS_FORMAT, s->ui_gps_format, NULL);
    item_field(list, MESH_UI_FIELD_UI_ALERT, s->ui_alert_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_UI_BANNER, s->ui_banner_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_UI_RING_TONE, s->ui_ring_tone_id, NULL);
    item_heading(list, MESH_STR_HEAD_LOCKS);
    item_text(list, MESH_STR_SETTINGS_UI_LANGUAGE, INKSTAND_FORM_INFO,
              mesh_radio_language_name(s->ui_language));
    item_toggle(list, MESH_STR_SETTINGS_UI_SCREEN_LOCK, s->ui_screen_lock);
    item_toggle(list, MESH_STR_SETTINGS_UI_SETTINGS_LOCK, s->ui_settings_lock);
}

/*
 * The radio's quick replies. Six slots, always all six, empty ones included - the Channels
 * shape, and for the same reason: adding a message is filling an empty slot, and a row count
 * that grew as they were filled would move under the cursor mid-edit.
 *
 * A radio holding more than six says so on a row of its own rather than pretending the rest
 * are gone; the save carries them across untouched. That row appears and disappears with what
 * the radio holds and never with an edit, which is the line the heading rule actually draws.
 */
static void build_canned(const struct mesh_ui_settings *s, struct item_list *list) {
    char entry[MESH_UI_CANNED_SLOT_MAX];
    for (uint32_t i = 0; i < MESH_UI_CANNED_SLOTS; ++i) {
        mesh_ui_settings_canned_entry(s->canned_messages, i, entry, sizeof entry);
        item_field(list, (enum mesh_ui_setting_field)(MESH_UI_FIELD_CANNED_0 + i), 0U, entry);
    }
    const uint32_t held = mesh_ui_settings_canned_count(s->canned_messages);
    if (held > MESH_UI_CANNED_SLOTS) {
        char value[MESH_UI_SETTINGS_VALUE_MAX];
        inkcell_str_format(value, sizeof value, MESH_STR_SETTINGS_CANNED_KEPT_N,
                           (unsigned)(held - MESH_UI_CANNED_SLOTS));
        item_text(list, MESH_STR_SETTINGS_CANNED_KEPT, INKSTAND_FORM_INFO, value);
    }
}

static void build_modules(const struct mesh_ui_settings *s,
                          const struct mesh_ui_handshake_state *hs, struct item_list *list) {
    const uint32_t count = mesh_ui_settings_module_count();
    for (uint32_t i = 0; i < count; ++i) {
        const enum mesh_ui_settings_section section = mesh_ui_settings_module_at(i);
        struct mesh_ui_settings_item *item =
            item_add_named(list, mesh_ui_settings_section_name(section), INKSTAND_FORM_ACTION);
        if (item == NULL) {
            continue;
        }
        item->number = (uint32_t)section;
        /* The one list of items whose rows are subjects rather than settings, so the one that
           fills the leading slot. Set before the "not loaded" branch below: a module the radio has
           not answered for is still that module. */
        item->icon = mesh_ui_settings_section_icon(section);
        const enum mesh_ui_settings_availability state =
            mesh_ui_settings_section_availability(s, hs, section);
        if (state != MESH_UI_SETTINGS_SECTION_READY) {
            /* Why there is nothing to show, in the section list's own words: a module the radio
               has not sent yet, or one its firmware was built without. The second is the reason
               this is a state rather than a bool - it is the row that should stop somebody
               pressing X at it. */
            inkwell_str_copy(item->value, sizeof item->value,
                             inkcell_str(mesh_ui_settings_availability_label(state)));
            continue;
        }
        bool enabled = false;
        switch (section) {
        case MESH_UI_SETTINGS_MQTT:
            enabled = s->mqtt_enabled;
            break;
        case MESH_UI_SETTINGS_STORE_FORWARD:
            enabled = s->store_forward_enabled;
            break;
        case MESH_UI_SETTINGS_TELEMETRY:
            /* Telemetry has no single enabled flag; it is on when it is reporting anything. */
            enabled = s->device_telemetry_enabled || s->environment_measurement_enabled ||
                      s->air_quality_enabled || s->power_measurement_enabled ||
                      s->health_measurement_enabled;
            break;
        case MESH_UI_SETTINGS_NEIGHBOR_INFO:
            enabled = s->neighbor_info_enabled;
            break;
        case MESH_UI_SETTINGS_RANGE_TEST:
            enabled = s->range_test_enabled;
            break;
        case MESH_UI_SETTINGS_PAXCOUNTER:
            enabled = s->paxcounter_enabled;
            break;
        case MESH_UI_SETTINGS_AMBIENT:
            enabled = s->ambient_led_state;
            break;
        case MESH_UI_SETTINGS_STATUS_MESSAGE:
            /* No enabled flag: the module is doing something exactly when there is a status. */
            enabled = s->status_message[0] != '\0';
            break;
        case MESH_UI_SETTINGS_TAK:
            /* Nor here - TAKConfig is two enums with no switch. Unspecifed/Unspecifed is the
               untouched state, which is as close to "off" as this module gets. */
            enabled = s->tak_team != 0U || s->tak_role != 0U;
            break;
        case MESH_UI_SETTINGS_DETECTION:
            enabled = s->detection_enabled;
            break;
        case MESH_UI_SETTINGS_EXT_NOTIFICATION:
            enabled = s->extnotif_enabled;
            break;
        case MESH_UI_SETTINGS_TRAFFIC:
            /* No enabled flag: upstream removed the bool toggles for "non-zero implies
               enabled", so this module is on when any of its five rows is set. */
            enabled = s->traffic_position_min_interval_secs != 0U ||
                      s->traffic_nodeinfo_max_hops != 0U ||
                      s->traffic_rate_limit_window_secs != 0U ||
                      s->traffic_rate_limit_max_packets != 0U ||
                      s->traffic_unknown_packet_threshold != 0U;
            break;
        case MESH_UI_SETTINGS_CANNED:
            /* The one row here with no config behind it at all: the module is doing something
               exactly when the radio has messages to offer, the same test Status message
               answers with. */
            enabled = s->canned_messages[0] != '\0';
            break;
        default:
            break;
        }
        inkwell_str_copy(item->value, sizeof item->value,
                         inkcell_str(enabled ? MESH_STR_COMMON_ON : MESH_STR_COMMON_OFF));
    }
}

static void build_mqtt(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_MQTT_ENABLED, s->mqtt_enabled ? 1U : 0U, NULL);
    /*
     * Second, because it is the second question about the same connection - whether this radio
     * reaches the broker itself or hands it to whatever it is paired with - and every row below
     * is a detail of the first. It sat at the foot of the section, under the map-report heading
     * it has nothing to do with, for as long as it was read-only.
     */
    item_field(list, MESH_UI_FIELD_MQTT_PROXY, s->mqtt_proxy_to_client_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_MQTT_ADDRESS, 0U, s->mqtt_address);
    item_field(list, MESH_UI_FIELD_MQTT_USERNAME, 0U, s->mqtt_username);
    item_field(list, MESH_UI_FIELD_MQTT_PASSWORD, 0U, s->mqtt_password);
    item_field(list, MESH_UI_FIELD_MQTT_ROOT, 0U, s->mqtt_root);
    item_field(list, MESH_UI_FIELD_MQTT_ENCRYPTION, s->mqtt_encryption_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_MQTT_TLS, s->mqtt_tls_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_MQTT_MAP_REPORTING, s->mqtt_map_reporting_enabled ? 1U : 0U,
               NULL);
    /* MapReportSettings, the one submessage in this section. Listed under the toggle that
       decides whether the radio reads them at all, and listed whether or not it is on - the
       heading rule. */
    item_heading(list, MESH_STR_HEAD_MAP_REPORT);
    item_field(list, MESH_UI_FIELD_MQTT_MAP_INTERVAL, s->mqtt_map_publish_interval_secs, NULL);
    item_field(list, MESH_UI_FIELD_MQTT_MAP_PRECISION, s->mqtt_map_position_precision, NULL);
    item_field(list, MESH_UI_FIELD_MQTT_MAP_LOCATION, s->mqtt_map_should_report_location ? 1U : 0U,
               NULL);
}

/*
 * Where a history request has got to, as the value column of a row.
 *
 * One place rather than two because the running states are drawn on the action row - which
 * cannot be pressed while one is running - and the finished ones on the row under it, and a
 * screen that answered the same question twice would be the marker-gutter mistake again.
 */
static void store_forward_progress(const struct mesh_ui_store_forward *sf, char *out,
                                   size_t out_len) {
    switch ((enum mesh_store_forward_state)sf->state) {
    case MESH_STORE_FORWARD_SEEKING:
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_SF_SEEKING));
        return;
    case MESH_STORE_FORWARD_REQUESTED:
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_SF_WAITING));
        return;
    case MESH_STORE_FORWARD_REPLAYING:
        /* The router announces a count and then trickles the messages out, so this is a
           progress reading - and it says "so far" instead when the announcement was the packet
           that went missing, because "4 of 0" is not a reading. */
        if (sf->expected > 0U) {
            inkcell_str_format(out, out_len, MESH_STR_SF_REPLAYING, sf->received, sf->expected);
        } else {
            inkcell_str_format(out, out_len, MESH_STR_SF_REPLAYING_UNKNOWN, sf->received);
        }
        return;
    case MESH_STORE_FORWARD_DONE:
        /* Two numbers, because they are two facts: what the router sent, and what of it was
           new. A client that was off for ten minutes gets a window of four hours back. */
        if (sf->stored > 0U) {
            inkcell_str_format(out, out_len, MESH_STR_SF_ADDED, sf->stored, sf->received);
        } else {
            inkcell_str_format(out, out_len, MESH_STR_SF_NOTHING_NEW, sf->received);
        }
        return;
    case MESH_STORE_FORWARD_EMPTY:
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_SF_NOTHING_MISSED));
        return;
    case MESH_STORE_FORWARD_BUSY:
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_SF_BUSY));
        return;
    case MESH_STORE_FORWARD_NO_ROUTER:
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_SF_NO_ROUTER));
        return;
    case MESH_STORE_FORWARD_TIMEOUT:
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_SF_NO_REPLY));
        return;
    case MESH_STORE_FORWARD_FAILED:
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_SF_FAILED));
        return;
    case MESH_STORE_FORWARD_IDLE:
    default:
        out[0] = '\0';
        return;
    }
}

/*
 * Store & Forward, in two halves under three headings.
 *
 * The first group is the half of the module that is about *this* client: a router somewhere on
 * the mesh has been keeping the traffic that arrived while the Brick was off, and one press
 * asks for it. Everything below it configures the module, and the Server rows under that
 * configure running one - which a handheld with no mains power will never be.
 *
 * That is why the request group is first, and why the rows that were already here gained a
 * heading of their own: on this device the press is what somebody opens the section to do, and
 * the toggles are what they read afterwards to find out why it did not work.
 */
static void build_store_forward(const struct mesh_ui_settings *s,
                                const struct mesh_ui_handshake_state *handshake,
                                struct item_list *list) {
    const struct mesh_ui_store_forward *sf = &s->store_forward;
    const bool connected = handshake != NULL && handshake->link_up;
    const bool running = sf->state == (uint8_t)MESH_STORE_FORWARD_SEEKING ||
                         sf->state == (uint8_t)MESH_STORE_FORWARD_REQUESTED ||
                         sf->state == (uint8_t)MESH_STORE_FORWARD_REPLAYING;

    item_heading(list, MESH_STR_HEAD_MISSED);
    /* Who would answer. Listed before the press rather than only after one, because "none
       heard yet" is the answer to why the press is about to take half a minute - the client
       goes looking with a broadcast ping when it knows no router. */
    if (sf->router == 0U) {
        item_str(list, MESH_STR_SF_ROUTER, INKSTAND_FORM_INFO, MESH_STR_SF_ROUTER_NONE);
    } else {
        struct mesh_ui_settings_item *item = item_add(list, MESH_STR_SF_ROUTER, INKSTAND_FORM_INFO);
        if (item != NULL) {
            if (sf->router_secondary) {
                inkcell_str_format(item->value, sizeof item->value, MESH_STR_SF_ROUTER_SECONDARY,
                                   sf->router_name);
            } else {
                inkwell_str_copy(item->value, sizeof item->value, sf->router_name);
            }
        }
    }

    /*
     * The press, and what it is doing while it is doing it. A running request becomes an INFO
     * row carrying the progress rather than disappearing: the session refuses a second one with
     * -EBUSY anyway, and a row that vanished mid-replay would move every row under it while the
     * user was reading them.
     */
    if (running) {
        struct mesh_ui_settings_item *item =
            item_add(list, MESH_STR_SF_REQUEST, INKSTAND_FORM_INFO);
        if (item != NULL) {
            store_forward_progress(sf, item->value, sizeof item->value);
        }
    } else {
        item_radio_action(list, MESH_STR_SF_REQUEST, MESH_UI_SETTINGS_ACTION_REQUEST_HISTORY,
                          connected);
    }

    /* What the last one did, once there has been one. The only row here that is not listed
       unconditionally - before the first press it would be a heading over a blank value. */
    if (!running && sf->state != (uint8_t)MESH_STORE_FORWARD_IDLE) {
        struct mesh_ui_settings_item *item =
            item_add(list, MESH_STR_SF_LAST_REQUEST, INKSTAND_FORM_INFO);
        if (item != NULL) {
            store_forward_progress(sf, item->value, sizeof item->value);
        }
    }

    /* The router's own storage, when it has volunteered it. Never asked for: this client has no
       row that would act on the difference, and a request costs the mesh a round trip. */
    if (sf->has_stats) {
        struct mesh_ui_settings_item *item =
            item_add(list, MESH_STR_SF_ROUTER_HOLDS, INKSTAND_FORM_INFO);
        if (item != NULL) {
            inkcell_str_format(item->value, sizeof item->value, MESH_STR_SF_HOLDS_COUNT,
                               sf->messages_saved, sf->messages_max);
        }
    }

    item_heading(list, MESH_STR_HEAD_MODULE);
    item_field(list, MESH_UI_FIELD_SF_ENABLED, s->store_forward_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_SF_HEARTBEAT, s->store_forward_heartbeat ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_SF_SERVER, s->store_forward_is_server ? 1U : 0U, NULL);
    /* The three the radio only reads as a server. Shown regardless: a node is set up to be a
       server by filling these in and then turning the row above on. */
    item_heading(list, MESH_STR_HEAD_SERVER);
    item_field(list, MESH_UI_FIELD_SF_RECORDS, s->store_forward_records, NULL);
    item_field(list, MESH_UI_FIELD_SF_HISTORY_MAX, s->store_forward_history_return_max, NULL);
    item_field(list, MESH_UI_FIELD_SF_HISTORY_WINDOW, s->store_forward_history_return_window, NULL);
}

/*
 * Telemetry is five near-identical groups - a toggle, an interval, sometimes a screen flag -
 * and fifteen fields of that in a flat run is unreadable. The headings are what phase 9 added
 * them for, and they are why the rows inside a group are named "Enabled" and "Interval"
 * rather than repeating the group in every label.
 */
static void build_telemetry(const struct mesh_ui_settings *s, struct item_list *list) {
    item_heading(list, MESH_STR_HEAD_DEVICE);
    item_field(list, MESH_UI_FIELD_TELEMETRY_DEVICE, s->device_telemetry_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_INTERVAL, s->device_update_interval, NULL);
    item_heading(list, MESH_STR_HEAD_ENVIRONMENT);
    item_field(list, MESH_UI_FIELD_TELEMETRY_ENVIRONMENT,
               s->environment_measurement_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_ENV_INTERVAL, s->environment_update_interval, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_ENV_SCREEN, s->environment_screen_enabled ? 1U : 0U,
               NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_ENV_FAHRENHEIT,
               s->environment_display_fahrenheit ? 1U : 0U, NULL);
    item_heading(list, MESH_STR_HEAD_AIR_QUALITY);
    item_field(list, MESH_UI_FIELD_TELEMETRY_AIR_QUALITY, s->air_quality_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_AIR_INTERVAL, s->air_quality_interval, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_AIR_SCREEN, s->air_quality_screen_enabled ? 1U : 0U,
               NULL);
    item_heading(list, MESH_STR_HEAD_POWER);
    item_field(list, MESH_UI_FIELD_TELEMETRY_POWER, s->power_measurement_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_POWER_INTERVAL, s->power_update_interval, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_POWER_SCREEN, s->power_screen_enabled ? 1U : 0U, NULL);
    item_heading(list, MESH_STR_HEAD_HEALTH);
    item_field(list, MESH_UI_FIELD_TELEMETRY_HEALTH, s->health_measurement_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_HEALTH_INTERVAL, s->health_update_interval, NULL);
    item_field(list, MESH_UI_FIELD_TELEMETRY_HEALTH_SCREEN, s->health_screen_enabled ? 1U : 0U,
               NULL);
}

static void build_neighbor_info(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_NEIGHBOR_ENABLED, s->neighbor_info_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_NEIGHBOR_INTERVAL, s->neighbor_info_interval, NULL);
    item_field(list, MESH_UI_FIELD_NEIGHBOR_OVER_LORA, s->neighbor_info_over_lora ? 1U : 0U, NULL);
}

/*
 * Range test. The module is two different things depending on one row: with `sender` at 0 it
 * only listens, and with anything else this node transmits to the whole channel on that timer.
 * The heading says so, because a row reading "Send every  30s" does not convey that the traffic
 * lands on everyone else's radio too.
 */
static void build_range_test(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_RANGE_TEST_ENABLED, s->range_test_enabled ? 1U : 0U, NULL);
    item_heading(list, MESH_STR_HEAD_TRANSMITTER);
    item_str(list, MESH_STR_NOTE_TEST_PACKETS, INKSTAND_FORM_INFO,
             MESH_STR_NOTE_TEST_PACKETS_VALUE);
    item_field(list, MESH_UI_FIELD_RANGE_TEST_SENDER, s->range_test_sender, NULL);
    /* ESP32-only in the firmware; shown anyway, because the radio ignoring a flag is quieter
       than the row not being there when a phone app shows it. */
    item_heading(list, MESH_STR_HEAD_LOG_ESP32);
    item_field(list, MESH_UI_FIELD_RANGE_TEST_SAVE, s->range_test_save ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_RANGE_TEST_CLEAR, s->range_test_clear_on_reboot ? 1U : 0U, NULL);
}

static void build_paxcounter(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_PAX_ENABLED, s->paxcounter_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_PAX_INTERVAL, s->paxcounter_interval, NULL);
    /* Signed on the wire and signed in the store; the cast is the row model's, not the value's
       (see k_rssi_presets). A radio that has never had these set reports 0, which is not a
       threshold the module uses - the firmware substitutes -80. */
    item_heading(list, MESH_STR_HEAD_COUNT_ABOVE);
    item_field(list, MESH_UI_FIELD_PAX_WIFI_THRESHOLD,
               (uint32_t)(s->paxcounter_wifi_threshold != 0 ? s->paxcounter_wifi_threshold : -80),
               NULL);
    item_field(list, MESH_UI_FIELD_PAX_BLE_THRESHOLD,
               (uint32_t)(s->paxcounter_ble_threshold != 0 ? s->paxcounter_ble_threshold : -80),
               NULL);
}

static void build_tak(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_TAK_TEAM, s->tak_team, NULL);
    item_field(list, MESH_UI_FIELD_TAK_ROLE, s->tak_role, NULL);
}

static void build_ambient(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_AMBIENT_LED, s->ambient_led_state ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_AMBIENT_CURRENT, s->ambient_current, NULL);
    /* Three channels rather than a colour picker: a d-pad steps numbers well and picks colours
       badly. Listed under a heading so the trio reads as one setting. */
    item_heading(list, MESH_STR_HEAD_COLOUR);
    item_field(list, MESH_UI_FIELD_AMBIENT_RED, s->ambient_red, NULL);
    item_field(list, MESH_UI_FIELD_AMBIENT_GREEN, s->ambient_green, NULL);
    item_field(list, MESH_UI_FIELD_AMBIENT_BLUE, s->ambient_blue, NULL);
}

static void build_status_message(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_STATUS_TEXT, 0U, s->status_message);
}

static void build_detection(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_DETECT_ENABLED, s->detection_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_DETECT_NAME, 0U, s->detection_name);
    item_field(list, MESH_UI_FIELD_DETECT_MIN_BROADCAST, s->detection_minimum_broadcast_secs, NULL);
    item_field(list, MESH_UI_FIELD_DETECT_STATE_BROADCAST, s->detection_state_broadcast_secs, NULL);
    item_field(list, MESH_UI_FIELD_DETECT_SEND_BELL, s->detection_send_bell ? 1U : 0U, NULL);
    /* The pin and how it is read. Which pins a board exposes is its own business and nothing
       on the wire says, so the rows are offered and the heading says whose problem it is. */
    item_heading(list, MESH_STR_HEAD_WIRING);
    item_field(list, MESH_UI_FIELD_DETECT_PIN, s->detection_monitor_pin, NULL);
    item_field(list, MESH_UI_FIELD_DETECT_TRIGGER, s->detection_trigger_type, NULL);
    item_field(list, MESH_UI_FIELD_DETECT_PULLUP, s->detection_use_pullup ? 1U : 0U, NULL);
}

/*
 * External notification: fifteen fields, of which nine are three near-identical groups - a
 * pin, an alert on a message, an alert on a bell, once each for the plain output, the vibra
 * motor and the buzzer. Flat, that is nine rows whose labels differ by one word and which
 * cannot be told apart at a glance; under three headings it is three copies of the same
 * three-row shape. This is the module the heading row was added for.
 */
static void build_ext_notification(const struct mesh_ui_settings *s, struct item_list *list) {
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ENABLED, s->extnotif_enabled ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_OUTPUT_MS, s->extnotif_output_ms, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_NAG, s->extnotif_nag_timeout, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ACTIVE, s->extnotif_active ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_PWM, s->extnotif_use_pwm ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_I2S, s->extnotif_use_i2s_as_buzzer ? 1U : 0U, NULL);
    item_heading(list, MESH_STR_HEAD_OUTPUT);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_PIN, s->extnotif_output, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ALERT_MSG, s->extnotif_alert_message ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ALERT_BELL, s->extnotif_alert_bell ? 1U : 0U, NULL);
    item_heading(list, MESH_STR_HEAD_VIBRA);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_PIN_VIBRA, s->extnotif_output_vibra, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_VIBRA,
               s->extnotif_alert_message_vibra ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_VIBRA,
               s->extnotif_alert_bell_vibra ? 1U : 0U, NULL);
    item_heading(list, MESH_STR_HEAD_BUZZER);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_PIN_BUZZER, s->extnotif_output_buzzer, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_BUZZER,
               s->extnotif_alert_message_buzzer ? 1U : 0U, NULL);
    item_field(list, MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_BUZZER,
               s->extnotif_alert_bell_buzzer ? 1U : 0U, NULL);

    /*
     * The tune the buzzer plays, shown and not offered. RTTTL is a 230-byte string of note
     * lengths and octaves, and a keyboard on a d-pad is not a way to enter one; a preset list
     * would be this client inventing music the firmware does not have. So the row answers "why
     * is it playing that" - which is what it is opened for - and the phone apps stay where a
     * ringtone is changed. `MESH_UI_SETTING_TEXT_MAX` is 80 and this is 231, which is the other
     * half of the same answer. A long tune is cut at the value column, deliberately: the row
     * is here to identify what is playing, not to be the score.
     */
    if (s->has_ringtone) {
        item_heading(list, MESH_STR_HEAD_RINGTONE);
        item_text(list, MESH_STR_SETTINGS_RINGTONE_ROW, INKSTAND_FORM_INFO,
                  s->ringtone[0] != '\0' ? s->ringtone : inkcell_str(MESH_STR_COMMON_NONE));
    }
}

/*
 * Traffic management is the one module with no enabled toggle: upstream removed the bool
 * flags in favour of "a non-zero value implicitly enables it", and reserved their tags so
 * nobody puts them back. Every row here is therefore off at 0, and the section says so rather
 * than leaving a reader to wonder where the switch is.
 */
static void build_traffic(const struct mesh_ui_settings *s, struct item_list *list) {
    item_str(list, MESH_STR_NOTE_EACH_LIMIT, INKSTAND_FORM_INFO, MESH_STR_NOTE_EACH_LIMIT_VALUE);
    item_field(list, MESH_UI_FIELD_TRAFFIC_POSITION_INTERVAL, s->traffic_position_min_interval_secs,
               NULL);
    item_field(list, MESH_UI_FIELD_TRAFFIC_NODEINFO_HOPS, s->traffic_nodeinfo_max_hops, NULL);
    item_heading(list, MESH_STR_HEAD_RATE_LIMIT);
    item_field(list, MESH_UI_FIELD_TRAFFIC_RATE_WINDOW, s->traffic_rate_limit_window_secs, NULL);
    item_field(list, MESH_UI_FIELD_TRAFFIC_RATE_PACKETS, s->traffic_rate_limit_max_packets, NULL);
    item_field(list, MESH_UI_FIELD_TRAFFIC_UNKNOWN_THRESHOLD, s->traffic_unknown_packet_threshold,
               NULL);
}

/*
 * Mesh beacon: what this node tells strangers about the mesh it is on.
 *
 * Four groups, and the order is what a beacon *is*, read outwards. The three flags say whether
 * it listens, broadcasts, or splits the broadcast in two for older firmware. `Broadcast` then
 * says how often and with what text. `Offered channel` is the invitation the beacon carries -
 * a name and a key, which is a whole ChannelSettings on the wire and so the same two rows the
 * Channels section draws. `Target n` is where each copy goes out.
 *
 * The four targets are inline groups rather than a list one level down, which is where the
 * roadmap expected them. Four records of three rows is sixteen rows, which the section has room
 * for; a list would have cost a third level of nav under a section that is already one down
 * from Modules, for a screen with at most four rows on it. It is the shape External
 * notification's three output groups already have, and the repetition is a table entry rather
 * than four copies - see item_record_group().
 */
static void build_beacon(const struct mesh_ui_settings *s, struct item_list *list) {
    item_flag_group(list, MESH_UI_FIELD_GROUP_BEACON_FLAGS, s->beacon_flags);

    item_heading(list, MESH_STR_HEAD_BEACON_BROADCAST);
    item_field(list, MESH_UI_FIELD_BEACON_INTERVAL, s->beacon_interval_secs, NULL);
    item_field(list, MESH_UI_FIELD_BEACON_MESSAGE, 0U, s->beacon_message);

    item_heading(list, MESH_STR_HEAD_BEACON_OFFER);
    item_field(list, MESH_UI_FIELD_BEACON_OFFER_NAME, 0U, s->beacon_offer_name);
    item_key_field(list, MESH_UI_FIELD_BEACON_OFFER_KEY, s->beacon_offer_psk,
                   s->beacon_offer_psk_len);
    struct mesh_ui_settings_item *offer_region =
        item_field(list, MESH_UI_FIELD_BEACON_OFFER_REGION, s->beacon_offer_region, NULL);
    /*
     * The offered pair is the LoRa pair, advertised rather than run: a stranger who acts on it
     * sets their radio to exactly this region and preset, so a combination the firmware's own
     * map calls illegal is an invitation nobody can accept. An offer naming no region names no
     * constraint either - 0 here is "not offered" rather than "whatever is running".
     */
    constrain_preset_row(
        s, offer_region != NULL ? offer_region->number : s->beacon_offer_region,
        item_field(list, MESH_UI_FIELD_BEACON_OFFER_PRESET, s->beacon_offer_preset, NULL), 1U);

    /*
     * What a target record holds, in the order the group is walked in. Named because the layout
     * is read twice below - once to flatten the values in and once to find the region a preset
     * has to be legal in - and a record whose two readings disagreed would constrain the wrong
     * row while showing the right one.
     */
    enum {
        TARGET_PRESET = 0U,
        TARGET_REGION,
        TARGET_CHANNEL,
    };
    uint32_t targets[MESH_UI_BEACON_TARGETS * MESH_UI_BEACON_TARGET_FIELDS];
    struct mesh_ui_settings_item *rows[MESH_UI_BEACON_TARGETS * MESH_UI_BEACON_TARGET_FIELDS] = {
        NULL};
    for (uint32_t i = 0; i < MESH_UI_BEACON_TARGETS; ++i) {
        uint32_t *record = &targets[i * MESH_UI_BEACON_TARGET_FIELDS];
        record[TARGET_PRESET] = s->beacon_targets[i].preset;
        record[TARGET_REGION] = s->beacon_targets[i].region;
        record[TARGET_CHANNEL] = s->beacon_targets[i].channel;
    }
    item_record_group(list, MESH_UI_FIELD_GROUP_BEACON_TARGETS, MESH_UI_BEACON_TARGET_FIELDS,
                      MESH_STR_HEAD_BEACON_TARGET, targets, INKWELL_ARRAY_LEN(targets), rows);
    /*
     * And each target's own pair, which the radio really does switch to for the length of one
     * transmission - so an illegal combination here is this node transmitting where it may not.
     * A target naming no region falls back to the running config's, which is what the firmware
     * does with it, so that is the region its preset has to be legal in.
     */
    for (uint32_t i = 0; i < MESH_UI_BEACON_TARGETS; ++i) {
        struct mesh_ui_settings_item *const *record = &rows[i * MESH_UI_BEACON_TARGET_FIELDS];
        const struct mesh_ui_settings_item *region_row = record[TARGET_REGION];
        const uint32_t region =
            region_row != NULL && region_row->number != 0U ? region_row->number : s->region;
        constrain_preset_row(s, region, record[TARGET_PRESET], 1U);
    }
}

/*
 * Radio actions: the rows that make the radio do something rather than keep something. None of
 * them is a setting, so there is no Y to press and nothing to read back - A on a row opens the
 * confirm overlay and the answer goes out on its own.
 *
 * Ordered least to most destructive, so a cursor arriving at the top of the list is on the one
 * press here that costs nothing but a reconnect, and the two that cannot be undone are the
 * furthest to travel to.
 *
 * The two forget rows are the exception to the section's own name: they drop this client's
 * cached roster and send nothing. They live here because a NodeDB reset leaves that roster
 * standing on purpose - the radio holds 80 entries and evicts, so ours is often the only copy
 * - and this is the screen somebody who has just reset the radio is looking at while wondering
 * why the Nodes tab still says 81. Each says how many it would drop, so the press is not a
 * guess, and a row with nothing to drop is an INFO row rather than a press that does nothing.
 *
 * Which is what the four headings are for. Seven bare verbs in a column give no clue that one
 * of them empties the radio's database and the next two empty ours - the distinction that
 * makes the Status screen say 2 nodes while the Nodes tab says 81. "Nodes on the radio" and
 * "Nodes cached here" say whose is whose before the press rather than in the confirm text
 * after it, and they read as a pair because the rows under them are one.
 *
 * The labels stop at the noun the value column supplies - the label column is 20 cells and
 * "Forget off-radio nodes" is 22 - so the row reads across as one sentence rather than as a
 * clipped one: "Forget off-radio > 7 nodes".
 */
/*
 * The verbs done *to* a radio, a group at a time.
 *
 * Groups rather than one list because two pages and a section share them and take different
 * ones: the Radio tab's details page ends with power, backup and the factory resets; its node
 * lists page is the node database and the two forget rows; and Settings' Radio actions, which
 * exists only while another node is being administered, is everything that reaches the radio.
 * Each group is written once, so a withdrawn Reboot looks the same wherever it is drawn.
 *
 * `connected` asks whether the session can *send*, not whether we know our own node number.
 * The two were the same question only while a drop cleared has_my_info; spelled that way now,
 * every row here would stay pressable over a dead link and fail with -ENOTCONN after the
 * confirm dialog - and would already have done so on a cold start with a restored roster,
 * because the handshake is persisted. With no link a row says so rather than disappearing: a
 * page whose length changes when the radio drops moves the cursor out from under the user, and
 * "not connected" is the answer they were about to press A to find out.
 */
static bool actions_connected(const struct mesh_ui_handshake_state *handshake) {
    return handshake != NULL && handshake->link_up;
}

static void build_power_verbs(const struct mesh_ui_settings *s, bool connected,
                              struct item_list *list) {
    item_heading(list, MESH_STR_HEAD_POWER);
    item_radio_action(list, MESH_STR_ACTION_REBOOT, MESH_UI_SETTINGS_ACTION_REBOOT, connected);
    /* DeviceMetadata says whether the hardware can cut its own power; on a board that cannot,
       the request is simply ignored, so the row says so rather than lying about what A does.
       Until the metadata arrives the row is offered: the radio is the authority, not us. */
    if (s->has_metadata && !s->can_shutdown) {
        item_action_off(list, MESH_STR_ACTION_SHUTDOWN, MESH_STR_ACTION_SHUTDOWN_UNSUPPORTED,
                        MESH_UI_SETTINGS_ACTION_SHUTDOWN);
    } else {
        item_radio_action(list, MESH_STR_ACTION_SHUTDOWN, MESH_UI_SETTINGS_ACTION_SHUTDOWN,
                          connected);
    }
}

static void build_nodedb_verbs(bool connected, struct item_list *list) {
    item_heading(list, MESH_STR_HEAD_NODES_RADIO);
    item_radio_action(list, MESH_STR_ACTION_RESET_NODEDB, MESH_UI_SETTINGS_ACTION_RESET_NODEDB,
                      connected);
}

/* Not radio verbs at all: they drop this client's own roster, which a NodeDB reset deliberately
   leaves standing, so they send nothing and work with no link. Both numbers are what the press
   would remove, not what is cached or stale: a forget keeps our own record and every pin, so a
   roster of eighty nodes that are all pinned has nothing to drop and both rows say so. */
static void build_forget_verbs(const struct mesh_ui_handshake_state *handshake,
                               struct item_list *list) {
    item_heading(list, MESH_STR_HEAD_NODES_CACHED);
    forget_row(list, MESH_STR_ACTION_FORGET_OFF_RADIO,
               handshake != NULL ? handshake->nodes_forgettable_off_radio : 0U,
               MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES);
    forget_row(list, MESH_STR_ACTION_FORGET_ALL,
               handshake != NULL ? handshake->nodes_forgettable_all : 0U,
               MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES);
}

/* Backup before the factory resets, which is the order every list of these runs in: least to
   most destructive, and a backup is the thing you want to have pressed before the row below. */
static void build_backup_verbs(bool connected, struct item_list *list) {
    item_heading(list, MESH_STR_HEAD_BACKUP);
    item_radio_action(list, MESH_STR_ACTION_BACKUP_CONFIG, MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG,
                      connected);
    item_radio_action(list, MESH_STR_ACTION_RESTORE_CONFIG, MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG,
                      connected);
    item_radio_action(list, MESH_STR_ACTION_REMOVE_BACKUP, MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP,
                      connected);
    item_heading(list, MESH_STR_HEAD_FACTORY_RESET);
    item_radio_action(list, MESH_STR_ACTION_FACTORY_CONFIG,
                      MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG, connected);
    item_radio_action(list, MESH_STR_ACTION_FACTORY_DEVICE,
                      MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE, connected);
}

/* Whose radio the Settings tab is pointed at, when it is not this one - the statement every
   remote-facing list opens with. */
static void build_remote_head(const struct mesh_ui_settings *s, struct item_list *list) {
    item_heading(list, MESH_STR_RADIO_ADMIN_REMOTE_HEAD);
    item_text(list, MESH_STR_RADIO_ADMIN_REMOTE_NODE, INKSTAND_FORM_INFO, s->admin_dest_name);
}

/*
 * Settings' Radio actions: the verbs for a node administered over the mesh.
 *
 * Listed only while there is one (mesh_ui_settings_root_at()), which is the only time getting
 * "whose radio" wrong costs something: with a remote target, "Reboot" takes somebody else's
 * repeater down. So the target is stated first, the way About radio states it - the line the
 * reader passes on the way to the press. The forget rows are not here: they are about this
 * client's roster whatever the tab is pointed at, and live with the node lists on the Radio tab.
 */
static void build_actions(const struct mesh_ui_settings *s,
                          const struct mesh_ui_handshake_state *handshake, struct item_list *list) {
    const bool connected = actions_connected(handshake);
    if (s->admin_dest != 0U) {
        build_remote_head(s, list);
    }
    build_power_verbs(s, connected, list);
    build_nodedb_verbs(connected, list);
    build_backup_verbs(connected, list);
}

/*
 * The Radio tab's details page: About radio, and under it what can be done to the radio.
 *
 * Always the radio on the link. While the Settings tab is administering somebody else's node
 * the settings behind these rows are *that* node's, so the page says whose they are and offers
 * the way back instead of drawing another radio's firmware under this tab's name - and instead
 * of a Reboot that would reach across the mesh. The remote node's own facts and verbs are on the
 * Settings tab for as long as it is the target.
 */
static void build_radio_details(const struct mesh_ui_settings *s,
                                const struct mesh_ui_handshake_state *handshake,
                                struct item_list *list) {
    if (s->admin_dest != 0U) {
        build_remote_head(s, list);
        item_verb(list, MESH_STR_RADIO_ADMIN_REMOTE_RETURN, MESH_UI_SETTINGS_ACTION_ADMIN_LOCAL);
        return;
    }
    const bool connected = actions_connected(handshake);
    build_radio(s, handshake, list);
    build_power_verbs(s, connected, list);
    build_backup_verbs(connected, list);
}

/*
 * The Mesh card's node lists: the radio's database, and this client's longer roster.
 *
 * The reset is the radio's verb, so it is offered only for the radio on the link - while
 * another node is administered it would empty *that* node's database, and Settings' Radio
 * actions is where that press is made. The forget rows are always here: they touch nothing but
 * this client.
 */
static void build_node_lists(const struct mesh_ui_settings *s,
                             const struct mesh_ui_handshake_state *handshake,
                             struct item_list *list) {
    if (s->admin_dest == 0U) {
        build_nodedb_verbs(actions_connected(handshake), list);
    }
    build_forget_verbs(handshake, list);
}

static void build_section(const struct mesh_ui_settings *settings,
                          const struct mesh_ui_handshake_state *handshake,
                          const struct mesh_ui_setting_edit *edits, size_t edit_count,
                          enum mesh_ui_settings_section section, uint8_t channel,
                          struct item_list *list) {
    memset(list, 0, sizeof *list);
    list->edits = edits;
    list->edit_count = edits != NULL ? edit_count : 0U;
    if (settings == NULL || !mesh_ui_settings_section_loaded(settings, handshake, section)) {
        return;
    }
    const struct mesh_ui_setting_edit *units_edit =
        mesh_ui_settings_find_edit(list->edits, list->edit_count, MESH_UI_FIELD_DISPLAY_UNITS);
    list->imperial =
        mesh_ui_units_imperial(units_edit != NULL ? (uint8_t)units_edit->number : settings->units);
    switch (section) {
    case MESH_UI_SETTINGS_ABOUT:
        build_about(settings, list);
        break;
    case MESH_UI_SETTINGS_RADIO:
        build_radio(settings, handshake, list);
        break;
    case MESH_UI_SETTINGS_RADIO_UI:
        build_radio_ui(settings, list);
        break;
    case MESH_UI_SETTINGS_CANNED:
        build_canned(settings, list);
        break;
    case MESH_UI_SETTINGS_USER:
        build_user(settings, list);
        break;
    case MESH_UI_SETTINGS_DEVICE:
        build_device(settings, list);
        break;
    case MESH_UI_SETTINGS_DISPLAY:
        build_display(settings, list);
        break;
    case MESH_UI_SETTINGS_LORA:
        build_lora(settings, list);
        break;
    case MESH_UI_SETTINGS_BLUETOOTH:
        build_bluetooth(settings, list);
        break;
    case MESH_UI_SETTINGS_NETWORK:
        build_network(settings, list);
        break;
    case MESH_UI_SETTINGS_CHANNELS:
        if (channel != MESH_UI_SETTINGS_NO_CHANNEL) {
            build_channel(settings, channel, list);
        } else {
            build_channels(settings, handshake, list);
        }
        break;
    case MESH_UI_SETTINGS_SECURITY:
        build_security(settings, list);
        break;
    case MESH_UI_SETTINGS_POSITION:
        build_position(settings, list);
        break;
    case MESH_UI_SETTINGS_POWER:
        build_power(settings, list);
        break;
    case MESH_UI_SETTINGS_MQTT:
        build_mqtt(settings, list);
        break;
    case MESH_UI_SETTINGS_STORE_FORWARD:
        build_store_forward(settings, handshake, list);
        break;
    case MESH_UI_SETTINGS_TELEMETRY:
        build_telemetry(settings, list);
        break;
    case MESH_UI_SETTINGS_RADIO_DETAILS:
        build_radio_details(settings, handshake, list);
        break;
    case MESH_UI_SETTINGS_NODE_LISTS:
        build_node_lists(settings, handshake, list);
        break;
    case MESH_UI_SETTINGS_ACTIONS:
        build_actions(settings, handshake, list);
        break;
    case MESH_UI_SETTINGS_MODULES:
        build_modules(settings, handshake, list);
        break;
    case MESH_UI_SETTINGS_NEIGHBOR_INFO:
        build_neighbor_info(settings, list);
        break;
    case MESH_UI_SETTINGS_RANGE_TEST:
        build_range_test(settings, list);
        break;
    case MESH_UI_SETTINGS_PAXCOUNTER:
        build_paxcounter(settings, list);
        break;
    case MESH_UI_SETTINGS_TAK:
        build_tak(settings, list);
        break;
    case MESH_UI_SETTINGS_AMBIENT:
        build_ambient(settings, list);
        break;
    case MESH_UI_SETTINGS_STATUS_MESSAGE:
        build_status_message(settings, list);
        break;
    case MESH_UI_SETTINGS_DETECTION:
        build_detection(settings, list);
        break;
    case MESH_UI_SETTINGS_EXT_NOTIFICATION:
        build_ext_notification(settings, list);
        break;
    case MESH_UI_SETTINGS_TRAFFIC:
        build_traffic(settings, list);
        break;
    case MESH_UI_SETTINGS_BEACON:
        build_beacon(settings, list);
        break;
    default:
        break;
    }
}

/*
 * Whether anything in this section is a press rather than a value.
 *
 * Built rather than tabulated, because it is not a property of the section: About radio grows
 * its install press only once a check has found something, and Radio actions is nothing but
 * presses. The bar asks so it can name A exactly where A does something - the same reason the
 * help keycap asks mesh_ui_help_offered() rather than testing the nav itself.
 */
bool mesh_ui_settings_section_has_verbs(const struct mesh_ui_settings *settings,
                                        const struct mesh_ui_handshake_state *handshake,
                                        enum mesh_ui_settings_section section, uint8_t channel) {
    struct item_list list;
    build_section(settings, handshake, NULL, 0U, section, channel, &list);
    for (uint32_t i = 0; i < list.count; ++i) {
        if (list.items[i].kind == INKSTAND_FORM_ACTION) {
            return true;
        }
    }
    return false;
}

uint32_t mesh_ui_settings_item_count(const struct mesh_ui_settings *settings,
                                     const struct mesh_ui_handshake_state *handshake,
                                     enum mesh_ui_settings_section section, uint8_t channel) {
    struct item_list list;
    build_section(settings, handshake, NULL, 0U, section, channel, &list);
    return list.count;
}

uint32_t mesh_ui_settings_items(const struct mesh_ui_settings *settings,
                                const struct mesh_ui_handshake_state *handshake,
                                const struct mesh_ui_setting_edit *edits, size_t edit_count,
                                enum mesh_ui_settings_section section, uint8_t channel,
                                struct mesh_ui_settings_item *out, uint32_t max) {
    if (out == NULL || max == 0U) {
        return 0U;
    }
    struct item_list list;
    build_section(settings, handshake, edits, edit_count, section, channel, &list);
    const uint32_t count = list.count < max ? list.count : max;
    memcpy(out, list.items, (size_t)count * sizeof *out);
    return count;
}

bool mesh_ui_settings_item(const struct mesh_ui_settings *settings,
                           const struct mesh_ui_handshake_state *handshake,
                           const struct mesh_ui_setting_edit *edits, size_t edit_count,
                           enum mesh_ui_settings_section section, uint8_t channel, uint32_t row,
                           struct mesh_ui_settings_item *out) {
    if (out == NULL) {
        return false;
    }
    struct item_list list;
    build_section(settings, handshake, edits, edit_count, section, channel, &list);
    if (row >= list.count) {
        memset(out, 0, sizeof *out);
        return false;
    }
    *out = list.items[row];
    return true;
}
