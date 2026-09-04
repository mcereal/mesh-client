#pragma once

/*
 * The Settings tab as data: a list of sections, each a list of items with a label, a value
 * already formatted for display, and a kind. Backends draw the list; the nav walks it.
 *
 * An item that can be changed names its `field`. The nav keeps pending edits per field (in
 * `struct mesh_ui_nav`) and this module renders them in place of the radio's value, marked
 * dirty, until the app writes them (docs/settings-roadmap.md, phase 2). Everything about a
 * field the nav needs to edit it blind - its kind, enum names, number presets, text cap - is
 * answered here so the nav never has to know what a field means.
 */

#include "mesh/ui/store.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mesh_ui_settings_section {
    MESH_UI_SETTINGS_RADIO = 0, /* firmware, hardware, node number */
    MESH_UI_SETTINGS_USER,
    MESH_UI_SETTINGS_DEVICE,
    MESH_UI_SETTINGS_DISPLAY,
    MESH_UI_SETTINGS_LORA,
    MESH_UI_SETTINGS_BLUETOOTH,
    MESH_UI_SETTINGS_CHANNELS,
    MESH_UI_SETTINGS_SECURITY,
    MESH_UI_SETTINGS_POSITION,
    MESH_UI_SETTINGS_POWER,
    MESH_UI_SETTINGS_MQTT,
    MESH_UI_SETTINGS_STORE_FORWARD,
    MESH_UI_SETTINGS_TELEMETRY,
    MESH_UI_SETTINGS_SECTION_COUNT,
};

enum mesh_ui_setting_kind {
    MESH_UI_SETTING_INFO = 0, /* read-only fact */
    MESH_UI_SETTING_TOGGLE,
    MESH_UI_SETTING_ENUM,
    MESH_UI_SETTING_TEXT,
    MESH_UI_SETTING_NUMBER,
    MESH_UI_SETTING_KEY,
    MESH_UI_SETTING_ACTION,
};

/* Editable settings. Each is one protobuf field; app.c turns an edit back into the protobuf
   (mesh_app_apply_setting_edit) and this module knows how to show and step it. */
enum mesh_ui_setting_field {
    MESH_UI_FIELD_NONE = 0,
    MESH_UI_FIELD_USER_LONG_NAME,
    MESH_UI_FIELD_USER_SHORT_NAME,
    MESH_UI_FIELD_USER_LICENSED,
    MESH_UI_FIELD_USER_UNMESSAGEABLE,
    MESH_UI_FIELD_DISPLAY_SCREEN_ON,
    MESH_UI_FIELD_DISPLAY_CAROUSEL,
    MESH_UI_FIELD_DISPLAY_COMPASS,
    MESH_UI_FIELD_DISPLAY_12H,
    MESH_UI_FIELD_DISPLAY_UNITS,
    MESH_UI_FIELD_DISPLAY_FLIP,
    MESH_UI_FIELD_SF_ENABLED,
    MESH_UI_FIELD_SF_HEARTBEAT,
    MESH_UI_FIELD_SF_SERVER,
    MESH_UI_FIELD_TELEMETRY_DEVICE,
    MESH_UI_FIELD_TELEMETRY_INTERVAL,
    MESH_UI_FIELD_TELEMETRY_ENVIRONMENT,
    MESH_UI_FIELD_TELEMETRY_ENV_SCREEN,
    MESH_UI_FIELD_TELEMETRY_ENV_FAHRENHEIT,
    MESH_UI_FIELD_TELEMETRY_AIR_QUALITY,
    MESH_UI_FIELD_TELEMETRY_POWER,
    MESH_UI_FIELD_COUNT,
};

#define MESH_UI_SETTINGS_LABEL_MAX 24U
#define MESH_UI_SETTINGS_VALUE_MAX 48U
#define MESH_UI_SETTINGS_ITEMS_MAX 16U

struct mesh_ui_settings_item {
    char label[MESH_UI_SETTINGS_LABEL_MAX];
    char value[MESH_UI_SETTINGS_VALUE_MAX];
    enum mesh_ui_setting_kind kind;
    enum mesh_ui_setting_field field;    /* NONE: read-only */
    bool dirty;                          /* value shown is a pending edit */
    uint32_t number;                     /* toggle 0/1, enum index, or the raw number */
    char text[MESH_UI_SETTING_TEXT_MAX]; /* TEXT: the raw string */
};

const char *mesh_ui_settings_section_name(enum mesh_ui_settings_section section);

/* Field descriptions for the nav and the keyboard title. */
const char *mesh_ui_settings_field_label(enum mesh_ui_setting_field field);
enum mesh_ui_setting_kind mesh_ui_settings_field_kind(enum mesh_ui_setting_field field);
enum mesh_ui_settings_section mesh_ui_settings_field_section(enum mesh_ui_setting_field field);
/* ENUM fields: how many values and their names. */
uint32_t mesh_ui_settings_enum_count(enum mesh_ui_setting_field field);
const char *mesh_ui_settings_enum_name(enum mesh_ui_setting_field field, uint32_t value);
/* NUMBER fields step through a preset list: the next preset above (delta > 0) or below
   (delta < 0) `value`, or `value` itself at either end. */
uint32_t mesh_ui_settings_number_step(enum mesh_ui_setting_field field, uint32_t value, int delta);
/* TEXT fields: the longest value the radio accepts, in bytes without the NUL. */
uint32_t mesh_ui_settings_text_max(enum mesh_ui_setting_field field);

/* The pending edit for `field` among `edits`, or NULL. */
const struct mesh_ui_setting_edit *
mesh_ui_settings_find_edit(const struct mesh_ui_setting_edit *edits, size_t edit_count,
                           enum mesh_ui_setting_field field);

/* Whether the radio has sent the data this section shows. `handshake` may be NULL (no radio);
   it is only consulted for the Channels and Radio sections. */
bool mesh_ui_settings_section_loaded(const struct mesh_ui_settings *settings,
                                     const struct mesh_ui_handshake_state *handshake,
                                     enum mesh_ui_settings_section section);

/* Items in a section for the current data. Zero when the section has not loaded. */
uint32_t mesh_ui_settings_item_count(const struct mesh_ui_settings *settings,
                                     const struct mesh_ui_handshake_state *handshake,
                                     enum mesh_ui_settings_section section);

/* Describes one row, with pending `edits` (may be NULL) shown in place of the radio's values.
   Returns false when `row` is out of range. */
bool mesh_ui_settings_item(const struct mesh_ui_settings *settings,
                           const struct mesh_ui_handshake_state *handshake,
                           const struct mesh_ui_setting_edit *edits, size_t edit_count,
                           enum mesh_ui_settings_section section, uint32_t row,
                           struct mesh_ui_settings_item *out);

#ifdef __cplusplus
}
#endif
