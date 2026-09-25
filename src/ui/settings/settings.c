#define _POSIX_C_SOURCE 200809L

/*
 * What each setting is: the field table, and everything derived from it.
 *
 * One designated-initialiser table (k_fields) is the single description of every editable field -
 * its label, its section, how it steps, what its values are called. Adding a setting is adding a
 * row there plus a case in src/app/app_settings.c; nothing else in the client should be
 * switching on a field id.
 */

#include "inkcell/ui/anim.h"
#include "inkwell/base/array.h"
#include "inkwell/base/text.h"

#include "settings_internal.h"

#include "mesh/core/radio_settings.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "mesh/ui/preferences.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/units.h"

#include "mesh/core/radio_settings.h"
#include "mesh/core/updater.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * What each section is called, in the enum's own order.
 *
 * A table rather than the switch this was, and beside the icons and the notes for the reason
 * given there: it is a lookup with no cases in it. The switch also had to answer twice - once
 * as text for a heading, once as an id for the help topic's subject - and a switch answering
 * the same question two ways is the two-opinions bug this layer keeps designing out.
 */
static const inkcell_str_id k_section_labels[MESH_UI_SETTINGS_SECTION_COUNT] = {
    [MESH_UI_SETTINGS_ABOUT] = MESH_STR_SETTINGS_SECTION_ABOUT,
    [MESH_UI_SETTINGS_RADIO] = MESH_STR_SETTINGS_SECTION_RADIO,
    [MESH_UI_SETTINGS_USER] = MESH_STR_SETTINGS_SECTION_USER,
    [MESH_UI_SETTINGS_DEVICE] = MESH_STR_SETTINGS_SECTION_DEVICE,
    [MESH_UI_SETTINGS_DISPLAY] = MESH_STR_SETTINGS_SECTION_DISPLAY,
    [MESH_UI_SETTINGS_LORA] = MESH_STR_SETTINGS_SECTION_LORA,
    [MESH_UI_SETTINGS_BLUETOOTH] = MESH_STR_SETTINGS_SECTION_BLUETOOTH,
    [MESH_UI_SETTINGS_CHANNELS] = MESH_STR_SETTINGS_SECTION_CHANNELS,
    [MESH_UI_SETTINGS_SECURITY] = MESH_STR_SETTINGS_SECTION_SECURITY,
    [MESH_UI_SETTINGS_POSITION] = MESH_STR_SETTINGS_SECTION_POSITION,
    [MESH_UI_SETTINGS_POWER] = MESH_STR_SETTINGS_SECTION_POWER,
    [MESH_UI_SETTINGS_MQTT] = MESH_STR_SETTINGS_SECTION_MQTT,
    [MESH_UI_SETTINGS_STORE_FORWARD] = MESH_STR_SETTINGS_SECTION_STORE_FORWARD,
    [MESH_UI_SETTINGS_TELEMETRY] = MESH_STR_SETTINGS_SECTION_TELEMETRY,
    [MESH_UI_SETTINGS_ACTIONS] = MESH_STR_SETTINGS_SECTION_ACTIONS,
    [MESH_UI_SETTINGS_MODULES] = MESH_STR_SETTINGS_SECTION_MODULES,
    [MESH_UI_SETTINGS_NEIGHBOR_INFO] = MESH_STR_SETTINGS_SECTION_NEIGHBOR_INFO,
    [MESH_UI_SETTINGS_RANGE_TEST] = MESH_STR_SETTINGS_SECTION_RANGE_TEST,
    [MESH_UI_SETTINGS_PAXCOUNTER] = MESH_STR_SETTINGS_SECTION_PAXCOUNTER,
    [MESH_UI_SETTINGS_TAK] = MESH_STR_SETTINGS_SECTION_TAK,
    [MESH_UI_SETTINGS_AMBIENT] = MESH_STR_SETTINGS_SECTION_AMBIENT,
    [MESH_UI_SETTINGS_STATUS_MESSAGE] = MESH_STR_SETTINGS_SECTION_STATUS_MESSAGE,
    [MESH_UI_SETTINGS_DETECTION] = MESH_STR_SETTINGS_SECTION_DETECTION,
    [MESH_UI_SETTINGS_EXT_NOTIFICATION] = MESH_STR_SETTINGS_SECTION_EXT_NOTIFICATION,
    [MESH_UI_SETTINGS_TRAFFIC] = MESH_STR_SETTINGS_SECTION_TRAFFIC,
    [MESH_UI_SETTINGS_RADIO_UI] = MESH_STR_SETTINGS_SECTION_RADIO_UI,
    [MESH_UI_SETTINGS_CANNED] = MESH_STR_SETTINGS_SECTION_CANNED,
    [MESH_UI_SETTINGS_NETWORK] = MESH_STR_SETTINGS_SECTION_NETWORK,
    [MESH_UI_SETTINGS_BEACON] = MESH_STR_SETTINGS_SECTION_BEACON,
    [MESH_UI_SETTINGS_RADIO_DETAILS] = MESH_STR_SETTINGS_SECTION_RADIO_DETAILS,
    [MESH_UI_SETTINGS_NODE_LISTS] = MESH_STR_SETTINGS_SECTION_NODE_LISTS,
};

inkcell_str_id mesh_ui_settings_section_label(enum mesh_ui_settings_section section) {
    return section < MESH_UI_SETTINGS_SECTION_COUNT ? k_section_labels[section] : INKCELL_STR_NONE;
}

const char *mesh_ui_settings_section_name(enum mesh_ui_settings_section section) {
    const inkcell_str_id label = mesh_ui_settings_section_label(section);
    /* A section past the end still has to render as something: the "?" every unnameable value
       in this client draws, rather than the empty string INKCELL_STR_NONE would hand back. */
    return inkcell_str(label != INKCELL_STR_NONE ? label : INKCELL_STR_COMMON_UNKNOWN_SHORT);
}

/*
 * What each section is about, in one line each, in the enum's own order.
 *
 * A table rather than a switch because it is a lookup with no cases in it, and because a
 * section added without an icon then comes out as INKCELL_ICON_NONE - which draws nothing and
 * leaves the row where it was, rather than failing to compile in a file that has nothing to do
 * with icons.
 */
static const enum inkcell_icon k_section_icons[MESH_UI_SETTINGS_SECTION_COUNT] = {
    [MESH_UI_SETTINGS_ABOUT] = INKCELL_ICON_ABOUT,
    /* Facts about the radio, which is what the Status tab's Radio card holds - the same
       sentence, so the same icon. */
    [MESH_UI_SETTINGS_RADIO] = INKCELL_ICON_RADIO,
    [MESH_UI_SETTINGS_USER] = INKCELL_ICON_USER,
    [MESH_UI_SETTINGS_DEVICE] = INKCELL_ICON_DEVICE,
    [MESH_UI_SETTINGS_DISPLAY] = INKCELL_ICON_DISPLAY,
    [MESH_UI_SETTINGS_LORA] = INKCELL_ICON_LORA,
    [MESH_UI_SETTINGS_BLUETOOTH] = INKCELL_ICON_BLUETOOTH,
    [MESH_UI_SETTINGS_CHANNELS] = INKCELL_ICON_CHANNEL,
    [MESH_UI_SETTINGS_SECURITY] = INKCELL_ICON_SECURITY,
    [MESH_UI_SETTINGS_POSITION] = INKCELL_ICON_POSITION,
    [MESH_UI_SETTINGS_POWER] = INKCELL_ICON_POWER,
    [MESH_UI_SETTINGS_MQTT] = INKCELL_ICON_MQTT,
    [MESH_UI_SETTINGS_STORE_FORWARD] = INKCELL_ICON_STORE_FWD,
    [MESH_UI_SETTINGS_TELEMETRY] = INKCELL_ICON_TELEMETRY,
    [MESH_UI_SETTINGS_ACTIONS] = INKCELL_ICON_ACTIONS,
    /* The Radio tab's two pages wear the icons of the cards that open them. */
    [MESH_UI_SETTINGS_RADIO_DETAILS] = INKCELL_ICON_RADIO,
    [MESH_UI_SETTINGS_NODE_LISTS] = INKCELL_ICON_NODES,
    [MESH_UI_SETTINGS_MODULES] = INKCELL_ICON_MODULES,
    [MESH_UI_SETTINGS_NEIGHBOR_INFO] = INKCELL_ICON_NEIGHBORS,
    [MESH_UI_SETTINGS_RANGE_TEST] = INKCELL_ICON_RANGE_TEST,
    [MESH_UI_SETTINGS_PAXCOUNTER] = INKCELL_ICON_PAXCOUNTER,
    [MESH_UI_SETTINGS_TAK] = INKCELL_ICON_TAK,
    [MESH_UI_SETTINGS_AMBIENT] = INKCELL_ICON_AMBIENT,
    [MESH_UI_SETTINGS_STATUS_MESSAGE] = INKCELL_ICON_STATUS_MSG,
    [MESH_UI_SETTINGS_DETECTION] = INKCELL_ICON_DETECTION,
    [MESH_UI_SETTINGS_EXT_NOTIFICATION] = INKCELL_ICON_EXT_NOTIFY,
    [MESH_UI_SETTINGS_TRAFFIC] = INKCELL_ICON_TRAFFIC,
    /* Two more sections answering with an icon another part of the UI owns, for the reason the
       three above do: Radio UI *is* the radio's screen, which is what DISPLAY says, and a
       canned message is a quick reply, which is what REPLY says. */
    [MESH_UI_SETTINGS_RADIO_UI] = INKCELL_ICON_DISPLAY,
    [MESH_UI_SETTINGS_CANNED] = INKCELL_ICON_REPLY,
    [MESH_UI_SETTINGS_NETWORK] = INKCELL_ICON_NETWORK,
    /* A third section answering with an icon another part of the UI owns, by the rule the two
       above state: a beacon is a broadcast, which is the one thing that glyph says anywhere in
       this client. */
    [MESH_UI_SETTINGS_BEACON] = INKCELL_ICON_BROADCAST,
};

enum inkcell_icon mesh_ui_settings_section_icon(enum mesh_ui_settings_section section) {
    return section < MESH_UI_SETTINGS_SECTION_COUNT ? k_section_icons[section] : INKCELL_ICON_NONE;
}

/*
 * What each verb is, in the enum's own order - the leading slot's entry for an action row.
 *
 * Several rows share a symbol on purpose and the header says why: the two factory resets are
 * one job done to two depths, and the four share/import rows are two jobs done to a channel set
 * and to a contact. What a reader has to tell apart is the *label*; the symbol is what gets the
 * eye to the right row of the card first.
 *
 * INKCELL_ICON_NONE is a legible answer here rather than a hole - the disc draws empty and the
 * row keeps its column - which is the same bargain k_section_icons[] makes, and the reason both
 * are tables rather than switches.
 */
static const enum inkcell_icon k_action_icons[MESH_UI_SETTINGS_ACTION_COUNT] = {
    /* About: this client's own update, and the two rows that change how it looks and reads. */
    [MESH_UI_SETTINGS_ACTION_CHECK_UPDATE] = INKCELL_ICON_REFRESH,
    [MESH_UI_SETTINGS_ACTION_INSTALL_UPDATE] = INKCELL_ICON_DOWNLOAD,
    [MESH_UI_SETTINGS_ACTION_CYCLE_UPDATE_CHANNEL] = INKCELL_ICON_SWAP,
    [MESH_UI_SETTINGS_ACTION_TOGGLE_DEV_UPDATES] = INKCELL_ICON_SWAP,
    [MESH_UI_SETTINGS_ACTION_CYCLE_THEME] = INKCELL_ICON_THEME,
    [MESH_UI_SETTINGS_ACTION_CYCLE_LANGUAGE] = INKCELL_ICON_LANGUAGE,
    [MESH_UI_SETTINGS_ACTION_CYCLE_TEXT_SIZE] = INKCELL_ICON_DISPLAY,
    [MESH_UI_SETTINGS_ACTION_DISCARD_CRASH_REPORT] = INKCELL_ICON_DELETE,

    /* Radio actions, in the order the section runs them: least to most destructive. */
    [MESH_UI_SETTINGS_ACTION_REBOOT] = INKCELL_ICON_RESTART,
    [MESH_UI_SETTINGS_ACTION_SHUTDOWN] = INKCELL_ICON_SHUTDOWN,
    /* The radio's own database, and this client's cache of it. Three rows, one symbol, because
       all three are the same sentence about three stores - which is exactly what makes the
       group readable as a group. */
    [MESH_UI_SETTINGS_ACTION_RESET_NODEDB] = INKCELL_ICON_DELETE,
    [MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES] = INKCELL_ICON_DELETE,
    [MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES] = INKCELL_ICON_DELETE,
    [MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG] = INKCELL_ICON_FACTORY,
    [MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE] = INKCELL_ICON_FACTORY,
    [MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG] = INKCELL_ICON_BACKUP,
    [MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG] = INKCELL_ICON_RESTORE,
    [MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP] = INKCELL_ICON_DELETE,

    /* Position: the pin this radio is pinned to, and taking it off again. */
    [MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION] = INKCELL_ICON_POSITION,
    [MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION] = INKCELL_ICON_CLOSE,
    /* LoRa: a claim about the operator rather than about the hardware. */
    [MESH_UI_SETTINGS_ACTION_SET_HAM_MODE] = INKCELL_ICON_LICENSE,
    /* Store & Forward: what arrived while this client was away. */
    [MESH_UI_SETTINGS_ACTION_REQUEST_HISTORY] = INKCELL_ICON_HISTORY,

    /* About radio: the *other* binary. The check and the channel step are About's own pair one
       subject over, so they answer with the same two symbols; the install says which bus it is
       going over, because that is the whole reason there are two of it. */
    [MESH_UI_SETTINGS_ACTION_CHECK_RADIO_FIRMWARE] = INKCELL_ICON_REFRESH,
    [MESH_UI_SETTINGS_ACTION_CYCLE_FIRMWARE_CHANNEL] = INKCELL_ICON_SWAP,
    [MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB] = INKCELL_ICON_USB,
    [MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE] = INKCELL_ICON_BLUETOOTH,

    /* The two link pairs: a channel set and a contact, each going out as a code and coming back
       typed in. */
    [MESH_UI_SETTINGS_ACTION_SHARE_CHANNELS] = INKCELL_ICON_SHARE,
    [MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS] = INKCELL_ICON_IMPORT,
    /* Emptying a slot, which is the same sentence about a store that the node rows wear this
       symbol for - one channel rather than a database of them. */
    [MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL] = INKCELL_ICON_DELETE,
    [MESH_UI_SETTINGS_ACTION_SHARE_CONTACT] = INKCELL_ICON_SHARE,
    [MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT] = INKCELL_ICON_IMPORT,

    /* Stop configuring somebody else's radio. The way back, which is what that arrow means
       everywhere else in this client. */
    [MESH_UI_SETTINGS_ACTION_ADMIN_LOCAL] = INKCELL_ICON_BACK,
};

/*
 * What each verb costs, in the enum's own order.
 *
 * INKCELL_TONE_NORMAL is both the zero and the right answer for most rows, and the entries that
 * say so are written out anyway: a verb left out of this table and a verb deliberately drawn at
 * the ordinary weight are the same value, so spelling every row is the only thing that makes the
 * second one legible. What must not be left to the default is a row that deserved a weight and
 * did not get one, which is what `settings_verbs_that_cannot_be_undone_are_red` holds.
 */
static const enum inkcell_tone k_action_tones[MESH_UI_SETTINGS_ACTION_COUNT] = {
    [MESH_UI_SETTINGS_ACTION_CHECK_UPDATE] = INKCELL_TONE_NORMAL,
    /* Replaces the running binary and restarts under the reader. Not red - the check is the
       separate first press and the pak keeps what was there - but not an ordinary verb. */
    [MESH_UI_SETTINGS_ACTION_INSTALL_UPDATE] = INKCELL_TONE_WARNING,
    [MESH_UI_SETTINGS_ACTION_CYCLE_UPDATE_CHANNEL] = INKCELL_TONE_NORMAL,
    [MESH_UI_SETTINGS_ACTION_TOGGLE_DEV_UPDATES] = INKCELL_TONE_NORMAL,
    [MESH_UI_SETTINGS_ACTION_CYCLE_THEME] = INKCELL_TONE_NORMAL,
    [MESH_UI_SETTINGS_ACTION_CYCLE_LANGUAGE] = INKCELL_TONE_NORMAL,
    [MESH_UI_SETTINGS_ACTION_CYCLE_TEXT_SIZE] = INKCELL_TONE_NORMAL,
    /* The one copy of why the last run died, and nothing else has it - but what is lost is a
       diagnosis rather than anything the reader made, and this is the one row in the two tables
       with no confirm sheet in front of it. Red without a sheet is a trap; see
       `settings_verbs_that_cannot_be_undone_are_red`, which is what holds the pair together. */
    [MESH_UI_SETTINGS_ACTION_DISCARD_CRASH_REPORT] = INKCELL_TONE_WARNING,

    /* The link drops and auto-connect brings it back: nothing is lost, and you wait. */
    [MESH_UI_SETTINGS_ACTION_REBOOT] = INKCELL_TONE_WARNING,
    /* And the one row in the section this client cannot undo by any route - a radio that is off
       cannot be told to come on, so the way back is a walk to wherever it is. Red for the trip
       rather than for anything destroyed, which is the honest reading of what it costs. */
    [MESH_UI_SETTINGS_ACTION_SHUTDOWN] = INKCELL_TONE_ERROR,
    /* Both node stores rebuild from the air as their nodes speak again, which is what keeps
       them out of the red: what a press costs is the time until they do. The one that drops
       everything is a step above the one that drops what the radio has already let go. */
    [MESH_UI_SETTINGS_ACTION_RESET_NODEDB] = INKCELL_TONE_WARNING,
    [MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES] = INKCELL_TONE_NORMAL,
    [MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES] = INKCELL_TONE_WARNING,
    /*
     * The two rows nothing brings back. A factory reset is where the section has been heading
     * since its first row, and it is the only place the red belongs - which is the point of
     * spending it here rather than spreading it over the eight rows above.
     *
     * Marking every destructive row red marks none of them: this section is a list of things
     * done *to* a radio, so "this one costs something" is the baseline rather than the
     * exception, and what a reader needs from the colour is where the floor drops out. The
     * gradient the rows are already ordered by - least to most destructive - is what the three
     * weights are drawing.
     */
    [MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG] = INKCELL_TONE_ERROR,
    [MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE] = INKCELL_TONE_ERROR,
    /* A backup is the row you want pressed before the two under it. The restore overwrites every
       setting the radio holds and the delete throws the copy away - but the live configuration
       survives both, and another backup is one press away. */
    [MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG] = INKCELL_TONE_NORMAL,
    [MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG] = INKCELL_TONE_WARNING,
    [MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP] = INKCELL_TONE_WARNING,

    [MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION] = INKCELL_TONE_NORMAL,
    [MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION] = INKCELL_TONE_NORMAL,
    /* Turns the primary channel's encryption off, and pressing the row again does not turn it
       back on - the way out is two other rows in two other sections. */
    [MESH_UI_SETTINGS_ACTION_SET_HAM_MODE] = INKCELL_TONE_ERROR,
    [MESH_UI_SETTINGS_ACTION_REQUEST_HISTORY] = INKCELL_TONE_NORMAL,

    [MESH_UI_SETTINGS_ACTION_CHECK_RADIO_FIRMWARE] = INKCELL_TONE_NORMAL,
    [MESH_UI_SETTINGS_ACTION_CYCLE_FIRMWARE_CHANNEL] = INKCELL_TONE_NORMAL,
    /* Writing the radio's own firmware. Over USB the worst case is a board sitting in its
       bootloader that any computer can write again; over Bluetooth it leaves the mesh for a
       loader it cannot come back out of on its own, which is the difference the two rows exist
       for and the reason only one of them is red. */
    [MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB] = INKCELL_TONE_WARNING,
    [MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE] = INKCELL_TONE_ERROR,

    /* Showing a code touches nothing. Taking one in overwrites this radio's channel table,
       which is every channel the reader is on. */
    [MESH_UI_SETTINGS_ACTION_SHARE_CHANNELS] = INKCELL_TONE_NORMAL,
    [MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS] = INKCELL_TONE_WARNING,
    /* Red where the import above it is only amber, and the difference is what a key is. An
       import overwrites the table with one the reader is holding a link to; this erases a key
       and there may be no other copy of it anywhere. Nothing in this client brings it back. */
    [MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL] = INKCELL_TONE_ERROR,
    [MESH_UI_SETTINGS_ACTION_SHARE_CONTACT] = INKCELL_TONE_NORMAL,
    [MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT] = INKCELL_TONE_NORMAL,

    [MESH_UI_SETTINGS_ACTION_ADMIN_LOCAL] = INKCELL_TONE_NORMAL,
};

enum inkcell_icon mesh_ui_settings_action_icon(enum mesh_ui_settings_action action) {
    return action < MESH_UI_SETTINGS_ACTION_COUNT ? k_action_icons[action] : INKCELL_ICON_NONE;
}

enum inkcell_tone mesh_ui_settings_action_tone(enum mesh_ui_settings_action action) {
    return action < MESH_UI_SETTINGS_ACTION_COUNT ? k_action_tones[action] : INKCELL_TONE_NORMAL;
}

bool mesh_ui_settings_item_is_verb(const struct mesh_ui_settings_item *item) {
    return item != NULL && item->verb;
}

bool mesh_ui_settings_item_is_fact(const struct mesh_ui_settings_item *item) {
    if (item == NULL || item->kind == INKSTAND_FORM_HEADING) {
        return false;
    }
    /* Nothing the reader can press and nothing they can step. The ACTION test is what keeps a
       channel slot and a module row out of it: both are rows that open a list, and neither is
       a verb, so `verb` alone would read them as facts and quieten the one tier that is the
       name of the thing being opened. */
    return !item->verb && !item->cycle && item->field == MESH_UI_FIELD_NONE &&
           item->kind != INKSTAND_FORM_ACTION;
}

enum inkcell_icon mesh_ui_settings_item_marker(const struct mesh_ui_settings_item *item) {
    if (item == NULL) {
        return INKCELL_ICON_NONE;
    }
    /* The two that are about the value, ahead of everything about the offer - see the header. */
    if (item->conflict) {
        return INKCELL_ICON_WARNING;
    }
    if (item->dirty) {
        return INKCELL_ICON_UNSAVED;
    }
    if (item->cycle) {
        return INKCELL_ICON_SWAP;
    }
    /* A row with no field behind it is not changed here whatever its kind says: the read-only
       toggles this client draws for a radio's own switches are INKSTAND_FORM_TOGGLE and take
       MESH_UI_FIELD_NONE, and a mark on one would offer a press that does nothing. */
    if (item->field == MESH_UI_FIELD_NONE) {
        return INKCELL_ICON_NONE;
    }
    switch (item->kind) {
    /* A key row steps its presets on Left and Right as well, and still takes the pencil: typing
       one in is the thing it can do that no other row can, and the four presets are a shortcut
       past it rather than the point of the row. */
    case INKSTAND_FORM_TEXT:
    case INKSTAND_FORM_KEY:
        return INKCELL_ICON_EDIT;
    case INKSTAND_FORM_ENUM:
    case INKSTAND_FORM_NUMBER:
        return INKCELL_ICON_STEPPER;
    /* The switch and the checkbox say it themselves. */
    case INKSTAND_FORM_TOGGLE:
    case INKSTAND_FORM_FLAG:
    default:
        return INKCELL_ICON_NONE;
    }
}

uint32_t mesh_ui_settings_section_groups(const struct mesh_ui_settings_item *items,
                                         uint32_t count) {
    if (items == NULL) {
        return 0U;
    }
    uint32_t groups = 0U;
    bool open = false;
    for (uint32_t r = 0; r < count; ++r) {
        if (items[r].kind == INKSTAND_FORM_HEADING) {
            open = false;
            continue;
        }
        if (!open) {
            groups++;
            open = true;
        }
    }
    return groups;
}

/*
 * What each section is *for*, in a sentence or two, in the enum's own order.
 *
 * A table beside the icons and for the same reason the comment above them gives: it is a lookup
 * with no cases in it, and a section added without a note then reads as "no note" rather than
 * failing to compile in a file that has nothing to do with help.
 *
 * Every section has one, and that is a rule rather than an observation - it is what makes the
 * help key worth offering on every section screen. A field's note may be INKCELL_STR_NONE, because
 * most settings explain themselves; a section's may not, because "what is this whole screen
 * about" is the question somebody who opened it has by definition.
 */
static const inkcell_str_id k_section_notes[MESH_UI_SETTINGS_SECTION_COUNT] = {
    [MESH_UI_SETTINGS_ABOUT] = MESH_STR_SETTINGS_NOTE_ABOUT,
    [MESH_UI_SETTINGS_RADIO] = MESH_STR_SETTINGS_NOTE_RADIO,
    [MESH_UI_SETTINGS_USER] = MESH_STR_SETTINGS_NOTE_USER,
    [MESH_UI_SETTINGS_DEVICE] = MESH_STR_SETTINGS_NOTE_DEVICE,
    [MESH_UI_SETTINGS_DISPLAY] = MESH_STR_SETTINGS_NOTE_DISPLAY,
    [MESH_UI_SETTINGS_LORA] = MESH_STR_SETTINGS_NOTE_LORA,
    [MESH_UI_SETTINGS_BLUETOOTH] = MESH_STR_SETTINGS_NOTE_BLUETOOTH,
    [MESH_UI_SETTINGS_CHANNELS] = MESH_STR_SETTINGS_NOTE_CHANNELS,
    [MESH_UI_SETTINGS_SECURITY] = MESH_STR_SETTINGS_NOTE_SECURITY,
    [MESH_UI_SETTINGS_POSITION] = MESH_STR_SETTINGS_NOTE_POSITION,
    [MESH_UI_SETTINGS_POWER] = MESH_STR_SETTINGS_NOTE_POWER,
    [MESH_UI_SETTINGS_MQTT] = MESH_STR_SETTINGS_NOTE_MQTT,
    [MESH_UI_SETTINGS_STORE_FORWARD] = MESH_STR_SETTINGS_NOTE_STORE_FORWARD,
    [MESH_UI_SETTINGS_TELEMETRY] = MESH_STR_SETTINGS_NOTE_TELEMETRY,
    [MESH_UI_SETTINGS_ACTIONS] = MESH_STR_SETTINGS_NOTE_ACTIONS,
    [MESH_UI_SETTINGS_MODULES] = MESH_STR_SETTINGS_NOTE_MODULES,
    [MESH_UI_SETTINGS_NEIGHBOR_INFO] = MESH_STR_SETTINGS_NOTE_NEIGHBOR_INFO,
    [MESH_UI_SETTINGS_RANGE_TEST] = MESH_STR_SETTINGS_NOTE_RANGE_TEST,
    [MESH_UI_SETTINGS_PAXCOUNTER] = MESH_STR_SETTINGS_NOTE_PAXCOUNTER,
    [MESH_UI_SETTINGS_TAK] = MESH_STR_SETTINGS_NOTE_TAK,
    [MESH_UI_SETTINGS_AMBIENT] = MESH_STR_SETTINGS_NOTE_AMBIENT,
    [MESH_UI_SETTINGS_STATUS_MESSAGE] = MESH_STR_SETTINGS_NOTE_STATUS_MESSAGE,
    [MESH_UI_SETTINGS_DETECTION] = MESH_STR_SETTINGS_NOTE_DETECTION,
    [MESH_UI_SETTINGS_EXT_NOTIFICATION] = MESH_STR_SETTINGS_NOTE_EXT_NOTIFICATION,
    [MESH_UI_SETTINGS_TRAFFIC] = MESH_STR_SETTINGS_NOTE_TRAFFIC,
    [MESH_UI_SETTINGS_RADIO_UI] = MESH_STR_SETTINGS_NOTE_RADIO_UI,
    [MESH_UI_SETTINGS_CANNED] = MESH_STR_SETTINGS_NOTE_CANNED,
    [MESH_UI_SETTINGS_NETWORK] = MESH_STR_SETTINGS_NOTE_NETWORK,
    [MESH_UI_SETTINGS_BEACON] = MESH_STR_SETTINGS_NOTE_BEACON,
    [MESH_UI_SETTINGS_RADIO_DETAILS] = MESH_STR_SETTINGS_NOTE_RADIO_DETAILS,
    [MESH_UI_SETTINGS_NODE_LISTS] = MESH_STR_SETTINGS_NOTE_NODE_LISTS,
};

inkcell_str_id mesh_ui_settings_section_note(enum mesh_ui_settings_section section) {
    return section < MESH_UI_SETTINGS_SECTION_COUNT ? k_section_notes[section] : INKCELL_STR_NONE;
}

bool mesh_ui_settings_section_icons_rows(enum mesh_ui_settings_section section) {
    return section == MESH_UI_SETTINGS_MODULES;
}

/*
 * The two lists, as tables.
 *
 * k_root is the top level in the order it is read, which is roughly "this client, then what
 * the radio is, then how it talks, then everything optional, then the things that are not
 * settings at all". It is thirteen rows, which fits the Brick's screen without scrolling -
 * that is the point of Modules being one row rather than seventeen.
 */
/* Not a section: the heading row that opens the radio's card. See
 * mesh_ui_settings_root_is_heading(). */
#define ROOT_HEADING MESH_UI_SETTINGS_SECTION_COUNT

static const enum mesh_ui_settings_section k_root[] = {
    MESH_UI_SETTINGS_ABOUT,
    /* Everything above this is the client and everything below it is the radio. They shared one
       column until the heading, and the client's own theme and text size read as one more radio
       setting there. About stays the unnamed card at the top, so row 0 is still a section. */
    ROOT_HEADING,
    MESH_UI_SETTINGS_RADIO,
    MESH_UI_SETTINGS_USER,
    MESH_UI_SETTINGS_DEVICE,
    MESH_UI_SETTINGS_DISPLAY,
    MESH_UI_SETTINGS_RADIO_UI,
    MESH_UI_SETTINGS_POSITION,
    MESH_UI_SETTINGS_POWER,
    MESH_UI_SETTINGS_LORA,
    /* Network sits with the other two ways the radio talks to something that is not the mesh,
       and after Bluetooth because it is the one of the three this client is not using. */
    MESH_UI_SETTINGS_BLUETOOTH,
    MESH_UI_SETTINGS_NETWORK,
    MESH_UI_SETTINGS_CHANNELS,
    MESH_UI_SETTINGS_SECURITY,
    MESH_UI_SETTINGS_MODULES,
    MESH_UI_SETTINGS_ACTIONS,
};

/* Every ModuleConfig variant this client keeps. Grows by one row per module as the phases
   land; the order is the protobuf's field order, which is as good as any and is stable. */
static const enum mesh_ui_settings_section k_modules[] = {
    MESH_UI_SETTINGS_MQTT,
    MESH_UI_SETTINGS_STORE_FORWARD,
    MESH_UI_SETTINGS_TELEMETRY,
    MESH_UI_SETTINGS_RANGE_TEST,
    MESH_UI_SETTINGS_NEIGHBOR_INFO,
    MESH_UI_SETTINGS_AMBIENT,
    MESH_UI_SETTINGS_PAXCOUNTER,
    MESH_UI_SETTINGS_STATUS_MESSAGE,
    MESH_UI_SETTINGS_TAK,
    MESH_UI_SETTINGS_DETECTION,
    MESH_UI_SETTINGS_EXT_NOTIFICATION,
    MESH_UI_SETTINGS_TRAFFIC,
    MESH_UI_SETTINGS_BEACON,
    /* Last, and the one row here that is not a ModuleConfig: the canned message list is its
       own pair of admin verbs. It is in this list because the question the list answers - what
       is this radio running - is one it answers, and nowhere else would be shorter to find. */
    MESH_UI_SETTINGS_CANNED,
};

/* The rows of k_root that only a remote node's administration lists: see the header. The radio
   on the link keeps both on the Radio tab, as MESH_UI_SETTINGS_RADIO_DETAILS. */
static bool root_row_is_remote_only(enum mesh_ui_settings_section section) {
    return section == MESH_UI_SETTINGS_RADIO || section == MESH_UI_SETTINGS_ACTIONS;
}

uint32_t mesh_ui_settings_root_count(const struct mesh_ui_settings *settings) {
    const bool remote = settings != NULL && settings->admin_dest != 0U;
    uint32_t count = 0U;
    for (size_t i = 0; i < INKWELL_ARRAY_LEN(k_root); ++i) {
        count += (remote || !root_row_is_remote_only(k_root[i])) ? 1U : 0U;
    }
    return count;
}

enum mesh_ui_settings_section mesh_ui_settings_root_at(const struct mesh_ui_settings *settings,
                                                       uint32_t row) {
    const bool remote = settings != NULL && settings->admin_dest != 0U;
    uint32_t at = 0U;
    for (size_t i = 0; i < INKWELL_ARRAY_LEN(k_root); ++i) {
        if (!remote && root_row_is_remote_only(k_root[i])) {
            continue;
        }
        if (at++ == row) {
            return k_root[i];
        }
    }
    return MESH_UI_SETTINGS_ABOUT;
}

bool mesh_ui_settings_root_is_heading(const struct mesh_ui_settings *settings, uint32_t row) {
    return row < mesh_ui_settings_root_count(settings) &&
           mesh_ui_settings_root_at(settings, row) == ROOT_HEADING;
}

uint32_t mesh_ui_settings_module_count(void) { return (uint32_t)INKWELL_ARRAY_LEN(k_modules); }

enum mesh_ui_settings_section mesh_ui_settings_module_at(uint32_t row) {
    return row < INKWELL_ARRAY_LEN(k_modules) ? k_modules[row] : MESH_UI_SETTINGS_MQTT;
}

bool mesh_ui_settings_section_is_module(enum mesh_ui_settings_section section) {
    for (size_t i = 0; i < INKWELL_ARRAY_LEN(k_modules); ++i) {
        if (k_modules[i] == section) {
            return true;
        }
    }
    return false;
}

/*
 * Which bit of DeviceMetadata.excluded_modules stands for each section, in the enum's own
 * order, and 0 for a section the firmware cannot be built without.
 *
 * The values are literals rather than meshtastic_ExcludedModules_* because this file is on the
 * nanopb-free side of the fence, the way MESH_UI_CANNED_MESSAGES_MAX is in store.h - and like
 * that constant they are pinned against the protobuf by a test, so a renumbering upstream
 * fails there rather than quietly greying out the wrong row.
 *
 * Two of these are not modules at all: upstream reuses the mask to say a build has no
 * Bluetooth and no networking, which is exactly what the two sections by those names would
 * otherwise sit there waiting for.
 */
static const uint32_t k_section_excluded_bit[MESH_UI_SETTINGS_SECTION_COUNT] = {
    [MESH_UI_SETTINGS_BLUETOOTH] = 0x2000U,     [MESH_UI_SETTINGS_NETWORK] = 0x4000U,
    [MESH_UI_SETTINGS_MQTT] = 0x0001U,          [MESH_UI_SETTINGS_EXT_NOTIFICATION] = 0x0004U,
    [MESH_UI_SETTINGS_STORE_FORWARD] = 0x0008U, [MESH_UI_SETTINGS_RANGE_TEST] = 0x0010U,
    [MESH_UI_SETTINGS_TELEMETRY] = 0x0020U,     [MESH_UI_SETTINGS_CANNED] = 0x0040U,
    [MESH_UI_SETTINGS_NEIGHBOR_INFO] = 0x0200U, [MESH_UI_SETTINGS_AMBIENT] = 0x0400U,
    [MESH_UI_SETTINGS_DETECTION] = 0x0800U,     [MESH_UI_SETTINGS_PAXCOUNTER] = 0x1000U,
};

uint32_t mesh_ui_settings_section_excluded_bit(enum mesh_ui_settings_section section) {
    return section < MESH_UI_SETTINGS_SECTION_COUNT ? k_section_excluded_bit[section] : 0U;
}

enum mesh_ui_settings_availability
mesh_ui_settings_section_availability(const struct mesh_ui_settings *settings,
                                      const struct mesh_ui_handshake_state *handshake,
                                      enum mesh_ui_settings_section section) {
    if (mesh_ui_settings_section_loaded(settings, handshake, section)) {
        /* The radio answered for it. A build that says it excluded a section and then sends one
           is telling us two things, and the one with rows in it wins. */
        return MESH_UI_SETTINGS_SECTION_READY;
    }
    const uint32_t bit = mesh_ui_settings_section_excluded_bit(section);
    if (settings != NULL && settings->has_metadata && bit != 0U &&
        (settings->excluded_modules & bit) != 0U) {
        return MESH_UI_SETTINGS_SECTION_EXCLUDED;
    }
    return MESH_UI_SETTINGS_SECTION_WAITING;
}

inkcell_str_id mesh_ui_settings_availability_label(enum mesh_ui_settings_availability state) {
    switch (state) {
    case MESH_UI_SETTINGS_SECTION_WAITING:
        return MESH_STR_SETTINGS_NOT_LOADED;
    case MESH_UI_SETTINGS_SECTION_EXCLUDED:
        return MESH_STR_SETTINGS_NOT_IN_FIRMWARE;
    case MESH_UI_SETTINGS_SECTION_READY:
    default:
        return INKCELL_STR_NONE;
    }
}

inkcell_str_id mesh_ui_settings_availability_reason(enum mesh_ui_settings_availability state) {
    switch (state) {
    case MESH_UI_SETTINGS_SECTION_EXCLUDED:
        return MESH_STR_SETTINGS_EMPTY_EXCLUDED;
    case MESH_UI_SETTINGS_SECTION_WAITING:
    case MESH_UI_SETTINGS_SECTION_READY:
    default:
        return MESH_STR_SETTINGS_EMPTY_SECTION;
    }
}

bool mesh_ui_settings_section_loaded(const struct mesh_ui_settings *settings,
                                     const struct mesh_ui_handshake_state *handshake,
                                     enum mesh_ui_settings_section section) {
    if (settings == NULL) {
        return false;
    }
    switch (section) {
    case MESH_UI_SETTINGS_ABOUT:
        /* The client knows its own version with no radio in sight, which is the whole point of
           having this section: it is reachable before anything is connected. */
        return true;
    case MESH_UI_SETTINGS_RADIO:
        return settings->has_metadata || (handshake != NULL && handshake->has_my_info);
    case MESH_UI_SETTINGS_USER:
        return settings->has_owner;
    case MESH_UI_SETTINGS_DEVICE:
        return settings->has_device;
    case MESH_UI_SETTINGS_DISPLAY:
        return settings->has_display;
    case MESH_UI_SETTINGS_LORA:
        return settings->has_lora;
    case MESH_UI_SETTINGS_BLUETOOTH:
        return settings->has_bluetooth;
    case MESH_UI_SETTINGS_NETWORK:
        return settings->has_network;
    case MESH_UI_SETTINGS_CHANNELS:
        return settings->has_channels || (handshake != NULL && handshake->channel_count > 0U);
    case MESH_UI_SETTINGS_SECURITY:
        return settings->has_security;
    case MESH_UI_SETTINGS_POSITION:
        return settings->has_position;
    case MESH_UI_SETTINGS_POWER:
        return settings->has_power;
    case MESH_UI_SETTINGS_MQTT:
        return settings->has_mqtt;
    case MESH_UI_SETTINGS_STORE_FORWARD:
        return settings->has_store_forward;
    case MESH_UI_SETTINGS_TELEMETRY:
        return settings->has_telemetry;
    case MESH_UI_SETTINGS_ACTIONS:
        /* Nothing is read for this section, so what it waits on is not a config fragment but
           the one thing an AdminMessage cannot be addressed without: our own node number. A
           cached roster with no link opens it too, for the two rows that drop that roster and
           send nothing - the rest then render as "not connected". */
        return handshake != NULL && (handshake->has_my_info || handshake->node_count > 0U);
    /* The union of the two it is made of: About radio's facts, or enough to address the verbs
       under them. The node lists wait on what Radio actions did, for the same two rows. */
    case MESH_UI_SETTINGS_RADIO_DETAILS:
        return settings->has_metadata ||
               (handshake != NULL && (handshake->has_my_info || handshake->node_count > 0U));
    case MESH_UI_SETTINGS_NODE_LISTS:
        return handshake != NULL && (handshake->has_my_info || handshake->node_count > 0U);
    case MESH_UI_SETTINGS_MODULES:
        /* A folder, not a fragment. It lists every module whether or not the radio has sent
           one, because "which of these has not arrived" is exactly what the list is for. */
        return true;
    case MESH_UI_SETTINGS_NEIGHBOR_INFO:
        return settings->has_neighbor_info;
    case MESH_UI_SETTINGS_RANGE_TEST:
        return settings->has_range_test;
    case MESH_UI_SETTINGS_PAXCOUNTER:
        return settings->has_paxcounter;
    case MESH_UI_SETTINGS_TAK:
        return settings->has_tak;
    case MESH_UI_SETTINGS_AMBIENT:
        return settings->has_ambient_lighting;
    case MESH_UI_SETTINGS_STATUS_MESSAGE:
        return settings->has_status_message;
    case MESH_UI_SETTINGS_DETECTION:
        return settings->has_detection_sensor;
    case MESH_UI_SETTINGS_EXT_NOTIFICATION:
        return settings->has_external_notification;
    case MESH_UI_SETTINGS_TRAFFIC:
        return settings->has_traffic_management;
    case MESH_UI_SETTINGS_BEACON:
        return settings->has_mesh_beacon;
    case MESH_UI_SETTINGS_RADIO_UI:
        return settings->has_ui_config;
    case MESH_UI_SETTINGS_CANNED:
        return settings->has_canned_messages;
    default:
        return false;
    }
}

/* ---- editable fields ---------------------------------------------------------------------- */

static const char *compass_name(uint32_t orientation) {
    static const inkcell_str_id k_names[] = {
        MESH_STR_ENUM_COMPASS_0,        MESH_STR_ENUM_COMPASS_90,
        MESH_STR_ENUM_COMPASS_180,      MESH_STR_ENUM_COMPASS_270,
        MESH_STR_ENUM_COMPASS_0_FLIP,   MESH_STR_ENUM_COMPASS_90_FLIP,
        MESH_STR_ENUM_COMPASS_180_FLIP, MESH_STR_ENUM_COMPASS_270_FLIP,
    };
    return inkcell_str(orientation < INKWELL_ARRAY_LEN(k_names) ? k_names[orientation]
                                                                : INKCELL_STR_COMMON_UNKNOWN_SHORT);
}

static const char *units_name(uint32_t units) {
    return inkcell_str(mesh_ui_units_imperial((uint8_t)units) ? MESH_STR_ENUM_UNITS_IMPERIAL
                                                              : MESH_STR_ENUM_UNITS_METRIC);
}

/* DisplayConfig.OledType, 0..5 and contiguous. The panel a board carries, for the case where
   the radio's own autodetect got it wrong; every value after Auto is a controller part
   number, because that is what the board's documentation calls it. */
static const char *oled_name(uint32_t oled) {
    static const inkcell_str_id k_names[] = {
        MESH_STR_ENUM_OLED_AUTO,   MESH_STR_ENUM_OLED_SSD1306,    MESH_STR_ENUM_OLED_SH1106,
        MESH_STR_ENUM_OLED_SH1107, MESH_STR_ENUM_OLED_SH1107_128, MESH_STR_ENUM_OLED_SH1107_ROT,
    };
    return inkcell_str(oled < INKWELL_ARRAY_LEN(k_names) ? k_names[oled]
                                                         : INKCELL_STR_COMMON_UNKNOWN_SHORT);
}

/* DisplayConfig.DisplayMode, 0..3 and contiguous. */
static const char *displaymode_name(uint32_t mode) {
    static const inkcell_str_id k_names[] = {
        MESH_STR_ENUM_DISPLAYMODE_DEFAULT,
        MESH_STR_ENUM_DISPLAYMODE_TWOCOLOR,
        MESH_STR_ENUM_DISPLAYMODE_INVERTED,
        MESH_STR_ENUM_DISPLAYMODE_COLOR,
    };
    return inkcell_str(mode < INKWELL_ARRAY_LEN(k_names) ? k_names[mode]
                                                         : INKCELL_STR_COMMON_UNKNOWN_SHORT);
}

/* DetectionSensorConfig.TriggerType, 0..5 and contiguous. Named for what the pin does rather
   than for the constant: "Low" says more than "LOGIC_LOW" next to the word Trigger. */
static const char *trigger_name(uint32_t trigger) {
    static const inkcell_str_id k_names[] = {
        MESH_STR_ENUM_TRIGGER_LOW,        MESH_STR_ENUM_TRIGGER_HIGH,
        MESH_STR_ENUM_TRIGGER_FALLING,    MESH_STR_ENUM_TRIGGER_RISING,
        MESH_STR_ENUM_TRIGGER_EITHER_LOW, MESH_STR_ENUM_TRIGGER_EITHER_HIGH,
    };
    return inkcell_str(trigger < INKWELL_ARRAY_LEN(k_names) ? k_names[trigger]
                                                            : INKCELL_STR_COMMON_UNKNOWN_SHORT);
}

/* DeviceUIConfig's three editable enums, all contiguous from 0 - which is what the nav's
   (value + 1) % count stepping needs, and why Language is not among them. */
static const char *ui_theme_name(uint32_t theme) {
    static const inkcell_str_id k_names[] = {
        MESH_STR_ENUM_UI_THEME_DARK,
        MESH_STR_ENUM_UI_THEME_LIGHT,
        MESH_STR_ENUM_UI_THEME_RED,
    };
    return inkcell_str(theme < INKWELL_ARRAY_LEN(k_names) ? k_names[theme]
                                                          : INKCELL_STR_COMMON_UNKNOWN_SHORT);
}

static const char *ui_compass_name(uint32_t mode) {
    static const inkcell_str_id k_names[] = {
        MESH_STR_ENUM_UI_COMPASS_DYNAMIC,
        MESH_STR_ENUM_UI_COMPASS_FIXED,
        MESH_STR_ENUM_UI_COMPASS_FREEZE,
    };
    return inkcell_str(mode < INKWELL_ARRAY_LEN(k_names) ? k_names[mode]
                                                         : INKCELL_STR_COMMON_UNKNOWN_SHORT);
}

/* Named for what a reader would call the format rather than for the acronym, except where the
   acronym is what it is called - MGRS and UTM are not expanded on a map either. */
static const char *ui_gps_format_name(uint32_t format) {
    static const inkcell_str_id k_names[] = {
        MESH_STR_ENUM_UI_GPS_DEC,  MESH_STR_ENUM_UI_GPS_DMS, MESH_STR_ENUM_UI_GPS_UTM,
        MESH_STR_ENUM_UI_GPS_MGRS, MESH_STR_ENUM_UI_GPS_OLC, MESH_STR_ENUM_UI_GPS_OSGR,
        MESH_STR_ENUM_UI_GPS_MLS,
    };
    return inkcell_str(format < INKWELL_ARRAY_LEN(k_names) ? k_names[format]
                                                           : INKCELL_STR_COMMON_UNKNOWN_SHORT);
}

/* `is_clockface_analog` is a bool on the wire and an enum here: a row reading "Clock face: on"
   says nothing, and the two values have names. */
static const char *ui_clockface_name(uint32_t analog) {
    return inkcell_str(analog != 0U ? MESH_STR_ENUM_UI_CLOCK_ANALOG
                                    : MESH_STR_ENUM_UI_CLOCK_DIGITAL);
}

/* meshtastic_Team and meshtastic_MemberRole, from atak.proto. Both are contiguous from 0, which
   is what the nav's (value + 1) % count stepping needs; value 0 is "Unspecifed" upstream (their
   spelling), shown here as what the firmware actually does with it.

   Named as the phone apps name them - "RTO", not its expansion - for the reason keys are shown
   as base64: a setting read off the Brick should be recognisable in the app and back. */
static const char *tak_team_name(uint32_t team) {
    static const inkcell_str_id k_names[] = {
        MESH_STR_ENUM_TAK_TEAM_DEFAULT,   MESH_STR_ENUM_TAK_TEAM_WHITE,
        MESH_STR_ENUM_TAK_TEAM_YELLOW,    MESH_STR_ENUM_TAK_TEAM_ORANGE,
        MESH_STR_ENUM_TAK_TEAM_MAGENTA,   MESH_STR_ENUM_TAK_TEAM_RED,
        MESH_STR_ENUM_TAK_TEAM_MAROON,    MESH_STR_ENUM_TAK_TEAM_PURPLE,
        MESH_STR_ENUM_TAK_TEAM_DARK_BLUE, MESH_STR_ENUM_TAK_TEAM_BLUE,
        MESH_STR_ENUM_TAK_TEAM_CYAN,      MESH_STR_ENUM_TAK_TEAM_TEAL,
        MESH_STR_ENUM_TAK_TEAM_GREEN,     MESH_STR_ENUM_TAK_TEAM_DARK_GREEN,
        MESH_STR_ENUM_TAK_TEAM_BROWN,
    };
    return inkcell_str(team < INKWELL_ARRAY_LEN(k_names) ? k_names[team]
                                                         : INKCELL_STR_COMMON_UNKNOWN_SHORT);
}

static const char *tak_role_name(uint32_t role) {
    static const inkcell_str_id k_names[] = {
        MESH_STR_ENUM_TAK_ROLE_DEFAULT,     MESH_STR_ENUM_TAK_ROLE_MEMBER,
        MESH_STR_ENUM_TAK_ROLE_LEAD,        MESH_STR_ENUM_TAK_ROLE_HQ,
        MESH_STR_ENUM_TAK_ROLE_SNIPER,      MESH_STR_ENUM_TAK_ROLE_MEDIC,
        MESH_STR_ENUM_TAK_ROLE_FORWARD_OBS, MESH_STR_ENUM_TAK_ROLE_RTO,
        MESH_STR_ENUM_TAK_ROLE_K9,
    };
    return inkcell_str(role < INKWELL_ARRAY_LEN(k_names) ? k_names[role]
                                                         : INKCELL_STR_COMMON_UNKNOWN_SHORT);
}

static const char *rebroadcast_name(uint32_t mode) {
    static const inkcell_str_id k_names[] = {
        MESH_STR_ENUM_REBROADCAST_ALL,   MESH_STR_ENUM_REBROADCAST_ALL_SKIP,
        MESH_STR_ENUM_REBROADCAST_LOCAL, MESH_STR_ENUM_REBROADCAST_KNOWN,
        MESH_STR_ENUM_REBROADCAST_NONE,  MESH_STR_ENUM_REBROADCAST_CORE,
    };
    return inkcell_str(mode < INKWELL_ARRAY_LEN(k_names) ? k_names[mode]
                                                         : INKCELL_STR_COMMON_UNKNOWN_SHORT);
}

static const char *gps_mode_name(uint32_t mode) {
    switch (mode) {
    case 0U:
        return inkcell_str(MESH_STR_ENUM_GPS_DISABLED);
    case 1U:
        return inkcell_str(MESH_STR_ENUM_GPS_ENABLED);
    case 2U:
        return inkcell_str(MESH_STR_ENUM_GPS_NOT_PRESENT);
    default:
        return inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT);
    }
}

static const char *channel_role_name(uint32_t value) {
    return inkcell_str(value == 1U ? MESH_STR_ENUM_CHANNEL_SECONDARY
                                   : MESH_STR_ENUM_CHANNEL_DISABLED);
}

static const char *pairing_enum_name(uint32_t mode) {
    switch (mode) {
    case 0U:
        return inkcell_str(MESH_STR_ENUM_PAIRING_RANDOM_PIN);
    case 1U:
        return inkcell_str(MESH_STR_ENUM_PAIRING_FIXED_PIN);
    case 2U:
        return inkcell_str(MESH_STR_ENUM_PAIRING_NO_PIN);
    default:
        return inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT);
    }
}

/* Position precision is a bit count; the phone apps label the useful ones by distance. */
/*
 * What `precision_bits` means on the ground, said twice - as the words a row reads and as the
 * number a map draws a ring from.
 *
 * One table with two columns rather than two tables, because they are one fact. The words are
 * catalog ids and cannot be derived from the metres (a locale writes "~23 km" its own way), and
 * the metres cannot be parsed back out of the words without a screen doing arithmetic on a
 * translation - so the only way for the two to stay in step is for them to be written on the
 * same line. Ten rows, covering the bit counts the firmware's own channel setting offers; the
 * `bits` outside that range are answered by the branches below rather than by a row here.
 */
static const struct {
    inkcell_str_id label;
    inkcell_str_id imperial;
    uint32_t metres;
} k_precision[] = {
    {MESH_STR_VALUE_PRECISION_23KM, MESH_STR_VALUE_PRECISION_14MI, 23000U},
    {MESH_STR_VALUE_PRECISION_12KM, MESH_STR_VALUE_PRECISION_7_5MI, 12000U},
    {MESH_STR_VALUE_PRECISION_6KM, MESH_STR_VALUE_PRECISION_3_6MI, 5800U},
    {MESH_STR_VALUE_PRECISION_3KM, MESH_STR_VALUE_PRECISION_1_8MI, 2900U},
    {MESH_STR_VALUE_PRECISION_1_5KM, MESH_STR_VALUE_PRECISION_0_9MI, 1500U},
    {MESH_STR_VALUE_PRECISION_730M, MESH_STR_VALUE_PRECISION_2400FT, 730U},
    {MESH_STR_VALUE_PRECISION_360M, MESH_STR_VALUE_PRECISION_1200FT, 360U},
    {MESH_STR_VALUE_PRECISION_180M, MESH_STR_VALUE_PRECISION_600FT, 180U},
    {MESH_STR_VALUE_PRECISION_90M, MESH_STR_VALUE_PRECISION_300FT, 90U},
    {MESH_STR_VALUE_PRECISION_45M, MESH_STR_VALUE_PRECISION_150FT, 45U},
};

uint32_t mesh_ui_settings_precision_metres(uint32_t bits) {
    /*
     * 0 for anything this cannot answer with a distance, which is three different states and
     * all of them mean "do not draw a circle": the sender never set the field, the sender said
     * the fix is exact, or the count is outside the range upstream's own setting offers. A ring
     * of zero radius is the honest picture of all three.
     */
    if (bits < 10U || bits > 19U) {
        return 0U;
    }
    return k_precision[bits - 10U].metres;
}

void mesh_ui_settings_format_precision(uint32_t bits, bool imperial, char *out, size_t out_len) {
    if (bits == 0U) {
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_VALUE_PRECISION_OFF));
    } else if (bits >= 32U) {
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_VALUE_PRECISION_EXACT));
    } else if (bits >= 10U && bits <= 19U) {
        const size_t row = (size_t)bits - 10U;
        snprintf(out, out_len, "%s",
                 inkcell_str(imperial ? k_precision[row].imperial : k_precision[row].label));
    } else {
        inkcell_str_format(out, out_len, MESH_STR_VALUE_PRECISION_BITS, (unsigned)bits);
    }
}

/* PositionConfig's smart-broadcast threshold, which is metres on the wire whatever it reads as. */
static void format_metres(uint32_t metres, bool imperial, char *out, size_t out_len) {
    if (metres == 0U) {
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_COMMON_DEFAULT));
        return;
    }
    mesh_ui_format_length(metres, imperial, out, out_len);
}

/*
 * The role names as a *chooser* shows them. ROUTER_CLIENT and REPEATER are still in the
 * protobuf and still what an older radio reports, so they have to be steppable - the row
 * could not otherwise display the setting a node already has - but they are marked so nobody
 * picks one on purpose. mesh_radio_role_name() stays unmarked: reading another node's role in
 * the Nodes tab is not making a choice.
 */
static const char *role_enum_name(uint32_t role) {
    switch (role) {
    case 3U:
        return inkcell_str(MESH_STR_ENUM_ROLE_ROUTER_CLIENT);
    case 4U:
        return inkcell_str(MESH_STR_ENUM_ROLE_REPEATER);
    default:
        return mesh_radio_role_name(role);
    }
}

static const char *region_enum_name(uint32_t region) { return mesh_radio_region_name(region); }
static const char *preset_enum_name(uint32_t preset) {
    return mesh_radio_modem_preset_name(preset);
}
/*
 * Mesh beacon's two preset rows, and the one thing the module's encoding costs.
 *
 * `broadcast_offer_preset` and a target's `preset` are `optional` on the wire, so absent is a
 * value the row has to be able to show and to step onto. Both are stored one past themselves -
 * 0 is "there is no preset here" and n is ModemPreset n-1 - which keeps the row an ordinary
 * contiguous enum that Left and Right walk by modulo. The two differ only in what absent
 * *means*, and that is the difference the two names below are: an offer that names no preset is
 * not offering one, while a target that names none goes out on whatever the radio is running.
 */
static const char *beacon_offer_preset_name(uint32_t value) {
    return value == 0U ? inkcell_str(MESH_STR_ENUM_BEACON_NOT_OFFERED)
                       : mesh_radio_modem_preset_name(value - 1U);
}
static const char *beacon_target_preset_name(uint32_t value) {
    return value == 0U ? inkcell_str(MESH_STR_ENUM_BEACON_AS_RUNNING)
                       : mesh_radio_modem_preset_name(value - 1U);
}
/*
 * The region rows say absent in the same words their neighbours do, which is why they do not
 * simply take `region_enum_name`. RegionCode's own name for 0 is "Unset", and a target reading
 * "as configured", "Unset", "as configured" down three rows that all mean the same thing is a
 * record that looks like it is holding two answers and a mistake.
 */
static const char *beacon_offer_region_name(uint32_t value) {
    return value == 0U ? inkcell_str(MESH_STR_ENUM_BEACON_NOT_OFFERED)
                       : mesh_radio_region_name(value);
}
static const char *beacon_target_region_name(uint32_t value) {
    return value == 0U ? inkcell_str(MESH_STR_ENUM_BEACON_AS_RUNNING)
                       : mesh_radio_region_name(value);
}
/*
 * A target's channel is the same "0 is absent" shift over a channel index, and it is a NUMBER
 * rather than an ENUM because an index is a number: there is nothing to name. Which is also why
 * it can carry the shift without a second naming function - the field's zero_label says what 0
 * is and the formatter prints n-1, so nothing outside these three lines sees the offset.
 */
static void format_beacon_channel(uint32_t value, bool imperial, char *out, size_t out_len) {
    /* Not a length. Every NUMBER formatter takes the units so none of them can silently
       decide to keep metres; the ones with nothing to say discard it here. */
    (void)imperial;
    inkcell_str_format(out, out_len, MESH_STR_VALUE_PLAIN,
                       (unsigned)(value > 0U ? value - 1U : 0U));
}

static const char *signature_policy_name(uint32_t policy) {
    switch (policy) {
    case 0U:
        return inkcell_str(MESH_STR_ENUM_SIGNATURE_COMPATIBLE);
    case 1U:
        return inkcell_str(MESH_STR_ENUM_SIGNATURE_BALANCED);
    case 2U:
        return inkcell_str(MESH_STR_ENUM_SIGNATURE_STRICT);
    default:
        return inkcell_str(INKCELL_STR_COMMON_UNKNOWN_SHORT);
    }
}

static void format_bandwidth(uint32_t khz, bool imperial, char *out, size_t out_len) {
    (void)imperial;
    if (khz == 31U) {
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_VALUE_BANDWIDTH_31));
    } else if (khz == 62U) {
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_VALUE_BANDWIDTH_62));
    } else {
        inkcell_str_format(out, out_len, MESH_STR_VALUE_BANDWIDTH_KHZ, (unsigned)khz);
    }
}
static void format_plain(uint32_t value, bool imperial, char *out, size_t out_len) {
    (void)imperial;
    inkcell_str_format(out, out_len, MESH_STR_VALUE_PLAIN, (unsigned)value);
}
static void format_coding_rate(uint32_t value, bool imperial, char *out, size_t out_len) {
    (void)imperial;
    inkcell_str_format(out, out_len, MESH_STR_VALUE_CODING_RATE, (unsigned)value);
}
static void format_tx_power(uint32_t value, bool imperial, char *out, size_t out_len) {
    (void)imperial;
    if (value == 0U) {
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_VALUE_TX_POWER_MAX));
    } else {
        inkcell_str_format(out, out_len, MESH_STR_VALUE_DBM, (int)(int8_t)value);
    }
}

static const uint32_t k_bandwidth_presets[] = {31U, 62U, 125U, 250U, 500U};
static const uint32_t k_spread_presets[] = {7U, 8U, 9U, 10U, 11U, 12U};
static const uint32_t k_coding_presets[] = {5U, 6U, 7U, 8U};
static const uint32_t k_hop_presets[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U};
static const uint32_t k_tx_power_presets[] = {0U, 2U, 5U, 8U, 10U, 14U, 17U, 20U, 22U, 27U, 30U};

static const uint32_t k_precision_presets[] = {0U,  10U, 11U, 12U, 13U, 14U,
                                               15U, 16U, 17U, 18U, 19U, 32U};

/*
 * DeviceUIConfig's three number rows.
 *
 * Brightness is a level out of 255 and every stop is a real setting, so it is a plain scale
 * with no zero to stand outside it - a screen at 0 is a screen turned down, not a screen with
 * no brightness. The timeout's 0 is "never sleep", which is not a duration, so it stands
 * outside the track the way every other zero-as-a-word does. The ring tone is an index into
 * the firmware's own list of tunes: it names one rather than measuring anything, so it is a
 * NAMED_PRESETS run of every value the field can hold rather than a curated few.
 */
static const uint32_t k_ui_brightness_presets[] = {16U,  32U,  64U,  96U, 128U,
                                                   160U, 192U, 224U, 255U};
static const uint32_t k_ui_timeout_presets[] = {0U,   15U,  30U,  60U,   120U,
                                                300U, 600U, 900U, 1800U, 3600U};
static const uint32_t k_ui_ringtone_presets[] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};

/* 0 means "firmware default" for these, and the presets are what the phone apps offer. */
static const uint32_t k_screen_on_presets[] = {0U,   15U,  30U,  60U,   120U,
                                               300U, 600U, 900U, 1800U, 3600U};
static const uint32_t k_carousel_presets[] = {0U, 10U, 15U, 30U, 60U, 120U, 300U, 600U};
static const uint32_t k_interval_presets[] = {0U,    60U,   300U,   900U,   1800U,
                                              3600U, 7200U, 14400U, 43200U, 86400U};

/* Device, Position and Power intervals. Each list starts at 0, which every one of these
   fields reads as "the firmware's own default" rather than as zero seconds. */
static const uint32_t k_nodeinfo_presets[] = {0U, 3600U, 7200U, 10800U, 21600U, 43200U, 86400U};
static const uint32_t k_gps_interval_presets[] = {0U, 30U, 60U, 120U, 300U, 600U, 900U, 1800U};
static const uint32_t k_smart_distance_presets[] = {0U, 10U, 25U, 50U, 100U, 250U, 500U, 1000U};
static const uint32_t k_smart_interval_presets[] = {0U, 30U, 60U, 120U, 300U, 600U};
static const uint32_t k_sleep_presets[] = {0U, 60U, 300U, 600U, 900U, 1800U, 3600U};
static const uint32_t k_wake_presets[] = {0U, 5U, 10U, 30U, 60U, 120U, 300U};
static const uint32_t k_wait_bt_presets[] = {0U, 10U, 30U, 60U, 120U, 300U};
static const uint32_t k_shutdown_presets[] = {0U,    300U,   1800U,  3600U,
                                              7200U, 21600U, 43200U, 86400U};

/* Store & Forward. The firmware sizes its own ring when `records` is 0, which on an ESP32
   with PSRAM is larger than a hand-picked number would be, so 0 stays the first preset and
   the rest are for a node deliberately kept small. `history_return_window` is seconds of
   backlog a client may ask for; the other two are counts, not seconds, which is why they
   carry format_count rather than the seconds default. */
static const uint32_t k_sf_records_presets[] = {0U, 25U, 50U, 100U, 250U, 500U, 1000U};
static const uint32_t k_sf_history_presets[] = {0U, 10U, 25U, 50U, 100U, 250U};
static const uint32_t k_sf_window_presets[] = {0U, 300U, 900U, 1800U, 3600U, 7200U, 86400U};

/* Map reporting. The public map drops anything under an hour, so the presets start there
   rather than offering a value the server will not honour. */
static const uint32_t k_map_interval_presets[] = {3600U, 7200U, 10800U, 21600U, 43200U, 86400U};

/* Neighbor info. The firmware floors the interval at 14400 (4 hours) and quietly raises
   anything under it, so the presets start there instead of showing a value that will not
   survive the read-back. */
static const uint32_t k_neighbor_presets[] = {14400U, 21600U, 43200U, 86400U};

/* Range test's sender interval. A sender transmits to everyone on the channel on this timer,
   so the list starts at 0 (receive only, which is what the module is for most of the time) and
   then at 30s - fast enough to walk a link out, slow enough not to swamp the channel. */
static const uint32_t k_range_test_presets[] = {0U, 30U, 60U, 120U, 300U, 600U, 1800U};

/*
 * Paxcounter's two RSSI floors, which are the only NUMBER rows in the client over a signed
 * value. They are stored as uint32_t like every other preset, through a cast.
 *
 * That works because they are all negative: two's-complement negatives cast to uint32_t all
 * land in the top half of the range and keep their relative order (-100 -> 0xFFFFFF9C is below
 * -80 -> 0xFFFFFFB0), so mesh_ui_settings_number_step() walks them correctly without knowing
 * they are signed. A list mixing signs would not step correctly, and there is no reason for one
 * here: an RSSI threshold above 0 dBm is not a threshold.
 */
#define RSSI(dbm) ((uint32_t)(int32_t)(dbm))
static const uint32_t k_rssi_presets[] = {RSSI(-100), RSSI(-95), RSSI(-90), RSSI(-85), RSSI(-80),
                                          RSSI(-75),  RSSI(-70), RSSI(-65), RSSI(-60)};

/*
 * GPIO pins: every number in the range, not a curated subset.
 *
 * The first version of this list was the pins an ESP32 typically exposes, which is exactly the
 * board-specific assumption there is no way to make here - nothing on the wire says what board
 * this is, and a RAK4631 puts usable output on 9, 10 and 28, all of which that list omitted and
 * so made unreachable. A contiguous range covers nRF52840 (P0.00-P1.15, flat 0-47) and ESP32-S3
 * (0-48) alike, and stepping it is not tedious because the d-pad autorepeats (inkcell_input
 * honours repeat for the navigation keys).
 *
 * Whether a pin is wired to anything remains the radio's business and unanswerable here; this
 * only rules out numbers no board has.
 */
static const uint32_t k_gpio_presets[] = {
    0U,  1U,  2U,  3U,  4U,  5U,  6U,  7U,  8U,  9U,  10U, 11U, 12U, 13U, 14U, 15U, 16U,
    17U, 18U, 19U, 20U, 21U, 22U, 23U, 24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U, 32U, 33U,
    34U, 35U, 36U, 37U, 38U, 39U, 40U, 41U, 42U, 43U, 44U, 45U, 46U, 47U, 48U};

/* External notification's on-time and repeat: output_ms is milliseconds, nag_timeout seconds. */
static const uint32_t k_output_ms_presets[] = {100U, 250U, 500U, 1000U, 2000U, 5000U};
static const uint32_t k_nag_presets[] = {0U, 10U, 30U, 60U, 120U, 300U};

/* Detection sensor's two intervals: a floor between messages, and a heartbeat the proto says
   is off at 0. */
static const uint32_t k_detect_min_presets[] = {0U, 10U, 30U, 60U, 300U, 900U};
static const uint32_t k_detect_state_presets[] = {0U, 60U, 300U, 900U, 1800U, 3600U};

/* Traffic management. Every row is off at 0 by the module's own convention, so each list
   starts there; hops and packet counts are counts rather than seconds. */
/*
 * The beacon's own two lists.
 *
 * The interval starts at the firmware's floor of an hour and does not go below it, which is
 * Range test's antisocial-transmitter rule with a harder edge: a beacon carries a channel offer
 * that other nodes act on, so a fast one is not merely noisy. There is no 0 on the list because
 * 0 is not "off" here - the broadcast flag is - and a row whose zero means nothing should not
 * offer one.
 *
 * The channel list is every slot plus the absent one at 0, so it names rather than measures.
 */
static const uint32_t k_beacon_interval_presets[] = {3600U,  7200U,  10800U, 21600U,
                                                     43200U, 86400U, 172800U};
static const uint32_t k_beacon_channel_presets[] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

static const uint32_t k_traffic_interval_presets[] = {0U, 30U, 60U, 120U, 300U, 600U, 1800U};
static const uint32_t k_traffic_hops_presets[] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
static const uint32_t k_traffic_packets_presets[] = {0U, 5U, 10U, 20U, 50U, 100U, 200U};

/* Ambient lighting: an LED current in mA (the firmware's default is 10) and three 0-255
   channels. Three number rows rather than a colour picker, which a d-pad cannot do well. */
static const uint32_t k_led_current_presets[] = {0U, 5U, 10U, 15U, 20U, 25U, 30U};
static const uint32_t k_led_level_presets[] = {0U, 32U, 64U, 96U, 128U, 160U, 192U, 224U, 255U};

#define CHANNEL_KEY_CHOICES                                                                        \
    (MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_KEEP) | MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_DEFAULT) |      \
     MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_RANDOM_128) |                                              \
     MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_RANDOM_256) | MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_NONE))
#define PRIVATE_KEY_CHOICES                                                                        \
    (MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_KEEP) | MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_RANDOM_256))
#define ADMIN_KEY_CHOICES                                                                          \
    (MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_KEEP) | MESH_UI_PSK_CHOICE_BIT(MESH_UI_PSK_NONE))

/* Milliseconds, for the notification's on-time: the seconds formatter would call 500 ms
   "500s". */
static void format_millis(uint32_t value, bool imperial, char *out, size_t out_len) {
    (void)imperial;
    if (value % 1000U == 0U && value != 0U) {
        inkcell_str_format(out, out_len, MESH_STR_VALUE_SECONDS, (unsigned)(value / 1000U));
    } else {
        inkcell_str_format(out, out_len, MESH_STR_VALUE_MILLISECONDS, (unsigned)value);
    }
}

/* A GPIO pin, or nothing at all. */
static void format_pin(uint32_t value, bool imperial, char *out, size_t out_len) {
    (void)imperial;
    if (value == 0U) {
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_VALUE_PIN_UNSET));
    } else {
        inkcell_str_format(out, out_len, MESH_STR_VALUE_PIN, (unsigned)value);
    }
}

/* Signed dBm, read back out of the uint32_t the preset table stores it in. */
static void format_rssi(uint32_t value, bool imperial, char *out, size_t out_len) {
    (void)imperial;
    inkcell_str_format(out, out_len, MESH_STR_VALUE_DBM, (int)(int32_t)value);
}

/* A plain 0-255 level, so an LED channel does not read as a duration. */
static void format_level(uint32_t value, bool imperial, char *out, size_t out_len) {
    (void)imperial;
    inkcell_str_format(out, out_len, MESH_STR_VALUE_PLAIN, (unsigned)value);
}

/* Milliamps, for the LED current row. */
static void format_milliamps(uint32_t value, bool imperial, char *out, size_t out_len) {
    (void)imperial;
    inkcell_str_format(out, out_len, MESH_STR_VALUE_MILLIAMPS, (unsigned)value);
}

/* NUMBER fields whose value is a count rather than a duration; without this the seconds
   formatter would render 100 records as "1m40s". */
static void format_count(uint32_t value, bool imperial, char *out, size_t out_len) {
    (void)imperial;
    if (value == 0U) {
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_COMMON_DEFAULT));
    } else {
        inkcell_str_format(out, out_len, MESH_STR_VALUE_PLAIN, (unsigned)value);
    }
}

/*
 * A NUMBER field's presets, and what its numbers *are*.
 *
 * SCALE_PRESETS: they measure something - seconds, metres, packets, dBm - so where a value sits
 * between the ends of the list is a fact about it, and a row may draw that as a length.
 *
 * SCALE_PRESETS_AFTER_ZERO: all but the first. These lists open with a 0 the field reads as a
 * word - "default" on most of them, "max" on LoRa's transmit power - and a word is not a
 * quantity: the scale is what follows it, and 0 is off the track rather than at the bottom of
 * it. The distinction is not pedantic. Transmit power drawn the other way put "max" at the
 * empty end of its own bar.
 *
 * NAMED_PRESETS: they name something that happens to be written as a number - a GPIO pin, a
 * spreading factor, a bandwidth, a count of coordinate bits. Stepping them is still the right
 * way to edit them; drawing one as a length is a claim about magnitude that none of them makes.
 *
 * Every field states which, because nothing in a preset list says it: {0, 1, ... 7} is a hop
 * limit under MESH_UI_FIELD_LORA_HOPS and a pin under MESH_UI_FIELD_DETECT_PIN, and a leading 0
 * is "never" on one field and "the firmware decides" on the next.
 */
#define SCALE_PRESETS(array)                                                                       \
    { (array), INKWELL_ARRAY_LEN(array), true, false }
/* The same, for a list whose leading 0 is "the firmware's own default" or "as much as this
   radio has" rather than the bottom of the scale: the track spans what follows it. */
#define SCALE_PRESETS_AFTER_ZERO(array)                                                            \
    { (array), INKWELL_ARRAY_LEN(array), true, true }
#define NAMED_PRESETS(array)                                                                       \
    { (array), INKWELL_ARRAY_LEN(array), false, false }
#define NO_PRESETS                                                                                 \
    { NULL, 0U, false, false }

/*
 * Two of the limits in settings_text.def are the same number as a constant declared elsewhere,
 * and are written out there because that file is read from mesh/ui/nav.h, which sits below the
 * headers those constants live in. Held together here, where both are in scope, so a change to
 * either side fails the build instead of quietly shortening a row.
 */
_Static_assert(MESH_UI_TEXT_LIMIT_CANNED_0 + 1U == MESH_UI_CANNED_SLOT_MAX,
               "a canned slot's typed limit is its wire slot less the NUL");
_Static_assert(MESH_UI_TEXT_LIMIT_CHANNEL_KEY == 2U * MESH_UI_PSK_MAX &&
                   MESH_UI_TEXT_LIMIT_SECURITY_PRIVATE_KEY == 2U * MESH_UI_PSK_MAX,
               "a key is typed as two hex characters per byte");

/*
 * Seventeen ModemPresets plus the value that stands for none of them, which is what lets an
 * `optional` wire field be an ordinary contiguous enum row. The LoRa section's own preset row
 * is the same table without that extra value, because a running preset is never absent.
 */
#define BEACON_PRESET_VALUES 18U

/*
 * One broadcast target's three rows, written once and expanded four times.
 *
 * The four targets differ in nothing but which record they are: the same labels and the same
 * values. Writing them out would be twelve rows of which nine are copies, and a copy is where
 * the fourth one quietly ends up pointing at the third one's field. The token paste is what
 * makes the run contiguous in the order MESH_UI_FIELD_GROUP_BEACON_TARGETS walks.
 *
 * None of the twelve carries a help note, and that is Telemetry's five `Enabled` rows being
 * right rather than an omission: a help topic is a flat list of labels, so four rows called
 * `Region` would be four paragraphs a reader cannot tell apart. What a target is belongs to the
 * section and is said once in its overview.
 */
#define BEACON_TARGET_ROWS(n)                                                                      \
    [MESH_UI_FIELD_BEACON_TARGET_##n##                                                             \
        _PRESET] = {{MESH_STR_SETTINGS_FIELD_BEACON_TARGET_PRESET, INKSTAND_FORM_ENUM,             \
                     MESH_UI_SETTINGS_BEACON, BEACON_PRESET_VALUES, beacon_target_preset_name,     \
                     NO_PRESETS, 0U, INKCELL_STR_NONE},                                            \
                    INKCELL_STR_NONE,                                                              \
                    NULL},                                                                         \
   [MESH_UI_FIELD_BEACON_TARGET_##n##                                                              \
       _REGION] = {{MESH_STR_SETTINGS_FIELD_BEACON_TARGET_REGION, INKSTAND_FORM_ENUM,              \
                    MESH_UI_SETTINGS_BEACON, 38U, beacon_target_region_name, NO_PRESETS, 0U,       \
                    INKCELL_STR_NONE},                                                             \
                   INKCELL_STR_NONE,                                                               \
                   NULL},                                                                          \
   [MESH_UI_FIELD_BEACON_TARGET_##n##                                                              \
       _CHANNEL] = {{MESH_STR_SETTINGS_FIELD_BEACON_TARGET_CHANNEL, INKSTAND_FORM_NUMBER,          \
                     MESH_UI_SETTINGS_BEACON, 0U, NULL, NAMED_PRESETS(k_beacon_channel_presets),   \
                     0U, INKCELL_STR_NONE},                                                        \
                    MESH_STR_ENUM_BEACON_AS_RUNNING,                                               \
                    format_beacon_channel}

static const struct field_spec k_fields[MESH_UI_FIELD_COUNT] = {
    [MESH_UI_FIELD_NONE] = {{INKCELL_STR_COMMON_UNKNOWN_SHORT, INKSTAND_FORM_INFO,
                             MESH_UI_SETTINGS_SECTION_COUNT, 0U, NULL, NO_PRESETS, 0U,
                             INKCELL_STR_NONE},
                            INKCELL_STR_NONE,
                            NULL},
    [MESH_UI_FIELD_USER_LONG_NAME] = {{MESH_STR_SETTINGS_FIELD_USER_LONG_NAME, INKSTAND_FORM_TEXT,
                                       MESH_UI_SETTINGS_USER, MESH_UI_TEXT_LIMIT_USER_LONG_NAME,
                                       NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
                                      INKCELL_STR_NONE,
                                      NULL},
    [MESH_UI_FIELD_USER_SHORT_NAME] = {{MESH_STR_SETTINGS_FIELD_USER_SHORT_NAME, INKSTAND_FORM_TEXT,
                                        MESH_UI_SETTINGS_USER, MESH_UI_TEXT_LIMIT_USER_SHORT_NAME,
                                        NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
                                       INKCELL_STR_NONE,
                                       NULL},
    [MESH_UI_FIELD_USER_LICENSED] = {{MESH_STR_SETTINGS_FIELD_USER_LICENSED, INKSTAND_FORM_TOGGLE,
                                      MESH_UI_SETTINGS_USER, 0U, NULL, NO_PRESETS, 0U,
                                      MESH_STR_SETTINGS_NOTE_USER_LICENSED},
                                     INKCELL_STR_NONE,
                                     NULL},
    [MESH_UI_FIELD_USER_UNMESSAGEABLE] = {{MESH_STR_SETTINGS_FIELD_USER_UNMESSAGEABLE,
                                           INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_USER, 0U, NULL,
                                           NO_PRESETS, 0U,
                                           MESH_STR_SETTINGS_NOTE_USER_UNMESSAGEABLE},
                                          INKCELL_STR_NONE,
                                          NULL},
    /* Thirteen values, two of them retired but still steppable: see role_enum_name(). */
    [MESH_UI_FIELD_DEVICE_ROLE] = {{MESH_STR_SETTINGS_FIELD_DEVICE_ROLE, INKSTAND_FORM_ENUM,
                                    MESH_UI_SETTINGS_DEVICE, 13U, role_enum_name, NO_PRESETS, 0U,
                                    MESH_STR_SETTINGS_NOTE_DEVICE_ROLE},
                                   INKCELL_STR_NONE,
                                   NULL},
    /* The radio applies tzdef to its own clock only; it has no bearing on what this client
       shows, which follows the Brick's own TZ. */
    [MESH_UI_FIELD_DEVICE_TZDEF] = {{MESH_STR_SETTINGS_FIELD_DEVICE_TZDEF, INKSTAND_FORM_TEXT,
                                     MESH_UI_SETTINGS_DEVICE, MESH_UI_TEXT_LIMIT_DEVICE_TZDEF, NULL,
                                     NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_DEVICE_TZDEF},
                                    INKCELL_STR_NONE,
                                    NULL},
    [MESH_UI_FIELD_DEVICE_REBROADCAST] = {{MESH_STR_SETTINGS_FIELD_DEVICE_REBROADCAST,
                                           INKSTAND_FORM_ENUM, MESH_UI_SETTINGS_DEVICE, 6U,
                                           rebroadcast_name, NO_PRESETS, 0U,
                                           MESH_STR_SETTINGS_NOTE_DEVICE_REBROADCAST},
                                          INKCELL_STR_NONE,
                                          NULL},
    [MESH_UI_FIELD_DEVICE_NODEINFO_SECS] = {{MESH_STR_SETTINGS_FIELD_DEVICE_NODEINFO_SECS,
                                             INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_DEVICE, 0U,
                                             NULL, SCALE_PRESETS_AFTER_ZERO(k_nodeinfo_presets), 0U,
                                             MESH_STR_SETTINGS_NOTE_DEVICE_NODEINFO},
                                            MESH_STR_ZERO_DEFAULT,
                                            NULL},
    [MESH_UI_FIELD_DEVICE_LED_HEARTBEAT] = {{MESH_STR_SETTINGS_FIELD_DEVICE_LED_HEARTBEAT,
                                             INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_DEVICE, 0U,
                                             NULL, NO_PRESETS, 0U,
                                             MESH_STR_SETTINGS_NOTE_DEVICE_LED},
                                            INKCELL_STR_NONE,
                                            NULL},
    [MESH_UI_FIELD_DEVICE_DOUBLE_TAP] = {{MESH_STR_SETTINGS_FIELD_DEVICE_DOUBLE_TAP,
                                          INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_DEVICE, 0U, NULL,
                                          NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_DEVICE_DOUBLE_TAP},
                                         INKCELL_STR_NONE,
                                         NULL},
    [MESH_UI_FIELD_POSITION_GPS_MODE] = {{MESH_STR_SETTINGS_FIELD_POSITION_GPS_MODE,
                                          INKSTAND_FORM_ENUM, MESH_UI_SETTINGS_POSITION, 3U,
                                          gps_mode_name, NO_PRESETS, 0U,
                                          MESH_STR_SETTINGS_NOTE_POSITION_GPS_MODE},
                                         INKCELL_STR_NONE,
                                         NULL},
    [MESH_UI_FIELD_POSITION_BROADCAST_SECS] = {{MESH_STR_SETTINGS_FIELD_POSITION_BROADCAST_SECS,
                                                INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_POSITION, 0U,
                                                NULL, SCALE_PRESETS_AFTER_ZERO(k_interval_presets),
                                                0U, MESH_STR_SETTINGS_NOTE_POSITION_BROADCAST},
                                               MESH_STR_ZERO_DEFAULT,
                                               NULL},
    [MESH_UI_FIELD_POSITION_SMART] = {{MESH_STR_SETTINGS_FIELD_POSITION_SMART, INKSTAND_FORM_TOGGLE,
                                       MESH_UI_SETTINGS_POSITION, 0U, NULL, NO_PRESETS, 0U,
                                       MESH_STR_SETTINGS_NOTE_POSITION_SMART},
                                      INKCELL_STR_NONE,
                                      NULL},
    [MESH_UI_FIELD_POSITION_SMART_DISTANCE] = {{MESH_STR_SETTINGS_FIELD_POSITION_SMART_DISTANCE,
                                                INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_POSITION, 0U,
                                                NULL, SCALE_PRESETS(k_smart_distance_presets), 0U,
                                                MESH_STR_SETTINGS_NOTE_POSITION_SMART_DISTANCE},
                                               INKCELL_STR_NONE,
                                               format_metres},
    [MESH_UI_FIELD_POSITION_SMART_INTERVAL] = {{MESH_STR_SETTINGS_FIELD_POSITION_SMART_INTERVAL,
                                                INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_POSITION, 0U,
                                                NULL,
                                                SCALE_PRESETS_AFTER_ZERO(k_smart_interval_presets),
                                                0U, MESH_STR_SETTINGS_NOTE_POSITION_SMART_INTERVAL},
                                               MESH_STR_ZERO_DEFAULT,
                                               NULL},
    [MESH_UI_FIELD_POSITION_GPS_INTERVAL] =
        {{MESH_STR_SETTINGS_FIELD_POSITION_GPS_INTERVAL, INKSTAND_FORM_NUMBER,
          MESH_UI_SETTINGS_POSITION, 0U, NULL, SCALE_PRESETS_AFTER_ZERO(k_gps_interval_presets), 0U,
          MESH_STR_SETTINGS_NOTE_POSITION_GPS_INTERVAL},
         MESH_STR_ZERO_DEFAULT,
         NULL},
    /*
     * PositionConfig.position_flags, ten rows over one word. `limit` is the row's own bit,
     * written as a literal because the UI layer is the nanopb-free side of the fence - the
     * trade src/ui/settings/settings.c already makes for the excluded-modules mask - and every one
     * of them is pinned against meshtastic_Config_PositionConfig_PositionFlags by a test.
     */
    [MESH_UI_FIELD_POSITION_FLAG_ALTITUDE] = {{MESH_STR_SETTINGS_FIELD_POSITION_FLAG_ALTITUDE,
                                               INKSTAND_FORM_FLAG, MESH_UI_SETTINGS_POSITION,
                                               0x0001U, NULL, NO_PRESETS, 0U,
                                               MESH_STR_SETTINGS_NOTE_POSITION_FLAG_ALTITUDE},
                                              INKCELL_STR_NONE,
                                              NULL},
    [MESH_UI_FIELD_POSITION_FLAG_ALTITUDE_MSL] =
        {{MESH_STR_SETTINGS_FIELD_POSITION_FLAG_ALTITUDE_MSL, INKSTAND_FORM_FLAG,
          MESH_UI_SETTINGS_POSITION, 0x0002U, NULL, NO_PRESETS, 0U,
          MESH_STR_SETTINGS_NOTE_POSITION_FLAG_ALTITUDE_MSL},
         INKCELL_STR_NONE,
         NULL},
    [MESH_UI_FIELD_POSITION_FLAG_GEOIDAL] = {{MESH_STR_SETTINGS_FIELD_POSITION_FLAG_GEOIDAL,
                                              INKSTAND_FORM_FLAG, MESH_UI_SETTINGS_POSITION,
                                              0x0004U, NULL, NO_PRESETS, 0U,
                                              MESH_STR_SETTINGS_NOTE_POSITION_FLAG_GEOIDAL},
                                             INKCELL_STR_NONE,
                                             NULL},
    [MESH_UI_FIELD_POSITION_FLAG_DOP] = {{MESH_STR_SETTINGS_FIELD_POSITION_FLAG_DOP,
                                          INKSTAND_FORM_FLAG, MESH_UI_SETTINGS_POSITION, 0x0008U,
                                          NULL, NO_PRESETS, 0U,
                                          MESH_STR_SETTINGS_NOTE_POSITION_FLAG_DOP},
                                         INKCELL_STR_NONE,
                                         NULL},
    [MESH_UI_FIELD_POSITION_FLAG_HVDOP] = {{MESH_STR_SETTINGS_FIELD_POSITION_FLAG_HVDOP,
                                            INKSTAND_FORM_FLAG, MESH_UI_SETTINGS_POSITION, 0x0010U,
                                            NULL, NO_PRESETS, 0U,
                                            MESH_STR_SETTINGS_NOTE_POSITION_FLAG_HVDOP},
                                           INKCELL_STR_NONE,
                                           NULL},
    [MESH_UI_FIELD_POSITION_FLAG_SATINVIEW] = {{MESH_STR_SETTINGS_FIELD_POSITION_FLAG_SATINVIEW,
                                                INKSTAND_FORM_FLAG, MESH_UI_SETTINGS_POSITION,
                                                0x0020U, NULL, NO_PRESETS, 0U,
                                                MESH_STR_SETTINGS_NOTE_POSITION_FLAG_SATINVIEW},
                                               INKCELL_STR_NONE,
                                               NULL},
    [MESH_UI_FIELD_POSITION_FLAG_SEQ_NO] = {{MESH_STR_SETTINGS_FIELD_POSITION_FLAG_SEQ_NO,
                                             INKSTAND_FORM_FLAG, MESH_UI_SETTINGS_POSITION, 0x0040U,
                                             NULL, NO_PRESETS, 0U,
                                             MESH_STR_SETTINGS_NOTE_POSITION_FLAG_SEQ_NO},
                                            INKCELL_STR_NONE,
                                            NULL},
    [MESH_UI_FIELD_POSITION_FLAG_TIMESTAMP] = {{MESH_STR_SETTINGS_FIELD_POSITION_FLAG_TIMESTAMP,
                                                INKSTAND_FORM_FLAG, MESH_UI_SETTINGS_POSITION,
                                                0x0080U, NULL, NO_PRESETS, 0U,
                                                MESH_STR_SETTINGS_NOTE_POSITION_FLAG_TIMESTAMP},
                                               INKCELL_STR_NONE,
                                               NULL},
    [MESH_UI_FIELD_POSITION_FLAG_HEADING] = {{MESH_STR_SETTINGS_FIELD_POSITION_FLAG_HEADING,
                                              INKSTAND_FORM_FLAG, MESH_UI_SETTINGS_POSITION,
                                              0x0100U, NULL, NO_PRESETS, 0U,
                                              MESH_STR_SETTINGS_NOTE_POSITION_FLAG_HEADING},
                                             INKCELL_STR_NONE,
                                             NULL},
    [MESH_UI_FIELD_POSITION_FLAG_SPEED] = {{MESH_STR_SETTINGS_FIELD_POSITION_FLAG_SPEED,
                                            INKSTAND_FORM_FLAG, MESH_UI_SETTINGS_POSITION, 0x0200U,
                                            NULL, NO_PRESETS, 0U,
                                            MESH_STR_SETTINGS_NOTE_POSITION_FLAG_SPEED},
                                           INKCELL_STR_NONE,
                                           NULL},
    [MESH_UI_FIELD_POSITION_LATITUDE] = {{MESH_STR_SETTINGS_FIELD_POSITION_LATITUDE,
                                          INKSTAND_FORM_TEXT, MESH_UI_SETTINGS_POSITION,
                                          MESH_UI_TEXT_LIMIT_POSITION_LATITUDE, NULL, NO_PRESETS,
                                          0U, MESH_STR_SETTINGS_NOTE_POSITION_FIXED},
                                         INKCELL_STR_NONE,
                                         NULL},
    [MESH_UI_FIELD_POSITION_LONGITUDE] = {{MESH_STR_SETTINGS_FIELD_POSITION_LONGITUDE,
                                           INKSTAND_FORM_TEXT, MESH_UI_SETTINGS_POSITION,
                                           MESH_UI_TEXT_LIMIT_POSITION_LONGITUDE, NULL, NO_PRESETS,
                                           0U, INKCELL_STR_NONE},
                                          INKCELL_STR_NONE,
                                          NULL},
    /*
     * The one length in the client that stays metric whatever DisplayConfig.units says, and it
     * says so in its own label: "Altitude (m)".
     *
     * It is typed rather than read. The value goes to the radio as whole metres, so wording the
     * box in feet would mean parsing feet and converting back on every write - and an integer
     * round trip through 0.3048 does not land where it started, which is a fixed position that
     * drifts a metre each time somebody opens the row and backs out of it. A reading can be
     * reworded for free; an edit cannot, and a label that states its unit is the honest version.
     */
    [MESH_UI_FIELD_POSITION_ALTITUDE] = {{MESH_STR_SETTINGS_FIELD_POSITION_ALTITUDE,
                                          INKSTAND_FORM_TEXT, MESH_UI_SETTINGS_POSITION,
                                          MESH_UI_TEXT_LIMIT_POSITION_ALTITUDE, NULL, NO_PRESETS,
                                          0U, INKCELL_STR_NONE},
                                         INKCELL_STR_NONE,
                                         NULL},
    [MESH_UI_FIELD_POWER_SAVING] = {{MESH_STR_SETTINGS_FIELD_POWER_SAVING, INKSTAND_FORM_TOGGLE,
                                     MESH_UI_SETTINGS_POWER, 0U, NULL, NO_PRESETS, 0U,
                                     MESH_STR_SETTINGS_NOTE_POWER_SAVING},
                                    INKCELL_STR_NONE,
                                    NULL},
    [MESH_UI_FIELD_POWER_LS_SECS] = {{MESH_STR_SETTINGS_FIELD_POWER_LS_SECS, INKSTAND_FORM_NUMBER,
                                      MESH_UI_SETTINGS_POWER, 0U, NULL,
                                      SCALE_PRESETS_AFTER_ZERO(k_sleep_presets), 0U,
                                      MESH_STR_SETTINGS_NOTE_POWER_LS_SECS},
                                     MESH_STR_ZERO_DEFAULT,
                                     NULL},
    [MESH_UI_FIELD_POWER_MIN_WAKE] = {{MESH_STR_SETTINGS_FIELD_POWER_MIN_WAKE, INKSTAND_FORM_NUMBER,
                                       MESH_UI_SETTINGS_POWER, 0U, NULL,
                                       SCALE_PRESETS_AFTER_ZERO(k_wake_presets), 0U,
                                       MESH_STR_SETTINGS_NOTE_POWER_MIN_WAKE},
                                      MESH_STR_ZERO_DEFAULT,
                                      NULL},
    [MESH_UI_FIELD_POWER_WAIT_BT] = {{MESH_STR_SETTINGS_FIELD_POWER_WAIT_BT, INKSTAND_FORM_NUMBER,
                                      MESH_UI_SETTINGS_POWER, 0U, NULL,
                                      SCALE_PRESETS_AFTER_ZERO(k_wait_bt_presets), 0U,
                                      MESH_STR_SETTINGS_NOTE_POWER_WAIT_BT},
                                     MESH_STR_ZERO_DEFAULT,
                                     NULL},
    [MESH_UI_FIELD_POWER_SHUTDOWN] = {{MESH_STR_SETTINGS_FIELD_POWER_SHUTDOWN, INKSTAND_FORM_NUMBER,
                                       MESH_UI_SETTINGS_POWER, 0U, NULL,
                                       SCALE_PRESETS(k_shutdown_presets), 0U,
                                       MESH_STR_SETTINGS_NOTE_POWER_SHUTDOWN},
                                      MESH_STR_ZERO_OFF,
                                      NULL},
    [MESH_UI_FIELD_DISPLAY_SCREEN_ON] = {{MESH_STR_SETTINGS_FIELD_DISPLAY_SCREEN_ON,
                                          INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_DISPLAY, 0U, NULL,
                                          SCALE_PRESETS_AFTER_ZERO(k_screen_on_presets), 0U,
                                          MESH_STR_SETTINGS_NOTE_DISPLAY_SCREEN_ON},
                                         MESH_STR_ZERO_DEFAULT,
                                         NULL},
    [MESH_UI_FIELD_DISPLAY_CAROUSEL] = {{MESH_STR_SETTINGS_FIELD_DISPLAY_CAROUSEL,
                                         INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_DISPLAY, 0U, NULL,
                                         SCALE_PRESETS(k_carousel_presets), 0U,
                                         MESH_STR_SETTINGS_NOTE_DISPLAY_CAROUSEL},
                                        MESH_STR_ZERO_OFF,
                                        NULL},
    [MESH_UI_FIELD_DISPLAY_COMPASS] = {{MESH_STR_SETTINGS_FIELD_DISPLAY_COMPASS, INKSTAND_FORM_ENUM,
                                        MESH_UI_SETTINGS_DISPLAY, 8U, compass_name, NO_PRESETS, 0U,
                                        MESH_STR_SETTINGS_NOTE_DISPLAY_COMPASS},
                                       INKCELL_STR_NONE,
                                       NULL},
    [MESH_UI_FIELD_DISPLAY_12H] = {{MESH_STR_SETTINGS_FIELD_DISPLAY_12H, INKSTAND_FORM_TOGGLE,
                                    MESH_UI_SETTINGS_DISPLAY, 0U, NULL, NO_PRESETS, 0U,
                                    INKCELL_STR_NONE},
                                   INKCELL_STR_NONE,
                                   NULL},
    /* The one Display row this client reads for itself: see src/ui/tables/units.c. */
    [MESH_UI_FIELD_DISPLAY_UNITS] = {{MESH_STR_SETTINGS_FIELD_DISPLAY_UNITS, INKSTAND_FORM_ENUM,
                                      MESH_UI_SETTINGS_DISPLAY, 2U, units_name, NO_PRESETS, 0U,
                                      MESH_STR_SETTINGS_NOTE_DISPLAY_UNITS},
                                     INKCELL_STR_NONE,
                                     NULL},
    [MESH_UI_FIELD_DISPLAY_FLIP] = {{MESH_STR_SETTINGS_FIELD_DISPLAY_FLIP, INKSTAND_FORM_TOGGLE,
                                     MESH_UI_SETTINGS_DISPLAY, 0U, NULL, NO_PRESETS, 0U,
                                     MESH_STR_SETTINGS_NOTE_DISPLAY_FLIP},
                                    INKCELL_STR_NONE,
                                    NULL},
    [MESH_UI_FIELD_DISPLAY_OLED] = {{MESH_STR_SETTINGS_FIELD_DISPLAY_OLED, INKSTAND_FORM_ENUM,
                                     MESH_UI_SETTINGS_DISPLAY, 6U, oled_name, NO_PRESETS, 0U,
                                     MESH_STR_SETTINGS_NOTE_DISPLAY_OLED},
                                    INKCELL_STR_NONE,
                                    NULL},
    [MESH_UI_FIELD_DISPLAY_MODE] = {{MESH_STR_SETTINGS_FIELD_DISPLAY_MODE, INKSTAND_FORM_ENUM,
                                     MESH_UI_SETTINGS_DISPLAY, 4U, displaymode_name, NO_PRESETS, 0U,
                                     MESH_STR_SETTINGS_NOTE_DISPLAY_MODE},
                                    INKCELL_STR_NONE,
                                    NULL},
    [MESH_UI_FIELD_DISPLAY_HEADING_BOLD] = {{MESH_STR_SETTINGS_FIELD_DISPLAY_HEADING_BOLD,
                                             INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_DISPLAY, 0U,
                                             NULL, NO_PRESETS, 0U,
                                             MESH_STR_SETTINGS_NOTE_DISPLAY_HEADING_BOLD},
                                            INKCELL_STR_NONE,
                                            NULL},
    [MESH_UI_FIELD_DISPLAY_WAKE_ON_MOTION] = {{MESH_STR_SETTINGS_FIELD_DISPLAY_WAKE_ON_MOTION,
                                               INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_DISPLAY, 0U,
                                               NULL, NO_PRESETS, 0U,
                                               MESH_STR_SETTINGS_NOTE_DISPLAY_WAKE_ON_MOTION},
                                              INKCELL_STR_NONE,
                                              NULL},
    [MESH_UI_FIELD_DISPLAY_LONG_NAMES] = {{MESH_STR_SETTINGS_FIELD_DISPLAY_LONG_NAMES,
                                           INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_DISPLAY, 0U, NULL,
                                           NO_PRESETS, 0U,
                                           MESH_STR_SETTINGS_NOTE_DISPLAY_LONG_NAMES},
                                          INKCELL_STR_NONE,
                                          NULL},
    [MESH_UI_FIELD_DISPLAY_MESSAGE_BUBBLES] = {{MESH_STR_SETTINGS_FIELD_DISPLAY_MESSAGE_BUBBLES,
                                                INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_DISPLAY, 0U,
                                                NULL, NO_PRESETS, 0U,
                                                MESH_STR_SETTINGS_NOTE_DISPLAY_MESSAGE_BUBBLES},
                                               INKCELL_STR_NONE,
                                               NULL},
    [MESH_UI_FIELD_MQTT_ENABLED] = {{MESH_STR_SETTINGS_FIELD_MQTT_ENABLED, INKSTAND_FORM_TOGGLE,
                                     MESH_UI_SETTINGS_MQTT, 0U, NULL, NO_PRESETS, 0U,
                                     MESH_STR_SETTINGS_NOTE_MQTT_ENABLED},
                                    INKCELL_STR_NONE,
                                    NULL},
    [MESH_UI_FIELD_MQTT_ADDRESS] = {{MESH_STR_SETTINGS_FIELD_MQTT_ADDRESS, INKSTAND_FORM_TEXT,
                                     MESH_UI_SETTINGS_MQTT, MESH_UI_TEXT_LIMIT_MQTT_ADDRESS, NULL,
                                     NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_MQTT_ADDRESS},
                                    INKCELL_STR_NONE,
                                    NULL},
    [MESH_UI_FIELD_MQTT_USERNAME] = {{MESH_STR_SETTINGS_FIELD_MQTT_USERNAME, INKSTAND_FORM_TEXT,
                                      MESH_UI_SETTINGS_MQTT, MESH_UI_TEXT_LIMIT_MQTT_USERNAME, NULL,
                                      NO_PRESETS, 0U, INKCELL_STR_NONE},
                                     INKCELL_STR_NONE,
                                     NULL},
    [MESH_UI_FIELD_MQTT_PASSWORD] = {{MESH_STR_SETTINGS_FIELD_MQTT_PASSWORD, INKSTAND_FORM_TEXT,
                                      MESH_UI_SETTINGS_MQTT, MESH_UI_TEXT_LIMIT_MQTT_PASSWORD, NULL,
                                      NO_PRESETS, 0U, INKCELL_STR_NONE},
                                     INKCELL_STR_NONE,
                                     NULL},
    [MESH_UI_FIELD_MQTT_ROOT] = {{MESH_STR_SETTINGS_FIELD_MQTT_ROOT, INKSTAND_FORM_TEXT,
                                  MESH_UI_SETTINGS_MQTT, MESH_UI_TEXT_LIMIT_MQTT_ROOT, NULL,
                                  NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_MQTT_ROOT},
                                 INKCELL_STR_NONE,
                                 NULL},
    [MESH_UI_FIELD_MQTT_PROXY] = {{MESH_STR_SETTINGS_FIELD_MQTT_PROXY, INKSTAND_FORM_TOGGLE,
                                   MESH_UI_SETTINGS_MQTT, 0U, NULL, NO_PRESETS, 0U,
                                   MESH_STR_SETTINGS_NOTE_MQTT_PROXY},
                                  INKCELL_STR_NONE,
                                  NULL},
    [MESH_UI_FIELD_MQTT_ENCRYPTION] = {{MESH_STR_SETTINGS_FIELD_MQTT_ENCRYPTION,
                                        INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                        NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_MQTT_ENCRYPTION},
                                       INKCELL_STR_NONE,
                                       NULL},
    [MESH_UI_FIELD_MQTT_TLS] = {{MESH_STR_SETTINGS_FIELD_MQTT_TLS, INKSTAND_FORM_TOGGLE,
                                 MESH_UI_SETTINGS_MQTT, 0U, NULL, NO_PRESETS, 0U,
                                 MESH_STR_SETTINGS_NOTE_MQTT_TLS},
                                INKCELL_STR_NONE,
                                NULL},
    /* Spelled out: this one publishes the node's position to a public map, which is not what
       "map reporting" reads as to somebody stepping through toggles. */
    [MESH_UI_FIELD_MQTT_MAP_REPORTING] = {{MESH_STR_SETTINGS_FIELD_MQTT_MAP_REPORTING,
                                           INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                           NO_PRESETS, 0U,
                                           MESH_STR_SETTINGS_NOTE_MQTT_MAP_REPORTING},
                                          INKCELL_STR_NONE,
                                          NULL},
    [MESH_UI_FIELD_MQTT_MAP_INTERVAL] = {{MESH_STR_SETTINGS_FIELD_MQTT_MAP_INTERVAL,
                                          INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                          SCALE_PRESETS(k_map_interval_presets), 0U,
                                          MESH_STR_SETTINGS_NOTE_MQTT_MAP_INTERVAL},
                                         INKCELL_STR_NONE,
                                         NULL},
    [MESH_UI_FIELD_MQTT_MAP_PRECISION] = {{MESH_STR_SETTINGS_FIELD_MQTT_MAP_PRECISION,
                                           INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                           NAMED_PRESETS(k_precision_presets), 0U,
                                           MESH_STR_SETTINGS_NOTE_MQTT_MAP_PRECISION},
                                          MESH_STR_ZERO_OFF,
                                          mesh_ui_settings_format_precision},
    [MESH_UI_FIELD_MQTT_MAP_LOCATION] = {{MESH_STR_SETTINGS_FIELD_MQTT_MAP_LOCATION,
                                          INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_MQTT, 0U, NULL,
                                          NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_MQTT_MAP_LOCATION},
                                         INKCELL_STR_NONE,
                                         NULL},
    [MESH_UI_FIELD_SF_ENABLED] = {{MESH_STR_SETTINGS_FIELD_SF_ENABLED, INKSTAND_FORM_TOGGLE,
                                   MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NO_PRESETS, 0U,
                                   MESH_STR_SETTINGS_NOTE_SF_ENABLED},
                                  INKCELL_STR_NONE,
                                  NULL},
    [MESH_UI_FIELD_SF_HEARTBEAT] = {{MESH_STR_SETTINGS_FIELD_SF_HEARTBEAT, INKSTAND_FORM_TOGGLE,
                                     MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NO_PRESETS, 0U,
                                     MESH_STR_SETTINGS_NOTE_SF_HEARTBEAT},
                                    INKCELL_STR_NONE,
                                    NULL},
    [MESH_UI_FIELD_SF_SERVER] = {{MESH_STR_SETTINGS_FIELD_SF_SERVER, INKSTAND_FORM_TOGGLE,
                                  MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL, NO_PRESETS, 0U,
                                  MESH_STR_SETTINGS_NOTE_SF_SERVER},
                                 INKCELL_STR_NONE,
                                 NULL},
    [MESH_UI_FIELD_SF_RECORDS] = {{MESH_STR_SETTINGS_FIELD_SF_RECORDS, INKSTAND_FORM_NUMBER,
                                   MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL,
                                   SCALE_PRESETS_AFTER_ZERO(k_sf_records_presets), 0U,
                                   MESH_STR_SETTINGS_NOTE_SF_RECORDS},
                                  INKCELL_STR_NONE,
                                  format_count},
    [MESH_UI_FIELD_SF_HISTORY_MAX] = {{MESH_STR_SETTINGS_FIELD_SF_HISTORY_MAX, INKSTAND_FORM_NUMBER,
                                       MESH_UI_SETTINGS_STORE_FORWARD, 0U, NULL,
                                       SCALE_PRESETS_AFTER_ZERO(k_sf_history_presets), 0U,
                                       MESH_STR_SETTINGS_NOTE_SF_HISTORY_MAX},
                                      INKCELL_STR_NONE,
                                      format_count},
    [MESH_UI_FIELD_SF_HISTORY_WINDOW] = {{MESH_STR_SETTINGS_FIELD_SF_HISTORY_WINDOW,
                                          INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_STORE_FORWARD, 0U,
                                          NULL, SCALE_PRESETS_AFTER_ZERO(k_sf_window_presets), 0U,
                                          MESH_STR_SETTINGS_NOTE_SF_HISTORY_WINDOW},
                                         MESH_STR_ZERO_DEFAULT,
                                         NULL},
    /* The five telemetry groups each sit under a heading, so their rows are named for what
       they are inside the group rather than repeating it ("Enabled", not "Env enabled"). The
       only consumer of a field label outside the row list is the keyboard title, and none of
       these is a TEXT field. */
    [MESH_UI_FIELD_TELEMETRY_DEVICE] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_DEVICE,
                                         INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U, NULL,
                                         NO_PRESETS, 0U, INKCELL_STR_NONE},
                                        INKCELL_STR_NONE,
                                        NULL},
    [MESH_UI_FIELD_TELEMETRY_INTERVAL] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_INTERVAL,
                                           INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                           NULL, SCALE_PRESETS_AFTER_ZERO(k_interval_presets), 0U,
                                           INKCELL_STR_NONE},
                                          MESH_STR_ZERO_DEFAULT,
                                          NULL},
    [MESH_UI_FIELD_TELEMETRY_ENVIRONMENT] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_ENVIRONMENT,
                                              INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                              NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
                                             INKCELL_STR_NONE,
                                             NULL},
    [MESH_UI_FIELD_TELEMETRY_ENV_INTERVAL] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_ENV_INTERVAL,
                                               INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                               NULL, SCALE_PRESETS_AFTER_ZERO(k_interval_presets),
                                               0U, INKCELL_STR_NONE},
                                              MESH_STR_ZERO_DEFAULT,
                                              NULL},
    [MESH_UI_FIELD_TELEMETRY_ENV_SCREEN] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_ENV_SCREEN,
                                             INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                             NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
                                            INKCELL_STR_NONE,
                                            NULL},
    [MESH_UI_FIELD_TELEMETRY_ENV_FAHRENHEIT] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_ENV_FAHRENHEIT,
                                                 INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_TELEMETRY,
                                                 0U, NULL, NO_PRESETS, 0U,
                                                 MESH_STR_SETTINGS_NOTE_TELEMETRY_FAHRENHEIT},
                                                INKCELL_STR_NONE,
                                                NULL},
    [MESH_UI_FIELD_TELEMETRY_AIR_QUALITY] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_AIR_QUALITY,
                                              INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                              NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
                                             INKCELL_STR_NONE,
                                             NULL},
    [MESH_UI_FIELD_TELEMETRY_AIR_INTERVAL] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_AIR_INTERVAL,
                                               INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                               NULL, SCALE_PRESETS_AFTER_ZERO(k_interval_presets),
                                               0U, INKCELL_STR_NONE},
                                              MESH_STR_ZERO_DEFAULT,
                                              NULL},
    [MESH_UI_FIELD_TELEMETRY_AIR_SCREEN] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_AIR_SCREEN,
                                             INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                             NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
                                            INKCELL_STR_NONE,
                                            NULL},
    [MESH_UI_FIELD_TELEMETRY_POWER] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_POWER,
                                        INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U, NULL,
                                        NO_PRESETS, 0U, INKCELL_STR_NONE},
                                       INKCELL_STR_NONE,
                                       NULL},
    [MESH_UI_FIELD_TELEMETRY_POWER_INTERVAL] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_POWER_INTERVAL,
                                                 INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_TELEMETRY,
                                                 0U, NULL,
                                                 SCALE_PRESETS_AFTER_ZERO(k_interval_presets), 0U,
                                                 INKCELL_STR_NONE},
                                                MESH_STR_ZERO_DEFAULT,
                                                NULL},
    [MESH_UI_FIELD_TELEMETRY_POWER_SCREEN] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_POWER_SCREEN,
                                               INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U,
                                               NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
                                              INKCELL_STR_NONE,
                                              NULL},
    [MESH_UI_FIELD_TELEMETRY_HEALTH] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_HEALTH,
                                         INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_TELEMETRY, 0U, NULL,
                                         NO_PRESETS, 0U, INKCELL_STR_NONE},
                                        INKCELL_STR_NONE,
                                        NULL},
    [MESH_UI_FIELD_TELEMETRY_HEALTH_INTERVAL] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_HEALTH_INTERVAL,
                                                  INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_TELEMETRY,
                                                  0U, NULL,
                                                  SCALE_PRESETS_AFTER_ZERO(k_interval_presets), 0U,
                                                  INKCELL_STR_NONE},
                                                 MESH_STR_ZERO_DEFAULT,
                                                 NULL},
    [MESH_UI_FIELD_TELEMETRY_HEALTH_SCREEN] = {{MESH_STR_SETTINGS_FIELD_TELEMETRY_HEALTH_SCREEN,
                                                INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_TELEMETRY,
                                                0U, NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
                                               INKCELL_STR_NONE,
                                               NULL},
    [MESH_UI_FIELD_CHANNEL_NAME] = {{MESH_STR_SETTINGS_FIELD_CHANNEL_NAME, INKSTAND_FORM_TEXT,
                                     MESH_UI_SETTINGS_CHANNELS, MESH_UI_TEXT_LIMIT_CHANNEL_NAME,
                                     NULL, NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_CHANNEL_NAME},
                                    INKCELL_STR_NONE,
                                    NULL},
    [MESH_UI_FIELD_CHANNEL_ROLE] = {{MESH_STR_SETTINGS_FIELD_CHANNEL_ROLE, INKSTAND_FORM_ENUM,
                                     MESH_UI_SETTINGS_CHANNELS, 2U, channel_role_name, NO_PRESETS,
                                     0U, MESH_STR_SETTINGS_NOTE_CHANNEL_ROLE},
                                    INKCELL_STR_NONE,
                                    NULL},
    [MESH_UI_FIELD_CHANNEL_KEY] = {{MESH_STR_SETTINGS_FIELD_CHANNEL_KEY, INKSTAND_FORM_KEY,
                                    MESH_UI_SETTINGS_CHANNELS, MESH_UI_TEXT_LIMIT_CHANNEL_KEY, NULL,
                                    NO_PRESETS, CHANNEL_KEY_CHOICES,
                                    MESH_STR_SETTINGS_NOTE_CHANNEL_KEY},
                                   INKCELL_STR_NONE,
                                   NULL},
    [MESH_UI_FIELD_CHANNEL_UPLINK] = {{MESH_STR_SETTINGS_FIELD_CHANNEL_UPLINK, INKSTAND_FORM_TOGGLE,
                                       MESH_UI_SETTINGS_CHANNELS, 0U, NULL, NO_PRESETS, 0U,
                                       MESH_STR_SETTINGS_NOTE_CHANNEL_UPLINK},
                                      INKCELL_STR_NONE,
                                      NULL},
    [MESH_UI_FIELD_CHANNEL_DOWNLINK] = {{MESH_STR_SETTINGS_FIELD_CHANNEL_DOWNLINK,
                                         INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_CHANNELS, 0U, NULL,
                                         NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_CHANNEL_DOWNLINK},
                                        INKCELL_STR_NONE,
                                        NULL},
    [MESH_UI_FIELD_CHANNEL_POSITION] = {{MESH_STR_SETTINGS_FIELD_CHANNEL_POSITION,
                                         INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_CHANNELS, 0U, NULL,
                                         NAMED_PRESETS(k_precision_presets), 0U,
                                         MESH_STR_SETTINGS_NOTE_CHANNEL_POSITION},
                                        MESH_STR_ZERO_OFF,
                                        mesh_ui_settings_format_precision},
    [MESH_UI_FIELD_CHANNEL_MUTED] = {{MESH_STR_SETTINGS_FIELD_CHANNEL_MUTED, INKSTAND_FORM_TOGGLE,
                                      MESH_UI_SETTINGS_CHANNELS, 0U, NULL, NO_PRESETS, 0U,
                                      MESH_STR_SETTINGS_NOTE_CHANNEL_MUTED},
                                     INKCELL_STR_NONE,
                                     NULL},
    [MESH_UI_FIELD_BT_ENABLED] = {{MESH_STR_SETTINGS_FIELD_BT_ENABLED, INKSTAND_FORM_TOGGLE,
                                   MESH_UI_SETTINGS_BLUETOOTH, 0U, NULL, NO_PRESETS, 0U,
                                   INKCELL_STR_NONE},
                                  INKCELL_STR_NONE,
                                  NULL},
    [MESH_UI_FIELD_BT_MODE] = {{MESH_STR_SETTINGS_FIELD_BT_MODE, INKSTAND_FORM_ENUM,
                                MESH_UI_SETTINGS_BLUETOOTH, 3U, pairing_enum_name, NO_PRESETS, 0U,
                                MESH_STR_SETTINGS_NOTE_BT_MODE},
                               INKCELL_STR_NONE,
                               NULL},
    [MESH_UI_FIELD_BT_PIN] = {{MESH_STR_SETTINGS_FIELD_BT_PIN, INKSTAND_FORM_TEXT,
                               MESH_UI_SETTINGS_BLUETOOTH, MESH_UI_TEXT_LIMIT_BT_PIN, NULL,
                               NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_BT_PIN},
                              INKCELL_STR_NONE,
                              NULL},
    [MESH_UI_FIELD_LORA_REGION] = {{MESH_STR_SETTINGS_FIELD_LORA_REGION, INKSTAND_FORM_ENUM,
                                    MESH_UI_SETTINGS_LORA, 38U, region_enum_name, NO_PRESETS, 0U,
                                    MESH_STR_SETTINGS_NOTE_LORA_REGION},
                                   INKCELL_STR_NONE,
                                   NULL},
    [MESH_UI_FIELD_LORA_USE_PRESET] = {{MESH_STR_SETTINGS_FIELD_LORA_USE_PRESET,
                                        INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_LORA, 0U, NULL,
                                        NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_LORA_USE_PRESET},
                                       INKCELL_STR_NONE,
                                       NULL},
    [MESH_UI_FIELD_LORA_PRESET] = {{MESH_STR_SETTINGS_FIELD_LORA_PRESET, INKSTAND_FORM_ENUM,
                                    MESH_UI_SETTINGS_LORA, 17U, preset_enum_name, NO_PRESETS, 0U,
                                    MESH_STR_SETTINGS_NOTE_LORA_PRESET},
                                   INKCELL_STR_NONE,
                                   NULL},
    [MESH_UI_FIELD_LORA_BANDWIDTH] = {{MESH_STR_SETTINGS_FIELD_LORA_BANDWIDTH, INKSTAND_FORM_NUMBER,
                                       MESH_UI_SETTINGS_LORA, 0U, NULL,
                                       NAMED_PRESETS(k_bandwidth_presets), 0U,
                                       MESH_STR_SETTINGS_NOTE_LORA_BANDWIDTH},
                                      INKCELL_STR_NONE,
                                      format_bandwidth},
    [MESH_UI_FIELD_LORA_SPREAD] = {{MESH_STR_SETTINGS_FIELD_LORA_SPREAD, INKSTAND_FORM_NUMBER,
                                    MESH_UI_SETTINGS_LORA, 0U, NULL,
                                    NAMED_PRESETS(k_spread_presets), 0U,
                                    MESH_STR_SETTINGS_NOTE_LORA_SPREAD},
                                   INKCELL_STR_NONE,
                                   format_plain},
    [MESH_UI_FIELD_LORA_CODING] = {{MESH_STR_SETTINGS_FIELD_LORA_CODING, INKSTAND_FORM_NUMBER,
                                    MESH_UI_SETTINGS_LORA, 0U, NULL,
                                    NAMED_PRESETS(k_coding_presets), 0U,
                                    MESH_STR_SETTINGS_NOTE_LORA_CODING},
                                   INKCELL_STR_NONE,
                                   format_coding_rate},
    [MESH_UI_FIELD_LORA_HOPS] = {{MESH_STR_SETTINGS_FIELD_LORA_HOPS, INKSTAND_FORM_NUMBER,
                                  MESH_UI_SETTINGS_LORA, 0U, NULL, SCALE_PRESETS(k_hop_presets), 0U,
                                  MESH_STR_SETTINGS_NOTE_LORA_HOPS},
                                 INKCELL_STR_NONE,
                                 format_plain},
    [MESH_UI_FIELD_LORA_TX_ENABLED] = {{MESH_STR_SETTINGS_FIELD_LORA_TX_ENABLED,
                                        INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_LORA, 0U, NULL,
                                        NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_LORA_TX_ENABLED},
                                       INKCELL_STR_NONE,
                                       NULL},
    [MESH_UI_FIELD_LORA_TX_POWER] = {{MESH_STR_SETTINGS_FIELD_LORA_TX_POWER, INKSTAND_FORM_NUMBER,
                                      MESH_UI_SETTINGS_LORA, 0U, NULL,
                                      SCALE_PRESETS_AFTER_ZERO(k_tx_power_presets), 0U,
                                      MESH_STR_SETTINGS_NOTE_LORA_TX_POWER},
                                     INKCELL_STR_NONE,
                                     format_tx_power},
    [MESH_UI_FIELD_LORA_IGNORE_MQTT] = {{MESH_STR_SETTINGS_FIELD_LORA_IGNORE_MQTT,
                                         INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_LORA, 0U, NULL,
                                         NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_LORA_IGNORE_MQTT},
                                        INKCELL_STR_NONE,
                                        NULL},
    [MESH_UI_FIELD_LORA_OK_TO_MQTT] = {{MESH_STR_SETTINGS_FIELD_LORA_OK_TO_MQTT,
                                        INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_LORA, 0U, NULL,
                                        NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_LORA_OK_TO_MQTT},
                                       INKCELL_STR_NONE,
                                       NULL},
    [MESH_UI_FIELD_LORA_BOOST_GAIN] = {{MESH_STR_SETTINGS_FIELD_LORA_BOOST_GAIN,
                                        INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_LORA, 0U, NULL,
                                        NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_LORA_BOOST_GAIN},
                                       INKCELL_STR_NONE,
                                       NULL},
    [MESH_UI_FIELD_LORA_OVERRIDE_DUTY] = {{MESH_STR_SETTINGS_FIELD_LORA_OVERRIDE_DUTY,
                                           INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_LORA, 0U, NULL,
                                           NO_PRESETS, 0U,
                                           MESH_STR_SETTINGS_NOTE_LORA_OVERRIDE_DUTY},
                                          INKCELL_STR_NONE,
                                          NULL},
    /* The three typed numbers. TEXT rather than NUMBER because none of them has presets worth
       stepping: every slot of a region's band is as likely as every other, and a frequency has
       more values than a d-pad has patience. */
    [MESH_UI_FIELD_LORA_CHANNEL_NUM] = {{MESH_STR_SETTINGS_FIELD_LORA_CHANNEL_NUM,
                                         INKSTAND_FORM_TEXT, MESH_UI_SETTINGS_LORA,
                                         MESH_UI_TEXT_LIMIT_LORA_CHANNEL_NUM, NULL, NO_PRESETS, 0U,
                                         MESH_STR_SETTINGS_NOTE_LORA_CHANNEL_NUM},
                                        INKCELL_STR_NONE,
                                        NULL},
    [MESH_UI_FIELD_LORA_OVERRIDE_FREQ] = {{MESH_STR_SETTINGS_FIELD_LORA_OVERRIDE_FREQ,
                                           INKSTAND_FORM_TEXT, MESH_UI_SETTINGS_LORA,
                                           MESH_UI_TEXT_LIMIT_LORA_OVERRIDE_FREQ, NULL, NO_PRESETS,
                                           0U, MESH_STR_SETTINGS_NOTE_LORA_OVERRIDE_FREQ},
                                          INKCELL_STR_NONE,
                                          NULL},
    [MESH_UI_FIELD_LORA_FREQUENCY_TRIM] = {{MESH_STR_SETTINGS_FIELD_LORA_FREQUENCY_TRIM,
                                            INKSTAND_FORM_TEXT, MESH_UI_SETTINGS_LORA,
                                            MESH_UI_TEXT_LIMIT_LORA_FREQUENCY_TRIM, NULL,
                                            NO_PRESETS, 0U,
                                            MESH_STR_SETTINGS_NOTE_LORA_FREQUENCY_TRIM},
                                           INKCELL_STR_NONE,
                                           NULL},
    /* Three slots of one repeated field, and the note is on the first of them alone: what
       makes them worth explaining is that they are not the Nodes tab's Ignore, which is one
       sentence about the group rather than three about the rows - and a paragraph repeated
       under three labels is three help entries saying the same thing, which is the collision
       help_note_labels_are_unique_in_a_section refuses. */
    [MESH_UI_FIELD_LORA_IGNORE_NODE_0] = {{MESH_STR_SETTINGS_FIELD_LORA_IGNORE_NODE_0,
                                           INKSTAND_FORM_TEXT, MESH_UI_SETTINGS_LORA,
                                           MESH_UI_TEXT_LIMIT_LORA_IGNORE_NODE_0, NULL, NO_PRESETS,
                                           0U, MESH_STR_SETTINGS_NOTE_LORA_IGNORE_NODES},
                                          INKCELL_STR_NONE,
                                          NULL},
    [MESH_UI_FIELD_LORA_IGNORE_NODE_1] = {{MESH_STR_SETTINGS_FIELD_LORA_IGNORE_NODE_1,
                                           INKSTAND_FORM_TEXT, MESH_UI_SETTINGS_LORA,
                                           MESH_UI_TEXT_LIMIT_LORA_IGNORE_NODE_1, NULL, NO_PRESETS,
                                           0U, INKCELL_STR_NONE},
                                          INKCELL_STR_NONE,
                                          NULL},
    [MESH_UI_FIELD_LORA_IGNORE_NODE_2] = {{MESH_STR_SETTINGS_FIELD_LORA_IGNORE_NODE_2,
                                           INKSTAND_FORM_TEXT, MESH_UI_SETTINGS_LORA,
                                           MESH_UI_TEXT_LIMIT_LORA_IGNORE_NODE_2, NULL, NO_PRESETS,
                                           0U, INKCELL_STR_NONE},
                                          INKCELL_STR_NONE,
                                          NULL},
    /* Ham mode's three. The power row shares the LoRa row's presets, because it is the same
       quantity written by a different verb. */
    [MESH_UI_FIELD_LORA_HAM_CALL_SIGN] = {{MESH_STR_SETTINGS_FIELD_LORA_HAM_CALL_SIGN,
                                           INKSTAND_FORM_TEXT, MESH_UI_SETTINGS_LORA,
                                           MESH_UI_TEXT_LIMIT_LORA_HAM_CALL_SIGN, NULL, NO_PRESETS,
                                           0U, MESH_STR_SETTINGS_NOTE_LORA_HAM_CALL_SIGN},
                                          INKCELL_STR_NONE,
                                          NULL},
    [MESH_UI_FIELD_LORA_HAM_FREQUENCY] = {{MESH_STR_SETTINGS_FIELD_LORA_HAM_FREQUENCY,
                                           INKSTAND_FORM_TEXT, MESH_UI_SETTINGS_LORA,
                                           MESH_UI_TEXT_LIMIT_LORA_HAM_FREQUENCY, NULL, NO_PRESETS,
                                           0U, MESH_STR_SETTINGS_NOTE_LORA_HAM_FREQUENCY},
                                          INKCELL_STR_NONE,
                                          NULL},
    [MESH_UI_FIELD_LORA_HAM_TX_POWER] = {{MESH_STR_SETTINGS_FIELD_LORA_HAM_TX_POWER,
                                          INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_LORA, 0U, NULL,
                                          SCALE_PRESETS_AFTER_ZERO(k_tx_power_presets), 0U,
                                          MESH_STR_SETTINGS_NOTE_LORA_HAM_TX_POWER},
                                         INKCELL_STR_NONE,
                                         format_tx_power},
    [MESH_UI_FIELD_SECURITY_PRIVATE_KEY] = {{MESH_STR_SETTINGS_FIELD_SECURITY_PRIVATE_KEY,
                                             INKSTAND_FORM_KEY, MESH_UI_SETTINGS_SECURITY,
                                             MESH_UI_TEXT_LIMIT_SECURITY_PRIVATE_KEY, NULL,
                                             NO_PRESETS, PRIVATE_KEY_CHOICES,
                                             MESH_STR_SETTINGS_NOTE_SECURITY_PRIVATE_KEY},
                                            INKCELL_STR_NONE,
                                            NULL},
    [MESH_UI_FIELD_SECURITY_ADMIN_KEY_0] = {{MESH_STR_SETTINGS_FIELD_SECURITY_ADMIN_KEY_0,
                                             INKSTAND_FORM_KEY, MESH_UI_SETTINGS_SECURITY,
                                             MESH_UI_TEXT_LIMIT_SECURITY_ADMIN_KEY_0, NULL,
                                             NO_PRESETS, ADMIN_KEY_CHOICES,
                                             MESH_STR_SETTINGS_NOTE_SECURITY_ADMIN_KEYS},
                                            INKCELL_STR_NONE,
                                            NULL},
    [MESH_UI_FIELD_SECURITY_ADMIN_KEY_1] = {{MESH_STR_SETTINGS_FIELD_SECURITY_ADMIN_KEY_1,
                                             INKSTAND_FORM_KEY, MESH_UI_SETTINGS_SECURITY,
                                             MESH_UI_TEXT_LIMIT_SECURITY_ADMIN_KEY_1, NULL,
                                             NO_PRESETS, ADMIN_KEY_CHOICES, INKCELL_STR_NONE},
                                            INKCELL_STR_NONE,
                                            NULL},
    [MESH_UI_FIELD_SECURITY_ADMIN_KEY_2] = {{MESH_STR_SETTINGS_FIELD_SECURITY_ADMIN_KEY_2,
                                             INKSTAND_FORM_KEY, MESH_UI_SETTINGS_SECURITY,
                                             MESH_UI_TEXT_LIMIT_SECURITY_ADMIN_KEY_2, NULL,
                                             NO_PRESETS, ADMIN_KEY_CHOICES, INKCELL_STR_NONE},
                                            INKCELL_STR_NONE,
                                            NULL},
    [MESH_UI_FIELD_SECURITY_MANAGED] = {{MESH_STR_SETTINGS_FIELD_SECURITY_MANAGED,
                                         INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_SECURITY, 0U, NULL,
                                         NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_SECURITY_MANAGED},
                                        INKCELL_STR_NONE,
                                        NULL},
    [MESH_UI_FIELD_SECURITY_ADMIN_CHANNEL] = {{MESH_STR_SETTINGS_FIELD_SECURITY_ADMIN_CHANNEL,
                                               INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_SECURITY, 0U,
                                               NULL, NO_PRESETS, 0U,
                                               MESH_STR_SETTINGS_NOTE_SECURITY_ADMIN_CHANNEL},
                                              INKCELL_STR_NONE,
                                              NULL},
    [MESH_UI_FIELD_SECURITY_SERIAL] = {{MESH_STR_SETTINGS_FIELD_SECURITY_SERIAL,
                                        INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_SECURITY, 0U, NULL,
                                        NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_SECURITY_SERIAL},
                                       INKCELL_STR_NONE,
                                       NULL},
    [MESH_UI_FIELD_SECURITY_DEBUG_LOG] = {{MESH_STR_SETTINGS_FIELD_SECURITY_DEBUG_LOG,
                                           INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_SECURITY, 0U,
                                           NULL, NO_PRESETS, 0U,
                                           MESH_STR_SETTINGS_NOTE_SECURITY_DEBUG_LOG},
                                          INKCELL_STR_NONE,
                                          NULL},
    [MESH_UI_FIELD_NEIGHBOR_ENABLED] = {{MESH_STR_SETTINGS_FIELD_NEIGHBOR_ENABLED,
                                         INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_NEIGHBOR_INFO, 0U,
                                         NULL, NO_PRESETS, 0U,
                                         MESH_STR_SETTINGS_NOTE_NEIGHBOR_ENABLED},
                                        INKCELL_STR_NONE,
                                        NULL},
    [MESH_UI_FIELD_NEIGHBOR_INTERVAL] = {{MESH_STR_SETTINGS_FIELD_NEIGHBOR_INTERVAL,
                                          INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_NEIGHBOR_INFO, 0U,
                                          NULL, SCALE_PRESETS(k_neighbor_presets), 0U,
                                          MESH_STR_SETTINGS_NOTE_NEIGHBOR_INTERVAL},
                                         INKCELL_STR_NONE,
                                         NULL},
    [MESH_UI_FIELD_NEIGHBOR_OVER_LORA] = {{MESH_STR_SETTINGS_FIELD_NEIGHBOR_OVER_LORA,
                                           INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_NEIGHBOR_INFO, 0U,
                                           NULL, NO_PRESETS, 0U,
                                           MESH_STR_SETTINGS_NOTE_NEIGHBOR_OVER_LORA},
                                          INKCELL_STR_NONE,
                                          NULL},
    [MESH_UI_FIELD_RANGE_TEST_ENABLED] = {{MESH_STR_SETTINGS_FIELD_RANGE_TEST_ENABLED,
                                           INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_RANGE_TEST, 0U,
                                           NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
                                          INKCELL_STR_NONE,
                                          NULL},
    [MESH_UI_FIELD_RANGE_TEST_SENDER] = {{MESH_STR_SETTINGS_FIELD_RANGE_TEST_SENDER,
                                          INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_RANGE_TEST, 0U,
                                          NULL, SCALE_PRESETS(k_range_test_presets), 0U,
                                          MESH_STR_SETTINGS_NOTE_RANGE_TEST_SENDER},
                                         MESH_STR_ZERO_NEVER,
                                         NULL},
    [MESH_UI_FIELD_RANGE_TEST_SAVE] = {{MESH_STR_SETTINGS_FIELD_RANGE_TEST_SAVE,
                                        INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_RANGE_TEST, 0U, NULL,
                                        NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_RANGE_TEST_SAVE},
                                       INKCELL_STR_NONE,
                                       NULL},
    [MESH_UI_FIELD_RANGE_TEST_CLEAR] = {{MESH_STR_SETTINGS_FIELD_RANGE_TEST_CLEAR,
                                         INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_RANGE_TEST, 0U,
                                         NULL, NO_PRESETS, 0U,
                                         MESH_STR_SETTINGS_NOTE_RANGE_TEST_CLEAR},
                                        INKCELL_STR_NONE,
                                        NULL},
    [MESH_UI_FIELD_PAX_ENABLED] = {{MESH_STR_SETTINGS_FIELD_PAX_ENABLED, INKSTAND_FORM_TOGGLE,
                                    MESH_UI_SETTINGS_PAXCOUNTER, 0U, NULL, NO_PRESETS, 0U,
                                    INKCELL_STR_NONE},
                                   INKCELL_STR_NONE,
                                   NULL},
    [MESH_UI_FIELD_PAX_INTERVAL] = {{MESH_STR_SETTINGS_FIELD_PAX_INTERVAL, INKSTAND_FORM_NUMBER,
                                     MESH_UI_SETTINGS_PAXCOUNTER, 0U, NULL,
                                     SCALE_PRESETS_AFTER_ZERO(k_interval_presets), 0U,
                                     MESH_STR_SETTINGS_NOTE_PAX_INTERVAL},
                                    MESH_STR_ZERO_DEFAULT,
                                    NULL},
    [MESH_UI_FIELD_PAX_WIFI_THRESHOLD] = {{MESH_STR_SETTINGS_FIELD_PAX_WIFI_THRESHOLD,
                                           INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_PAXCOUNTER, 0U,
                                           NULL, SCALE_PRESETS(k_rssi_presets), 0U,
                                           MESH_STR_SETTINGS_NOTE_PAX_WIFI},
                                          INKCELL_STR_NONE,
                                          format_rssi},
    [MESH_UI_FIELD_PAX_BLE_THRESHOLD] = {{MESH_STR_SETTINGS_FIELD_PAX_BLE_THRESHOLD,
                                          INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_PAXCOUNTER, 0U,
                                          NULL, SCALE_PRESETS(k_rssi_presets), 0U,
                                          MESH_STR_SETTINGS_NOTE_PAX_BLE},
                                         INKCELL_STR_NONE,
                                         format_rssi},
    [MESH_UI_FIELD_TAK_TEAM] = {{MESH_STR_SETTINGS_FIELD_TAK_TEAM, INKSTAND_FORM_ENUM,
                                 MESH_UI_SETTINGS_TAK, 15U, tak_team_name, NO_PRESETS, 0U,
                                 MESH_STR_SETTINGS_NOTE_TAK_TEAM},
                                INKCELL_STR_NONE,
                                NULL},
    [MESH_UI_FIELD_TAK_ROLE] = {{MESH_STR_SETTINGS_FIELD_TAK_ROLE, INKSTAND_FORM_ENUM,
                                 MESH_UI_SETTINGS_TAK, 9U, tak_role_name, NO_PRESETS, 0U,
                                 MESH_STR_SETTINGS_NOTE_TAK_ROLE},
                                INKCELL_STR_NONE,
                                NULL},
    [MESH_UI_FIELD_AMBIENT_LED] = {{MESH_STR_SETTINGS_FIELD_AMBIENT_LED, INKSTAND_FORM_TOGGLE,
                                    MESH_UI_SETTINGS_AMBIENT, 0U, NULL, NO_PRESETS, 0U,
                                    INKCELL_STR_NONE},
                                   INKCELL_STR_NONE,
                                   NULL},
    [MESH_UI_FIELD_AMBIENT_CURRENT] = {{MESH_STR_SETTINGS_FIELD_AMBIENT_CURRENT,
                                        INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_AMBIENT, 0U, NULL,
                                        SCALE_PRESETS(k_led_current_presets), 0U,
                                        MESH_STR_SETTINGS_NOTE_AMBIENT_CURRENT},
                                       INKCELL_STR_NONE,
                                       format_milliamps},
    [MESH_UI_FIELD_AMBIENT_RED] = {{MESH_STR_SETTINGS_FIELD_AMBIENT_RED, INKSTAND_FORM_NUMBER,
                                    MESH_UI_SETTINGS_AMBIENT, 0U, NULL,
                                    SCALE_PRESETS(k_led_level_presets), 0U, INKCELL_STR_NONE},
                                   INKCELL_STR_NONE,
                                   format_level},
    [MESH_UI_FIELD_AMBIENT_GREEN] = {{MESH_STR_SETTINGS_FIELD_AMBIENT_GREEN, INKSTAND_FORM_NUMBER,
                                      MESH_UI_SETTINGS_AMBIENT, 0U, NULL,
                                      SCALE_PRESETS(k_led_level_presets), 0U, INKCELL_STR_NONE},
                                     INKCELL_STR_NONE,
                                     format_level},
    [MESH_UI_FIELD_AMBIENT_BLUE] = {{MESH_STR_SETTINGS_FIELD_AMBIENT_BLUE, INKSTAND_FORM_NUMBER,
                                     MESH_UI_SETTINGS_AMBIENT, 0U, NULL,
                                     SCALE_PRESETS(k_led_level_presets), 0U, INKCELL_STR_NONE},
                                    INKCELL_STR_NONE,
                                    format_level},
    [MESH_UI_FIELD_STATUS_TEXT] = {{MESH_STR_SETTINGS_FIELD_STATUS_TEXT, INKSTAND_FORM_TEXT,
                                    MESH_UI_SETTINGS_STATUS_MESSAGE, MESH_UI_TEXT_LIMIT_STATUS_TEXT,
                                    NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
                                   INKCELL_STR_NONE,
                                   NULL},
    [MESH_UI_FIELD_DETECT_ENABLED] = {{MESH_STR_SETTINGS_FIELD_DETECT_ENABLED, INKSTAND_FORM_TOGGLE,
                                       MESH_UI_SETTINGS_DETECTION, 0U, NULL, NO_PRESETS, 0U,
                                       INKCELL_STR_NONE},
                                      INKCELL_STR_NONE,
                                      NULL},
    [MESH_UI_FIELD_DETECT_NAME] = {{MESH_STR_SETTINGS_FIELD_DETECT_NAME, INKSTAND_FORM_TEXT,
                                    MESH_UI_SETTINGS_DETECTION, MESH_UI_TEXT_LIMIT_DETECT_NAME,
                                    NULL, NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_DETECT_NAME},
                                   INKCELL_STR_NONE,
                                   NULL},
    [MESH_UI_FIELD_DETECT_MIN_BROADCAST] = {{MESH_STR_SETTINGS_FIELD_DETECT_MIN_BROADCAST,
                                             INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_DETECTION, 0U,
                                             NULL, SCALE_PRESETS(k_detect_min_presets), 0U,
                                             MESH_STR_SETTINGS_NOTE_DETECT_MIN_BROADCAST},
                                            MESH_STR_ZERO_NONE,
                                            NULL},
    [MESH_UI_FIELD_DETECT_STATE_BROADCAST] = {{MESH_STR_SETTINGS_FIELD_DETECT_STATE_BROADCAST,
                                               INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_DETECTION, 0U,
                                               NULL, SCALE_PRESETS(k_detect_state_presets), 0U,
                                               MESH_STR_SETTINGS_NOTE_DETECT_STATE_BROADCAST},
                                              MESH_STR_ZERO_OFF,
                                              NULL},
    [MESH_UI_FIELD_DETECT_SEND_BELL] = {{MESH_STR_SETTINGS_FIELD_DETECT_SEND_BELL,
                                         INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_DETECTION, 0U, NULL,
                                         NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_DETECT_SEND_BELL},
                                        INKCELL_STR_NONE,
                                        NULL},
    [MESH_UI_FIELD_DETECT_PIN] = {{MESH_STR_SETTINGS_FIELD_DETECT_PIN, INKSTAND_FORM_NUMBER,
                                   MESH_UI_SETTINGS_DETECTION, 0U, NULL,
                                   NAMED_PRESETS(k_gpio_presets), 0U,
                                   MESH_STR_SETTINGS_NOTE_DETECT_PIN},
                                  INKCELL_STR_NONE,
                                  format_pin},
    [MESH_UI_FIELD_DETECT_TRIGGER] = {{MESH_STR_SETTINGS_FIELD_DETECT_TRIGGER, INKSTAND_FORM_ENUM,
                                       MESH_UI_SETTINGS_DETECTION, 6U, trigger_name, NO_PRESETS, 0U,
                                       MESH_STR_SETTINGS_NOTE_DETECT_TRIGGER},
                                      INKCELL_STR_NONE,
                                      NULL},
    [MESH_UI_FIELD_DETECT_PULLUP] = {{MESH_STR_SETTINGS_FIELD_DETECT_PULLUP, INKSTAND_FORM_TOGGLE,
                                      MESH_UI_SETTINGS_DETECTION, 0U, NULL, NO_PRESETS, 0U,
                                      MESH_STR_SETTINGS_NOTE_DETECT_PULLUP},
                                     INKCELL_STR_NONE,
                                     NULL},
    [MESH_UI_FIELD_EXTNOTIF_ENABLED] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_ENABLED,
                                         INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                         0U, NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
                                        INKCELL_STR_NONE,
                                        NULL},
    [MESH_UI_FIELD_EXTNOTIF_ACTIVE] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_ACTIVE,
                                        INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U,
                                        NULL, NO_PRESETS, 0U,
                                        MESH_STR_SETTINGS_NOTE_EXTNOTIF_ACTIVE},
                                       INKCELL_STR_NONE,
                                       NULL},
    [MESH_UI_FIELD_EXTNOTIF_OUTPUT_MS] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_OUTPUT_MS,
                                           INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                           0U, NULL, SCALE_PRESETS(k_output_ms_presets), 0U,
                                           MESH_STR_SETTINGS_NOTE_EXTNOTIF_OUTPUT_MS},
                                          INKCELL_STR_NONE,
                                          format_millis},
    [MESH_UI_FIELD_EXTNOTIF_NAG] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_NAG, INKSTAND_FORM_NUMBER,
                                     MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                     SCALE_PRESETS(k_nag_presets), 0U,
                                     MESH_STR_SETTINGS_NOTE_EXTNOTIF_NAG},
                                    MESH_STR_ZERO_ONCE,
                                    NULL},
    [MESH_UI_FIELD_EXTNOTIF_PWM] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_PWM, INKSTAND_FORM_TOGGLE,
                                     MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL, NO_PRESETS, 0U,
                                     MESH_STR_SETTINGS_NOTE_EXTNOTIF_PWM},
                                    INKCELL_STR_NONE,
                                    NULL},
    [MESH_UI_FIELD_EXTNOTIF_I2S] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_I2S, INKSTAND_FORM_TOGGLE,
                                     MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL, NO_PRESETS, 0U,
                                     MESH_STR_SETTINGS_NOTE_EXTNOTIF_I2S},
                                    INKCELL_STR_NONE,
                                    NULL},
    /* The three output groups. Each row is named for what it is inside its group, the way the
       telemetry groups are, because the heading above it says which output it belongs to. */
    [MESH_UI_FIELD_EXTNOTIF_PIN] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_PIN, INKSTAND_FORM_NUMBER,
                                     MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                     NAMED_PRESETS(k_gpio_presets), 0U, INKCELL_STR_NONE},
                                    INKCELL_STR_NONE,
                                    format_pin},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_MSG] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_ALERT_MSG,
                                           INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                           0U, NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
                                          INKCELL_STR_NONE,
                                          NULL},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_BELL] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_ALERT_BELL,
                                            INKSTAND_FORM_TOGGLE, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                            0U, NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
                                           INKCELL_STR_NONE,
                                           NULL},
    [MESH_UI_FIELD_EXTNOTIF_PIN_VIBRA] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_PIN_VIBRA,
                                           INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                           0U, NULL, NAMED_PRESETS(k_gpio_presets), 0U,
                                           INKCELL_STR_NONE},
                                          INKCELL_STR_NONE,
                                          format_pin},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_VIBRA] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_ALERT_MSG_VIBRA,
                                                 INKSTAND_FORM_TOGGLE,
                                                 MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                                 NO_PRESETS, 0U, INKCELL_STR_NONE},
                                                INKCELL_STR_NONE,
                                                NULL},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_VIBRA] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_ALERT_BELL_VIBRA,
                                                  INKSTAND_FORM_TOGGLE,
                                                  MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                                  NO_PRESETS, 0U, INKCELL_STR_NONE},
                                                 INKCELL_STR_NONE,
                                                 NULL},
    [MESH_UI_FIELD_EXTNOTIF_PIN_BUZZER] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_PIN_BUZZER,
                                            INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_EXT_NOTIFICATION,
                                            0U, NULL, NAMED_PRESETS(k_gpio_presets), 0U,
                                            INKCELL_STR_NONE},
                                           INKCELL_STR_NONE,
                                           format_pin},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_BUZZER] = {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_ALERT_MSG_BUZZER,
                                                  INKSTAND_FORM_TOGGLE,
                                                  MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL,
                                                  NO_PRESETS, 0U, INKCELL_STR_NONE},
                                                 INKCELL_STR_NONE,
                                                 NULL},
    [MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_BUZZER] =
        {{MESH_STR_SETTINGS_FIELD_EXTNOTIF_ALERT_BELL_BUZZER, INKSTAND_FORM_TOGGLE,
          MESH_UI_SETTINGS_EXT_NOTIFICATION, 0U, NULL, NO_PRESETS, 0U, INKCELL_STR_NONE},
         INKCELL_STR_NONE,
         NULL},
    [MESH_UI_FIELD_TRAFFIC_POSITION_INTERVAL] = {{MESH_STR_SETTINGS_FIELD_TRAFFIC_POSITION_INTERVAL,
                                                  INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_TRAFFIC,
                                                  0U, NULL,
                                                  SCALE_PRESETS(k_traffic_interval_presets), 0U,
                                                  MESH_STR_SETTINGS_NOTE_TRAFFIC_POSITION},
                                                 MESH_STR_ZERO_OFF,
                                                 NULL},
    [MESH_UI_FIELD_TRAFFIC_NODEINFO_HOPS] = {{MESH_STR_SETTINGS_FIELD_TRAFFIC_NODEINFO_HOPS,
                                              INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_TRAFFIC, 0U,
                                              NULL, SCALE_PRESETS(k_traffic_hops_presets), 0U,
                                              MESH_STR_SETTINGS_NOTE_TRAFFIC_NODEINFO_HOPS},
                                             MESH_STR_ZERO_OFF,
                                             format_count},
    [MESH_UI_FIELD_TRAFFIC_RATE_WINDOW] = {{MESH_STR_SETTINGS_FIELD_TRAFFIC_RATE_WINDOW,
                                            INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_TRAFFIC, 0U,
                                            NULL, SCALE_PRESETS(k_traffic_interval_presets), 0U,
                                            MESH_STR_SETTINGS_NOTE_TRAFFIC_RATE_WINDOW},
                                           MESH_STR_ZERO_OFF,
                                           NULL},
    [MESH_UI_FIELD_TRAFFIC_RATE_PACKETS] = {{MESH_STR_SETTINGS_FIELD_TRAFFIC_RATE_PACKETS,
                                             INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_TRAFFIC, 0U,
                                             NULL, SCALE_PRESETS(k_traffic_packets_presets), 0U,
                                             MESH_STR_SETTINGS_NOTE_TRAFFIC_RATE_PACKETS},
                                            MESH_STR_ZERO_OFF,
                                            format_count},
    [MESH_UI_FIELD_TRAFFIC_UNKNOWN_THRESHOLD] = {{MESH_STR_SETTINGS_FIELD_TRAFFIC_UNKNOWN_THRESHOLD,
                                                  INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_TRAFFIC,
                                                  0U, NULL,
                                                  SCALE_PRESETS(k_traffic_packets_presets), 0U,
                                                  MESH_STR_SETTINGS_NOTE_TRAFFIC_UNKNOWN},
                                                 MESH_STR_ZERO_OFF,
                                                 format_count},
    [MESH_UI_FIELD_SECURITY_SIGNATURE_POLICY] = {{MESH_STR_SETTINGS_FIELD_SECURITY_SIGNATURE_POLICY,
                                                  INKSTAND_FORM_ENUM, MESH_UI_SETTINGS_SECURITY, 3U,
                                                  signature_policy_name, NO_PRESETS, 0U,
                                                  MESH_STR_SETTINGS_NOTE_SECURITY_SIGNATURE},
                                                 INKCELL_STR_NONE,
                                                 NULL},
    [MESH_UI_FIELD_UI_THEME] = {{MESH_STR_SETTINGS_FIELD_UI_THEME, INKSTAND_FORM_ENUM,
                                 MESH_UI_SETTINGS_RADIO_UI, 3U, ui_theme_name, NO_PRESETS, 0U,
                                 INKCELL_STR_NONE},
                                INKCELL_STR_NONE,
                                NULL},
    [MESH_UI_FIELD_UI_BRIGHTNESS] = {{MESH_STR_SETTINGS_FIELD_UI_BRIGHTNESS, INKSTAND_FORM_NUMBER,
                                      MESH_UI_SETTINGS_RADIO_UI, 0U, NULL,
                                      SCALE_PRESETS(k_ui_brightness_presets), 0U, INKCELL_STR_NONE},
                                     INKCELL_STR_NONE,
                                     format_plain},
    [MESH_UI_FIELD_UI_SCREEN_TIMEOUT] = {{MESH_STR_SETTINGS_FIELD_UI_SCREEN_TIMEOUT,
                                          INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_RADIO_UI, 0U, NULL,
                                          SCALE_PRESETS_AFTER_ZERO(k_ui_timeout_presets), 0U,
                                          MESH_STR_SETTINGS_NOTE_UI_SCREEN_TIMEOUT},
                                         MESH_STR_ZERO_NEVER,
                                         NULL},
    [MESH_UI_FIELD_UI_ALERT] = {{MESH_STR_SETTINGS_FIELD_UI_ALERT, INKSTAND_FORM_TOGGLE,
                                 MESH_UI_SETTINGS_RADIO_UI, 0U, NULL, NO_PRESETS, 0U,
                                 MESH_STR_SETTINGS_NOTE_UI_ALERT},
                                INKCELL_STR_NONE,
                                NULL},
    [MESH_UI_FIELD_UI_BANNER] = {{MESH_STR_SETTINGS_FIELD_UI_BANNER, INKSTAND_FORM_TOGGLE,
                                  MESH_UI_SETTINGS_RADIO_UI, 0U, NULL, NO_PRESETS, 0U,
                                  MESH_STR_SETTINGS_NOTE_UI_BANNER},
                                 INKCELL_STR_NONE,
                                 NULL},
    [MESH_UI_FIELD_UI_RING_TONE] = {{MESH_STR_SETTINGS_FIELD_UI_RING_TONE, INKSTAND_FORM_NUMBER,
                                     MESH_UI_SETTINGS_RADIO_UI, 0U, NULL,
                                     NAMED_PRESETS(k_ui_ringtone_presets), 0U,
                                     MESH_STR_SETTINGS_NOTE_UI_RING_TONE},
                                    INKCELL_STR_NONE,
                                    format_plain},
    [MESH_UI_FIELD_UI_COMPASS_MODE] = {{MESH_STR_SETTINGS_FIELD_UI_COMPASS_MODE, INKSTAND_FORM_ENUM,
                                        MESH_UI_SETTINGS_RADIO_UI, 3U, ui_compass_name, NO_PRESETS,
                                        0U, MESH_STR_SETTINGS_NOTE_UI_COMPASS_MODE},
                                       INKCELL_STR_NONE,
                                       NULL},
    [MESH_UI_FIELD_UI_GPS_FORMAT] = {{MESH_STR_SETTINGS_FIELD_UI_GPS_FORMAT, INKSTAND_FORM_ENUM,
                                      MESH_UI_SETTINGS_RADIO_UI, 7U, ui_gps_format_name, NO_PRESETS,
                                      0U, MESH_STR_SETTINGS_NOTE_UI_GPS_FORMAT},
                                     INKCELL_STR_NONE,
                                     NULL},
    [MESH_UI_FIELD_UI_CLOCKFACE] = {{MESH_STR_SETTINGS_FIELD_UI_CLOCKFACE, INKSTAND_FORM_ENUM,
                                     MESH_UI_SETTINGS_RADIO_UI, 2U, ui_clockface_name, NO_PRESETS,
                                     0U, MESH_STR_SETTINGS_NOTE_UI_CLOCKFACE},
                                    INKCELL_STR_NONE,
                                    NULL},
    /* The per-slot cap, not the wire's 200: see MESH_UI_CANNED_SLOTS for why the count and
       the length are chosen together. The keyboard reads `limit` as the number of bytes it may
       commit. */
    [MESH_UI_FIELD_CANNED_0] = {{MESH_STR_SETTINGS_FIELD_CANNED_0, INKSTAND_FORM_TEXT,
                                 MESH_UI_SETTINGS_CANNED, MESH_UI_TEXT_LIMIT_CANNED_0, NULL,
                                 NO_PRESETS, 0U, INKCELL_STR_NONE},
                                INKCELL_STR_NONE,
                                NULL},
    [MESH_UI_FIELD_CANNED_1] = {{MESH_STR_SETTINGS_FIELD_CANNED_1, INKSTAND_FORM_TEXT,
                                 MESH_UI_SETTINGS_CANNED, MESH_UI_TEXT_LIMIT_CANNED_1, NULL,
                                 NO_PRESETS, 0U, INKCELL_STR_NONE},
                                INKCELL_STR_NONE,
                                NULL},
    [MESH_UI_FIELD_CANNED_2] = {{MESH_STR_SETTINGS_FIELD_CANNED_2, INKSTAND_FORM_TEXT,
                                 MESH_UI_SETTINGS_CANNED, MESH_UI_TEXT_LIMIT_CANNED_2, NULL,
                                 NO_PRESETS, 0U, INKCELL_STR_NONE},
                                INKCELL_STR_NONE,
                                NULL},
    [MESH_UI_FIELD_CANNED_3] = {{MESH_STR_SETTINGS_FIELD_CANNED_3, INKSTAND_FORM_TEXT,
                                 MESH_UI_SETTINGS_CANNED, MESH_UI_TEXT_LIMIT_CANNED_3, NULL,
                                 NO_PRESETS, 0U, INKCELL_STR_NONE},
                                INKCELL_STR_NONE,
                                NULL},
    [MESH_UI_FIELD_CANNED_4] = {{MESH_STR_SETTINGS_FIELD_CANNED_4, INKSTAND_FORM_TEXT,
                                 MESH_UI_SETTINGS_CANNED, MESH_UI_TEXT_LIMIT_CANNED_4, NULL,
                                 NO_PRESETS, 0U, INKCELL_STR_NONE},
                                INKCELL_STR_NONE,
                                NULL},
    [MESH_UI_FIELD_CANNED_5] = {{MESH_STR_SETTINGS_FIELD_CANNED_5, INKSTAND_FORM_TEXT,
                                 MESH_UI_SETTINGS_CANNED, MESH_UI_TEXT_LIMIT_CANNED_5, NULL,
                                 NO_PRESETS, 0U, INKCELL_STR_NONE},
                                INKCELL_STR_NONE,
                                NULL},
    [MESH_UI_FIELD_BEACON_LISTEN] = {{MESH_STR_SETTINGS_FIELD_BEACON_LISTEN, INKSTAND_FORM_FLAG,
                                      MESH_UI_SETTINGS_BEACON, 0x0001U, NULL, NO_PRESETS, 0U,
                                      MESH_STR_SETTINGS_NOTE_BEACON_LISTEN},
                                     INKCELL_STR_NONE,
                                     NULL},
    [MESH_UI_FIELD_BEACON_BROADCAST] = {{MESH_STR_SETTINGS_FIELD_BEACON_BROADCAST,
                                         INKSTAND_FORM_FLAG, MESH_UI_SETTINGS_BEACON, 0x0002U, NULL,
                                         NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_BEACON_BROADCAST},
                                        INKCELL_STR_NONE,
                                        NULL},
    [MESH_UI_FIELD_BEACON_LEGACY_SPLIT] = {{MESH_STR_SETTINGS_FIELD_BEACON_LEGACY_SPLIT,
                                            INKSTAND_FORM_FLAG, MESH_UI_SETTINGS_BEACON, 0x0004U,
                                            NULL, NO_PRESETS, 0U,
                                            MESH_STR_SETTINGS_NOTE_BEACON_LEGACY_SPLIT},
                                           INKCELL_STR_NONE,
                                           NULL},
    [MESH_UI_FIELD_BEACON_INTERVAL] = {{MESH_STR_SETTINGS_FIELD_BEACON_INTERVAL,
                                        INKSTAND_FORM_NUMBER, MESH_UI_SETTINGS_BEACON, 0U, NULL,
                                        SCALE_PRESETS(k_beacon_interval_presets), 0U,
                                        MESH_STR_SETTINGS_NOTE_BEACON_INTERVAL},
                                       MESH_STR_ZERO_DEFAULT,
                                       NULL},
    [MESH_UI_FIELD_BEACON_MESSAGE] = {{MESH_STR_SETTINGS_FIELD_BEACON_MESSAGE, INKSTAND_FORM_TEXT,
                                       MESH_UI_SETTINGS_BEACON, MESH_UI_TEXT_LIMIT_BEACON_MESSAGE,
                                       NULL, NO_PRESETS, 0U, MESH_STR_SETTINGS_NOTE_BEACON_MESSAGE},
                                      INKCELL_STR_NONE,
                                      NULL},
    [MESH_UI_FIELD_BEACON_OFFER_NAME] = {{MESH_STR_SETTINGS_FIELD_BEACON_OFFER_NAME,
                                          INKSTAND_FORM_TEXT, MESH_UI_SETTINGS_BEACON,
                                          MESH_UI_TEXT_LIMIT_BEACON_OFFER_NAME, NULL, NO_PRESETS,
                                          0U, MESH_STR_SETTINGS_NOTE_BEACON_OFFER_NAME},
                                         INKCELL_STR_NONE,
                                         NULL},
    /* The offered key takes the channel row's choices whole: it is a ChannelSettings.psk, and
       every way there is of filling one in is a way of filling this one in. */
    [MESH_UI_FIELD_BEACON_OFFER_KEY] = {{MESH_STR_SETTINGS_FIELD_BEACON_OFFER_KEY,
                                         INKSTAND_FORM_KEY, MESH_UI_SETTINGS_BEACON,
                                         MESH_UI_TEXT_LIMIT_BEACON_OFFER_KEY, NULL, NO_PRESETS,
                                         CHANNEL_KEY_CHOICES,
                                         MESH_STR_SETTINGS_NOTE_BEACON_OFFER_KEY},
                                        INKCELL_STR_NONE,
                                        NULL},
    [MESH_UI_FIELD_BEACON_OFFER_REGION] = {{MESH_STR_SETTINGS_FIELD_BEACON_OFFER_REGION,
                                            INKSTAND_FORM_ENUM, MESH_UI_SETTINGS_BEACON, 38U,
                                            beacon_offer_region_name, NO_PRESETS, 0U,
                                            MESH_STR_SETTINGS_NOTE_BEACON_OFFER_REGION},
                                           INKCELL_STR_NONE,
                                           NULL},
    [MESH_UI_FIELD_BEACON_OFFER_PRESET] = {{MESH_STR_SETTINGS_FIELD_BEACON_OFFER_PRESET,
                                            INKSTAND_FORM_ENUM, MESH_UI_SETTINGS_BEACON,
                                            BEACON_PRESET_VALUES, beacon_offer_preset_name,
                                            NO_PRESETS, 0U,
                                            MESH_STR_SETTINGS_NOTE_BEACON_OFFER_PRESET},
                                           INKCELL_STR_NONE,
                                           NULL},
    BEACON_TARGET_ROWS(0),
    BEACON_TARGET_ROWS(1),
    BEACON_TARGET_ROWS(2),
    BEACON_TARGET_ROWS(3),
};

#undef BEACON_TARGET_ROWS

/*
 * The canned list is one string with '|' between entries. An empty list is no entries rather
 * than one empty one, and a trailing separator does not invent a last entry: both matter
 * because the count is what the "also on radio" row reports and what decides how far the write
 * builder walks.
 */
char mesh_ui_settings_field_reserved_char(enum mesh_ui_setting_field field) {
    switch (field) {
    case MESH_UI_FIELD_CANNED_0:
    case MESH_UI_FIELD_CANNED_1:
    case MESH_UI_FIELD_CANNED_2:
    case MESH_UI_FIELD_CANNED_3:
    case MESH_UI_FIELD_CANNED_4:
    case MESH_UI_FIELD_CANNED_5:
        return '|';
    default:
        return '\0';
    }
}

uint32_t mesh_ui_settings_canned_count(const char *list) {
    if (list == NULL || list[0] == '\0') {
        return 0U;
    }
    uint32_t count = 1U;
    for (const char *p = list; *p != '\0'; ++p) {
        if (*p == '|') {
            count++;
        }
    }
    /* A list ending in '|' has no entry after it. */
    if (list[strlen(list) - 1U] == '|') {
        count--;
    }
    return count;
}

void mesh_ui_settings_canned_entry(const char *list, uint32_t index, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if (list == NULL) {
        return;
    }
    const char *start = list;
    for (uint32_t i = 0; i < index; ++i) {
        const char *sep = strchr(start, '|');
        if (sep == NULL) {
            return;
        }
        start = sep + 1;
    }
    const char *end = strchr(start, '|');
    size_t len = (end != NULL) ? (size_t)(end - start) : strlen(start);
    if (len >= out_len) {
        len = out_len - 1U;
    }
    memcpy(out, start, len);
    out[len] = '\0';
}

const struct inkstand_form mesh_ui_settings_form = {k_fields, sizeof k_fields[0],
                                                    (uint16_t)MESH_UI_FIELD_COUNT};
_Static_assert(MESH_UI_FIELD_COUNT <= UINT16_MAX,
               "a field id is 16 bits in the form and the edits");

/* A field id as the form holds one, checked *before* it is narrowed: the form bounds-checks what
   it is handed, but an enum value past the table would first wrap to 16 bits and arrive as some
   real row's id. Every question put to the form goes through here. */
static uint16_t form_id(enum mesh_ui_setting_field field) {
    return (unsigned)field < MESH_UI_FIELD_COUNT ? (uint16_t)field : (uint16_t)MESH_UI_FIELD_NONE;
}

/* The row for `field` with this client's columns too - the same row mesh_ui_settings_form
   answers for, so an unknown field is the MESH_UI_FIELD_NONE row either way. */
const struct field_spec *field_spec(enum mesh_ui_setting_field field) {
    return (const struct field_spec *)inkstand_form_field(&mesh_ui_settings_form, form_id(field));
}

const char *mesh_ui_settings_field_label(enum mesh_ui_setting_field field) {
    return inkcell_str(field_spec(field)->form.label);
}

inkcell_str_id mesh_ui_settings_field_label_id(enum mesh_ui_setting_field field) {
    return field_spec(field)->form.label;
}

enum inkstand_form_kind mesh_ui_settings_field_kind(enum mesh_ui_setting_field field) {
    return field_spec(field)->form.kind;
}

enum mesh_ui_settings_section mesh_ui_settings_field_section(enum mesh_ui_setting_field field) {
    return (enum mesh_ui_settings_section)field_spec(field)->form.section;
}

/*
 * Whether the field table holds a row for this section at all.
 *
 * A walk rather than a column in the section tables, because the answer already exists in
 * k_fields and a second place to state it is a second place to forget it: a section whose last
 * editable row was retired would keep whatever the column said.
 */
bool mesh_ui_settings_section_has_fields(enum mesh_ui_settings_section section) {
    /* Checked before narrowing, as form_id() does for a field. */
    if ((unsigned)section >= MESH_UI_SETTINGS_SECTION_COUNT) {
        return false;
    }
    return inkstand_form_section_has_fields(&mesh_ui_settings_form, (uint16_t)section);
}

inkcell_str_id mesh_ui_settings_field_note(enum mesh_ui_setting_field field) {
    return field_spec(field)->form.note;
}

/*
 * The groups of FLAG rows, as a table of runs.
 *
 * One row per group rather than a first/count written out at each caller, for the reason the
 * section lists are accessors: the run is a fact about the field enum, and a fact stated in the
 * row builder and again in the write builder is one that will eventually be stated differently
 * in the two. A bit upstream adds is a field in the enum, a row in k_fields and a `count` here.
 */
static const struct {
    enum mesh_ui_setting_field first;
    uint32_t count;
} k_field_groups[MESH_UI_FIELD_GROUP_COUNT] = {
    [MESH_UI_FIELD_GROUP_POSITION_FLAGS] = {MESH_UI_FIELD_POSITION_FLAG_ALTITUDE, 10U},
    [MESH_UI_FIELD_GROUP_BEACON_FLAGS] = {MESH_UI_FIELD_BEACON_LISTEN, 3U},
    /* Twelve fields rather than four, because the group is the run and the record length is
       MESH_UI_BEACON_TARGET_FIELDS: both callers walk one and divide by the other. */
    [MESH_UI_FIELD_GROUP_BEACON_TARGETS] = {MESH_UI_FIELD_BEACON_TARGET_0_PRESET,
                                            MESH_UI_BEACON_TARGETS *MESH_UI_BEACON_TARGET_FIELDS},
};

uint32_t mesh_ui_settings_group_count(enum mesh_ui_setting_field_group group) {
    if ((unsigned)group >= MESH_UI_FIELD_GROUP_COUNT) {
        return 0U;
    }
    return k_field_groups[group].count;
}

enum mesh_ui_setting_field mesh_ui_settings_group_field(enum mesh_ui_setting_field_group group,
                                                        uint32_t index) {
    if (index >= mesh_ui_settings_group_count(group)) {
        return MESH_UI_FIELD_NONE;
    }
    return (enum mesh_ui_setting_field)(k_field_groups[group].first + index);
}

uint32_t mesh_ui_settings_field_bit(enum mesh_ui_setting_field field) {
    return inkstand_form_bit(&mesh_ui_settings_form, form_id(field));
}

uint32_t mesh_ui_settings_enum_count(enum mesh_ui_setting_field field) {
    return inkstand_form_enum_count(&mesh_ui_settings_form, form_id(field));
}

const char *mesh_ui_settings_enum_name(enum mesh_ui_setting_field field, uint32_t value) {
    return inkstand_form_enum_name(&mesh_ui_settings_form, form_id(field), value);
}

uint32_t mesh_ui_settings_number_step(enum mesh_ui_setting_field field, uint32_t value, int delta) {
    return inkstand_form_number_step(&mesh_ui_settings_form, form_id(field), value, delta);
}

/*
 * Where `value` sits on a NUMBER field's own scale - the arithmetic is in inkstand's form/scale.h.
 *
 * What is this client's is which lists are scales and which stand their 0 aside. LoRa's transmit
 * power reads 0 as "as much as this radio has", and the first version of the slider drew it with
 * the handle hard left. And two lists start above zero because the thing at the other end
 * refuses anything below - the public map drops a report under an hour, the firmware floors
 * neighbour info at four - while a radio nobody has configured still reports 0 for both.
 */
bool mesh_ui_settings_number_track(enum mesh_ui_setting_field field, uint32_t value,
                                   struct mesh_ui_settings_track *out) {
    struct inkstand_form_track track;
    if (!inkstand_form_number_track(&mesh_ui_settings_form, form_id(field), value, &track)) {
        return false;
    }
    if (out != NULL) {
        *out = (struct mesh_ui_settings_track){
            .position = track.position, .stops = track.stops, .unplaced = track.unplaced};
    }
    return true;
}

/*
 * The field's own limit, unclamped, because the edit buffer is measured from these limits
 * rather than the other way round (mesh/ui/settings_text.def).
 *
 * It used to be clamped to MESH_UI_SETTING_TEXT_MAX - 1, which is the same silent truncation
 * one layer up: a field wider than the buffer was offered a shorter keyboard cap and nothing
 * anywhere said the value had been cut. With the buffer sized from the table the clamp can
 * never fire, and a field that would have needed it fails
 * `settings_text_fields_fit_the_edit_buffer` instead - which names the field.
 */
uint32_t mesh_ui_settings_text_max(enum mesh_ui_setting_field field) {
    return inkstand_form_text_max(&mesh_ui_settings_form, form_id(field));
}

uint32_t mesh_ui_settings_key_choices(enum mesh_ui_setting_field field) {
    return inkstand_form_key_choices(&mesh_ui_settings_form, form_id(field));
}

bool mesh_ui_settings_key_len_ok(enum mesh_ui_setting_field field, size_t len) {
    switch (field) {
    case MESH_UI_FIELD_CHANNEL_KEY:
    /* The same key in the same submessage, so the same four lengths: the beacon's offer is a
       ChannelSettings and a key it would not accept is a channel nobody can join. */
    case MESH_UI_FIELD_BEACON_OFFER_KEY:
        return len == 0U || len == 1U || len == 16U || len == 32U;
    case MESH_UI_FIELD_SECURITY_PRIVATE_KEY:
        return len == 32U;
    case MESH_UI_FIELD_SECURITY_ADMIN_KEY_0:
    case MESH_UI_FIELD_SECURITY_ADMIN_KEY_1:
    case MESH_UI_FIELD_SECURITY_ADMIN_KEY_2:
        return len == 0U || len == 32U;
    default:
        return false;
    }
}

bool mesh_ui_settings_choice_allowed(uint32_t choices, uint32_t count, uint32_t value) {
    return inkstand_form_choice_allowed(choices, count, value);
}

uint32_t mesh_ui_settings_choice_step(uint32_t choices, uint32_t count, uint32_t current,
                                      int delta) {
    return inkstand_form_choice_step(choices, count, current, delta);
}

const struct mesh_ui_region_preset *
mesh_ui_settings_region_preset(const struct mesh_ui_settings *settings, uint32_t region) {
    if (settings == NULL || !settings->region_presets.loaded || region >= MESH_UI_REGION_COUNT) {
        return NULL;
    }
    const struct mesh_ui_region_preset *entry = &settings->region_presets.region[region];
    /* An empty set is a region the firmware's map did not describe, which is the same silence
       as no map at all. Answering with it would be answering "no preset is legal here". */
    return entry->presets != 0U ? entry : NULL;
}

bool mesh_ui_settings_section_needs_confirm(enum mesh_ui_settings_section section) {
    return section == MESH_UI_SETTINGS_BLUETOOTH || section == MESH_UI_SETTINGS_CHANNELS ||
           section == MESH_UI_SETTINGS_LORA || section == MESH_UI_SETTINGS_SECURITY ||
           section == MESH_UI_SETTINGS_POWER;
}

/* The fixed-position pair are the exception: setting a location is undone by setting another
   one and clearing it by setting it again, so a question in front of either would be a press
   the user has to make twice for nothing. */
enum mesh_ui_setting_consumer mesh_ui_settings_field_consumer(enum mesh_ui_setting_field field) {
    switch (field) {
    case MESH_UI_FIELD_POSITION_LATITUDE:
    case MESH_UI_FIELD_POSITION_LONGITUDE:
    case MESH_UI_FIELD_POSITION_ALTITUDE:
        return MESH_UI_SETTING_CONSUMER_FIXED_POSITION;
    /* The ham rows are LoRaConfig's neighbours and none of its fields: what they write is the
       owner's names, the primary channel's key and LoRa's frequency and power, all through one
       verb of their own. Y beside them must therefore leave them where they are. */
    case MESH_UI_FIELD_LORA_HAM_CALL_SIGN:
    case MESH_UI_FIELD_LORA_HAM_FREQUENCY:
    case MESH_UI_FIELD_LORA_HAM_TX_POWER:
        return MESH_UI_SETTING_CONSUMER_HAM_MODE;
    default:
        return MESH_UI_SETTING_CONSUMER_SECTION;
    }
}

bool mesh_ui_settings_action_needs_confirm(enum mesh_ui_settings_action action) {
    return action == MESH_UI_SETTINGS_ACTION_REBOOT || action == MESH_UI_SETTINGS_ACTION_SHUTDOWN ||
           action == MESH_UI_SETTINGS_ACTION_RESET_NODEDB ||
           action == MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG ||
           action == MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE ||
           action == MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG ||
           action == MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG ||
           action == MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP ||
           action == MESH_UI_SETTINGS_ACTION_SET_HAM_MODE ||
           action == MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL ||
           mesh_ui_settings_action_is_install_firmware(action) ||
           mesh_ui_settings_action_is_forget(action);
}

/*
 * The presses that raise something. See the note in settings.h for what the chevron promises.
 *
 * Spelled as "the ones behind a sheet, plus the four links", which is the shape the nav has: a
 * verb needing confirmation opens the question, and the sharing and importing pairs open a
 * screen and the keyboard. Everything else acts where it stands and the rows redraw when the
 * reply lands.
 *
 * "Back to my radio" is deliberately not here, and it is the one that has to be argued. It does
 * put a different radio's settings on the panel, so it navigates - but it raises nothing, and
 * what it navigates is *back*. A chevron pointing further in on the row that leaves is the wrong
 * half of the promise even where the promise is kept.
 *
 * `ui_nav_a_chevron_is_a_promise_the_nav_keeps` holds this against the nav rather than against
 * this list: it walks every section, presses A on every verb in it, and asks whether anything
 * was raised.
 */
bool mesh_ui_settings_action_opens(enum mesh_ui_settings_action action) {
    return mesh_ui_settings_action_needs_confirm(action) ||
           action == MESH_UI_SETTINGS_ACTION_SHARE_CHANNELS ||
           action == MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS ||
           action == MESH_UI_SETTINGS_ACTION_SHARE_CONTACT ||
           action == MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT;
}

/*
 * The presses that step the row's own value rather than doing anything. See the note in
 * settings.h for what the answer is spent on, and struct mesh_ui_settings_item::cycle for what
 * it makes the row.
 *
 * Spelled out rather than derived, on the terms mesh_ui_settings_action_is_radio() is. "Has a
 * value in its value column" would take in the forget rows, whose figure is the size of what
 * the press costs; "opens nothing" would take in the two checks, which send a request and
 * redraw when the answer lands. The question is whether pressing A leaves the reader on the
 * same row with a different setting on it, and only these six do.
 */
inkcell_str_id mesh_ui_text_size_name(int8_t size) {
    return size == MESH_UI_TEXT_SIZE_SMALL   ? MESH_STR_TEXT_SIZE_SMALL
           : size == MESH_UI_TEXT_SIZE_LARGE ? MESH_STR_TEXT_SIZE_LARGE
                                             : MESH_STR_TEXT_SIZE_STANDARD;
}

bool mesh_ui_settings_action_is_cycle(enum mesh_ui_settings_action action) {
    return action == MESH_UI_SETTINGS_ACTION_CYCLE_LANGUAGE ||
           action == MESH_UI_SETTINGS_ACTION_CYCLE_THEME ||
           action == MESH_UI_SETTINGS_ACTION_CYCLE_TEXT_SIZE ||
           action == MESH_UI_SETTINGS_ACTION_CYCLE_UPDATE_CHANNEL ||
           action == MESH_UI_SETTINGS_ACTION_TOGGLE_DEV_UPDATES ||
           action == MESH_UI_SETTINGS_ACTION_CYCLE_FIRMWARE_CHANNEL;
}

/* The two firmware installs, which are one press with two sheets in front of it. Asked as a
   predicate rather than compared inline for the reason mesh_ui_settings_action_is_forget() is:
   four places want the question and a fifth arriving is how they stop agreeing. */
bool mesh_ui_settings_action_is_install_firmware(enum mesh_ui_settings_action action) {
    return action == MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB ||
           action == MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE;
}

/* Spelled out rather than "everything that needs confirming, plus the position pair": the
   forget rows need the sheet too and are the one thing in that section the radio never hears
   about, so the two questions stopped having the same answer. */
bool mesh_ui_settings_action_is_radio(enum mesh_ui_settings_action action) {
    return action == MESH_UI_SETTINGS_ACTION_REBOOT || action == MESH_UI_SETTINGS_ACTION_SHUTDOWN ||
           action == MESH_UI_SETTINGS_ACTION_RESET_NODEDB ||
           action == MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG ||
           action == MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE ||
           action == MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION ||
           action == MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION ||
           action == MESH_UI_SETTINGS_ACTION_SET_HAM_MODE ||
           action == MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG ||
           action == MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG ||
           action == MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP ||
           action == MESH_UI_SETTINGS_ACTION_REQUEST_HISTORY;
}

bool mesh_ui_settings_action_is_forget(enum mesh_ui_settings_action action) {
    return action == MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES ||
           action == MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES;
}

void mesh_ui_settings_confirm_title(enum mesh_ui_settings_section section, uint8_t channel,
                                    enum mesh_ui_settings_action action, char *out,
                                    size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    switch (action) {
    case MESH_UI_SETTINGS_ACTION_REBOOT:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TITLE_REBOOT));
        return;
    case MESH_UI_SETTINGS_ACTION_SHUTDOWN:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TITLE_SHUTDOWN));
        return;
    case MESH_UI_SETTINGS_ACTION_RESET_NODEDB:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TITLE_RESET_DB));
        return;
    case MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TITLE_FORGET_OFF));
        return;
    case MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TITLE_FORGET_ALL));
        return;
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TITLE_FACTORY_CFG));
        return;
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TITLE_FACTORY_DEV));
        return;
    case MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TITLE_BACKUP));
        return;
    case MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TITLE_RESTORE));
        return;
    case MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TITLE_RM_BACKUP));
        return;
    case MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TITLE_FW_USB));
        return;
    case MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TITLE_FW_BLE));
        return;
    case MESH_UI_SETTINGS_ACTION_SET_HAM_MODE:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TITLE_HAM_MODE));
        return;
    /* The one verb in this switch that names a slot, which is why it is here rather than above:
       a sheet that asked "clear the channel?" over a list of eight would be asking about
       whichever one the reader had in mind. */
    case MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL:
        inkcell_str_format(out, out_len, MESH_STR_CONFIRM_TITLE_CLEAR_CHAN, (unsigned)channel);
        return;
    default:
        break;
    }
    if (section == MESH_UI_SETTINGS_CHANNELS && channel != MESH_UI_SETTINGS_NO_CHANNEL) {
        inkcell_str_format(out, out_len, MESH_STR_CONFIRM_TITLE_SAVE_CHANNEL, (unsigned)channel);
        return;
    }
    inkcell_str_format(out, out_len, MESH_STR_CONFIRM_TITLE_SAVE,
                       mesh_ui_settings_section_name(section));
}

const char *mesh_ui_settings_confirm_accept(enum mesh_ui_settings_action action) {
    switch (action) {
    case MESH_UI_SETTINGS_ACTION_REBOOT:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_REBOOT);
    case MESH_UI_SETTINGS_ACTION_SHUTDOWN:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_SHUTDOWN);
    case MESH_UI_SETTINGS_ACTION_RESET_NODEDB:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_RESET_DB);
    case MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_FORGET_OFF);
    case MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_FORGET_ALL);
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_FACTORY_CFG);
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_FACTORY_DEV);
    case MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_BACKUP);
    case MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_RESTORE);
    case MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_RM_BACKUP);
    case MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_FW_USB);
    case MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_FW_BLE);
    case MESH_UI_SETTINGS_ACTION_SET_HAM_MODE:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_HAM_MODE);
    /* "Clear the slot", not "Save": what stands behind this sheet is a write like any other,
       but agreeing to a save is not what the reader is being asked. */
    case MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_CLEAR_CHAN);
    /* "Join", not "Import": what the user is agreeing to is being on somebody else's mesh, and
       the word for the file operation says nothing about that. */
    case MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_IMPORT);
    /* "Add", not "Import": what the press does is put one node in a list. */
    case MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_ADD_CONTACT);
    default:
        return inkcell_str(MESH_STR_CONFIRM_ACCEPT_SAVE);
    }
}

void mesh_ui_settings_confirm_add_subject(const struct mesh_ui_settings *settings,
                                          enum mesh_ui_settings_action action, char *text,
                                          size_t text_len) {
    if (settings == NULL || text == NULL || text_len == 0U || settings->admin_dest == 0U) {
        return;
    }
    /* The presses that stay home whatever the tab is pointed at: the two that empty this
       client's own roster, and the two that write firmware over a bus to the radio in front of
       us. Saying "this goes over the mesh" of any of them would be false. */
    if (mesh_ui_settings_action_is_forget(action) ||
        mesh_ui_settings_action_is_install_firmware(action)) {
        return;
    }
    const size_t at = strlen(text);
    if (at >= text_len) {
        return;
    }
    inkcell_str_format(text + at, text_len - at, MESH_STR_CONFIRM_TEXT_REMOTE,
                       settings->admin_dest_name);
}

void mesh_ui_settings_confirm_text(enum mesh_ui_settings_section section,
                                   enum mesh_ui_settings_action action, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    /*
     * The actions first: they are the only rows here that cannot be undone by pressing the
     * opposite one, so each says what is lost rather than only what happens. A reset that
     * spares something says so too - "favorites excepted" is the difference between a press
     * the user regrets and one they do not.
     */
    switch (action) {
    case MESH_UI_SETTINGS_ACTION_REBOOT:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_REBOOT));
        return;
    case MESH_UI_SETTINGS_ACTION_SHUTDOWN:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_SHUTDOWN));
        return;
    /* Four wrapped lines is what the sheet draws, so each of these stops inside it: a warning
       whose last clause is cut off is worse than a shorter one. */
    case MESH_UI_SETTINGS_ACTION_RESET_NODEDB:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_RESET_DB));
        return;
    case MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_FORGET_OFF));
        return;
    case MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_FORGET_ALL));
        return;
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_FACTORY_CFG));
        return;
    case MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_FACTORY_DEV));
        return;
    case MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_BACKUP));
        return;
    case MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_RESTORE));
        return;
    /* The two that are not about settings at all. Each says what is actually true of its own
       bus, which is the whole reason there are two: an interrupted USB write leaves a
       bootloader anything can talk to, and an interrupted OTA leaves a radio off the mesh. */
    case MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_FW_USB));
        return;
    case MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_FW_BLE));
        return;
    case MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_RM_BACKUP));
        return;
    /* The one sheet here that is neither a reset nor an install: what it costs is the mesh the
       radio is on, because amateur rules require the encryption it turns off. */
    case MESH_UI_SETTINGS_ACTION_SET_HAM_MODE:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_HAM_MODE));
        return;
    /* Answered here rather than by the Channels arm below, which is a save's sheet and says
       the link may drop. That is true of this write too and is not what the reader needs from
       it: the link comes back and the key does not. */
    case MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_CLEAR_CHAN));
        return;
    default:
        break;
    }
    switch (section) {
    case MESH_UI_SETTINGS_BLUETOOTH:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_BLUETOOTH));
        break;
    case MESH_UI_SETTINGS_CHANNELS:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_CHANNELS));
        break;
    case MESH_UI_SETTINGS_LORA:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_LORA));
        break;
    case MESH_UI_SETTINGS_SECURITY:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_SECURITY));
        break;
    case MESH_UI_SETTINGS_POWER:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_POWER));
        break;
    default:
        snprintf(out, out_len, "%s", inkcell_str(MESH_STR_CONFIRM_TEXT_DEFAULT));
        break;
    }
}

const struct mesh_ui_setting_edit *
mesh_ui_settings_find_edit(const struct mesh_ui_setting_edit *edits, size_t edit_count,
                           enum mesh_ui_setting_field field) {
    /* Past the table is no field, and must not wrap to one an edit is held for. */
    if (edits == NULL || field == MESH_UI_FIELD_NONE || (unsigned)field >= MESH_UI_FIELD_COUNT) {
        return NULL;
    }
    for (size_t i = 0; i < edit_count && i < MESH_UI_SETTINGS_EDITS_MAX; ++i) {
        if (edits[i].field == (uint16_t)field) {
            return &edits[i];
        }
    }
    return NULL;
}
