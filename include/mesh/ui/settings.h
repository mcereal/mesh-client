#pragma once

/*
 * The Settings tab as data: a list of sections, each a list of items with a label, a value
 * already formatted for display, and a kind. Backends draw the list; the nav walks it. Phase 1
 * (docs/settings-roadmap.md) renders every kind read-only; the kinds are here so later phases
 * can wire up A per item without changing the backends.
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

#define MESH_UI_SETTINGS_LABEL_MAX 24U
#define MESH_UI_SETTINGS_VALUE_MAX 48U
#define MESH_UI_SETTINGS_ITEMS_MAX 16U

struct mesh_ui_settings_item {
    char label[MESH_UI_SETTINGS_LABEL_MAX];
    char value[MESH_UI_SETTINGS_VALUE_MAX];
    enum mesh_ui_setting_kind kind;
};

const char *mesh_ui_settings_section_name(enum mesh_ui_settings_section section);

/* Whether the radio has sent the data this section shows. `handshake` may be NULL (no radio);
   it is only consulted for the Channels and Radio sections. */
bool mesh_ui_settings_section_loaded(const struct mesh_ui_settings *settings,
                                     const struct mesh_ui_handshake_state *handshake,
                                     enum mesh_ui_settings_section section);

/* Items in a section for the current data. Zero when the section has not loaded. */
uint32_t mesh_ui_settings_item_count(const struct mesh_ui_settings *settings,
                                     const struct mesh_ui_handshake_state *handshake,
                                     enum mesh_ui_settings_section section);

/* Describes one row. Returns false when `row` is out of range. */
bool mesh_ui_settings_item(const struct mesh_ui_settings *settings,
                           const struct mesh_ui_handshake_state *handshake,
                           enum mesh_ui_settings_section section, uint32_t row,
                           struct mesh_ui_settings_item *out);

#ifdef __cplusplus
}
#endif
