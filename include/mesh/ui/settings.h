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
    /* This client, not the radio: version, where its data lives, and the self-update rows.
       First because it is the one section that means anything without a connection. */
    MESH_UI_SETTINGS_ABOUT = 0,
    MESH_UI_SETTINGS_RADIO, /* firmware, hardware, node number */
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
    /* Things the radio does rather than keeps: reboot, shutdown, the resets. Last because a
       cursor that overshoots the list should land on nothing worse than the row above it. */
    MESH_UI_SETTINGS_ACTIONS,
    /* Not a config section: the list of the ones that are modules, with each module's enabled
       state as its value. A row here opens that module the way a Channels row opens a slot.
       Declared last so every value above it keeps the number it had; where it *sits* in the
       list is mesh_ui_settings_root_at()'s business, not the enum's. */
    MESH_UI_SETTINGS_MODULES,
    /* Phase 10's six: no new UI primitive between them, and every field a bool, a number, an
       enum or one short string. Declared after MODULES for the reason MODULES was declared
       last - the list order is mesh_ui_settings_module_at()'s, not the enum's. */
    MESH_UI_SETTINGS_NEIGHBOR_INFO,
    MESH_UI_SETTINGS_RANGE_TEST,
    MESH_UI_SETTINGS_PAXCOUNTER,
    MESH_UI_SETTINGS_TAK,
    MESH_UI_SETTINGS_AMBIENT,
    MESH_UI_SETTINGS_STATUS_MESSAGE,
    /* Phase 11's three: large enough to need the heading rows, still no new row model. */
    MESH_UI_SETTINGS_DETECTION,
    MESH_UI_SETTINGS_EXT_NOTIFICATION,
    MESH_UI_SETTINGS_TRAFFIC,
    MESH_UI_SETTINGS_SECTION_COUNT,
};

/*
 * The two lists the Settings tab draws, as tables rather than as enum ranges.
 *
 * The section list stopped being 0..SECTION_COUNT in phase 9: modules sit one level down, so
 * the top level is a curated order and the Modules list is another. Keeping them as accessors
 * lets the enum above stay in declaration order - which is what every switch in the client is
 * written against - while the rows are ordered for the person reading them.
 */
uint32_t mesh_ui_settings_root_count(void);
enum mesh_ui_settings_section mesh_ui_settings_root_at(uint32_t row);
uint32_t mesh_ui_settings_module_count(void);
enum mesh_ui_settings_section mesh_ui_settings_module_at(uint32_t row);
/* True for a section that lives under Modules rather than at the top level. */
bool mesh_ui_settings_section_is_module(enum mesh_ui_settings_section section);

enum mesh_ui_setting_kind {
    MESH_UI_SETTING_INFO = 0, /* read-only fact */
    MESH_UI_SETTING_TOGGLE,
    MESH_UI_SETTING_ENUM,
    MESH_UI_SETTING_TEXT,
    MESH_UI_SETTING_NUMBER,
    MESH_UI_SETTING_KEY,
    MESH_UI_SETTING_ACTION,
    /* A group title inside a long section: dimmed, no value column, and A on it does nothing.
       The same row mesh_ui_node_item has drawn since the node detail existed. A heading is
       never added or removed by an edit - a row count that moves under the cursor mid-edit
       moves the cursor, which is the rule the LoRa trio is always listed for. */
    MESH_UI_SETTING_HEADING,
};

/* Editable settings. Each is one protobuf field; app.c turns an edit back into the protobuf
   (mesh_app_apply_setting_edit) and this module knows how to show and step it. */
enum mesh_ui_setting_field {
    MESH_UI_FIELD_NONE = 0,
    MESH_UI_FIELD_USER_LONG_NAME,
    MESH_UI_FIELD_USER_SHORT_NAME,
    MESH_UI_FIELD_USER_LICENSED,
    MESH_UI_FIELD_USER_UNMESSAGEABLE,
    MESH_UI_FIELD_DEVICE_ROLE,  /* enum: meshtastic_Config_DeviceConfig_Role, 0..12 */
    MESH_UI_FIELD_DEVICE_TZDEF, /* text: a POSIX TZ string, e.g. AST4 or EST5EDT,M3.2.0,M11.1.0 */
    MESH_UI_FIELD_DEVICE_REBROADCAST,
    MESH_UI_FIELD_DEVICE_NODEINFO_SECS,
    /* Shown the right way up: the row reads "LED heartbeat on", the protobuf field is
       led_heartbeat_disabled, and app.c inverts it on the way back. */
    MESH_UI_FIELD_DEVICE_LED_HEARTBEAT,
    MESH_UI_FIELD_DEVICE_DOUBLE_TAP,
    MESH_UI_FIELD_DISPLAY_SCREEN_ON,
    MESH_UI_FIELD_DISPLAY_CAROUSEL,
    MESH_UI_FIELD_DISPLAY_COMPASS,
    MESH_UI_FIELD_DISPLAY_12H,
    MESH_UI_FIELD_DISPLAY_UNITS,
    MESH_UI_FIELD_DISPLAY_FLIP,
    MESH_UI_FIELD_POSITION_GPS_MODE,
    MESH_UI_FIELD_POSITION_BROADCAST_SECS,
    MESH_UI_FIELD_POSITION_SMART,
    MESH_UI_FIELD_POSITION_SMART_DISTANCE, /* number: metres */
    MESH_UI_FIELD_POSITION_SMART_INTERVAL,
    MESH_UI_FIELD_POSITION_GPS_INTERVAL,
    /* Decimal degrees as text, e.g. "44.64880" and "-63.57520", and metres above sea level.
       Text rather than a number stepper because a coordinate has no useful presets and an
       altitude can be negative. They are not saved with the section: they are what the
       "Set fixed position" action reads, because the firmware takes them through
       set_fixed_position rather than through set_config. */
    MESH_UI_FIELD_POSITION_LATITUDE,
    MESH_UI_FIELD_POSITION_LONGITUDE,
    MESH_UI_FIELD_POSITION_ALTITUDE,
    MESH_UI_FIELD_POWER_SAVING,
    MESH_UI_FIELD_POWER_LS_SECS,
    MESH_UI_FIELD_POWER_MIN_WAKE,
    MESH_UI_FIELD_POWER_WAIT_BT,
    MESH_UI_FIELD_POWER_SHUTDOWN,
    MESH_UI_FIELD_MQTT_ENABLED,
    MESH_UI_FIELD_MQTT_ADDRESS,
    MESH_UI_FIELD_MQTT_USERNAME,
    MESH_UI_FIELD_MQTT_PASSWORD,
    MESH_UI_FIELD_MQTT_ROOT,
    MESH_UI_FIELD_MQTT_ENCRYPTION,
    MESH_UI_FIELD_MQTT_TLS,
    MESH_UI_FIELD_MQTT_MAP_REPORTING,
    /* MQTTConfig.map_report_settings, a submessage rather than a flat field. Listed under the
       toggle it belongs to; the firmware ignores them with map reporting off. */
    MESH_UI_FIELD_MQTT_MAP_INTERVAL,
    MESH_UI_FIELD_MQTT_MAP_PRECISION, /* the same precision presets a channel's position uses */
    MESH_UI_FIELD_MQTT_MAP_LOCATION,
    MESH_UI_FIELD_SF_ENABLED,
    MESH_UI_FIELD_SF_HEARTBEAT,
    MESH_UI_FIELD_SF_SERVER,
    MESH_UI_FIELD_SF_RECORDS,
    MESH_UI_FIELD_SF_HISTORY_MAX,
    MESH_UI_FIELD_SF_HISTORY_WINDOW,
    MESH_UI_FIELD_TELEMETRY_DEVICE,
    MESH_UI_FIELD_TELEMETRY_INTERVAL,
    MESH_UI_FIELD_TELEMETRY_ENVIRONMENT,
    MESH_UI_FIELD_TELEMETRY_ENV_INTERVAL,
    MESH_UI_FIELD_TELEMETRY_ENV_SCREEN,
    MESH_UI_FIELD_TELEMETRY_ENV_FAHRENHEIT,
    MESH_UI_FIELD_TELEMETRY_AIR_QUALITY,
    MESH_UI_FIELD_TELEMETRY_AIR_INTERVAL,
    MESH_UI_FIELD_TELEMETRY_AIR_SCREEN,
    MESH_UI_FIELD_TELEMETRY_POWER,
    MESH_UI_FIELD_TELEMETRY_POWER_INTERVAL,
    MESH_UI_FIELD_TELEMETRY_POWER_SCREEN,
    MESH_UI_FIELD_TELEMETRY_HEALTH,
    MESH_UI_FIELD_TELEMETRY_HEALTH_INTERVAL,
    MESH_UI_FIELD_TELEMETRY_HEALTH_SCREEN,
    MESH_UI_FIELD_CHANNEL_NAME,
    MESH_UI_FIELD_CHANNEL_ROLE, /* enum: 0 disabled, 1 secondary; the primary slot is read-only */
    MESH_UI_FIELD_CHANNEL_KEY,  /* kind KEY: number is an enum mesh_ui_psk_choice */
    MESH_UI_FIELD_CHANNEL_UPLINK,
    MESH_UI_FIELD_CHANNEL_DOWNLINK,
    MESH_UI_FIELD_CHANNEL_POSITION,
    MESH_UI_FIELD_BT_ENABLED,
    MESH_UI_FIELD_BT_MODE,
    MESH_UI_FIELD_BT_PIN, /* text: six digits */
    MESH_UI_FIELD_LORA_REGION,
    MESH_UI_FIELD_LORA_USE_PRESET,
    MESH_UI_FIELD_LORA_PRESET,
    MESH_UI_FIELD_LORA_BANDWIDTH,
    MESH_UI_FIELD_LORA_SPREAD,
    MESH_UI_FIELD_LORA_CODING,
    MESH_UI_FIELD_LORA_HOPS,
    MESH_UI_FIELD_LORA_TX_ENABLED,
    MESH_UI_FIELD_LORA_TX_POWER, /* number: dBm, 0 = the radio's maximum */
    MESH_UI_FIELD_LORA_IGNORE_MQTT,
    MESH_UI_FIELD_LORA_OK_TO_MQTT,
    MESH_UI_FIELD_SECURITY_PRIVATE_KEY, /* KEY: keep / new random / typed (restore a backup) */
    MESH_UI_FIELD_SECURITY_ADMIN_KEY_0, /* KEY: keep / none / typed */
    MESH_UI_FIELD_SECURITY_ADMIN_KEY_1,
    MESH_UI_FIELD_SECURITY_ADMIN_KEY_2,
    MESH_UI_FIELD_SECURITY_MANAGED,
    MESH_UI_FIELD_SECURITY_ADMIN_CHANNEL,
    MESH_UI_FIELD_SECURITY_SERIAL,
    MESH_UI_FIELD_SECURITY_DEBUG_LOG,
    MESH_UI_FIELD_SECURITY_SIGNATURE_POLICY,
    MESH_UI_FIELD_NEIGHBOR_ENABLED,
    /* The firmware floors this at 4 hours; the presets say so rather than offering a value it
       would quietly raise. */
    MESH_UI_FIELD_NEIGHBOR_INTERVAL,
    MESH_UI_FIELD_NEIGHBOR_OVER_LORA,
    MESH_UI_FIELD_RANGE_TEST_ENABLED,
    /* Seconds between test packets, 0 for receive-only. A test sender transmits to everyone on
       the channel on a timer, so the presets start where that is merely rude. */
    MESH_UI_FIELD_RANGE_TEST_SENDER,
    MESH_UI_FIELD_RANGE_TEST_SAVE,
    MESH_UI_FIELD_RANGE_TEST_CLEAR,
    MESH_UI_FIELD_PAX_ENABLED,
    MESH_UI_FIELD_PAX_INTERVAL,
    /* NUMBER rows over a signed value: the presets are all negative dBm, stored through a cast
       to uint32_t. See k_rssi_presets in settings.c for why that steps correctly. */
    MESH_UI_FIELD_PAX_WIFI_THRESHOLD,
    MESH_UI_FIELD_PAX_BLE_THRESHOLD,
    MESH_UI_FIELD_TAK_TEAM,
    MESH_UI_FIELD_TAK_ROLE,
    MESH_UI_FIELD_AMBIENT_LED,
    MESH_UI_FIELD_AMBIENT_CURRENT,
    MESH_UI_FIELD_AMBIENT_RED,
    MESH_UI_FIELD_AMBIENT_GREEN,
    MESH_UI_FIELD_AMBIENT_BLUE,
    MESH_UI_FIELD_STATUS_TEXT,
    MESH_UI_FIELD_DETECT_ENABLED,
    MESH_UI_FIELD_DETECT_NAME, /* text: 19 bytes; the firmware formats "<name> detected" */
    MESH_UI_FIELD_DETECT_MIN_BROADCAST,
    MESH_UI_FIELD_DETECT_STATE_BROADCAST,
    MESH_UI_FIELD_DETECT_SEND_BELL,
    /* A GPIO pin number. Offered as a number row over the plausible range rather than as free
       text: the radio's own wiring decides what is valid and we cannot know it, but a pin
       outside the range is certainly wrong. */
    MESH_UI_FIELD_DETECT_PIN,
    MESH_UI_FIELD_DETECT_TRIGGER,
    MESH_UI_FIELD_DETECT_PULLUP,
    MESH_UI_FIELD_EXTNOTIF_ENABLED,
    MESH_UI_FIELD_EXTNOTIF_ACTIVE,
    MESH_UI_FIELD_EXTNOTIF_OUTPUT_MS,
    MESH_UI_FIELD_EXTNOTIF_NAG,
    MESH_UI_FIELD_EXTNOTIF_PWM,
    MESH_UI_FIELD_EXTNOTIF_I2S,
    /* Three outputs, each a pin plus the two alerts that drive it. Grouped under headings
       rather than run flat: the six alert flags are otherwise unreadable. */
    MESH_UI_FIELD_EXTNOTIF_PIN,
    MESH_UI_FIELD_EXTNOTIF_ALERT_MSG,
    MESH_UI_FIELD_EXTNOTIF_ALERT_BELL,
    MESH_UI_FIELD_EXTNOTIF_PIN_VIBRA,
    MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_VIBRA,
    MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_VIBRA,
    MESH_UI_FIELD_EXTNOTIF_PIN_BUZZER,
    MESH_UI_FIELD_EXTNOTIF_ALERT_MSG_BUZZER,
    MESH_UI_FIELD_EXTNOTIF_ALERT_BELL_BUZZER,
    /* Traffic management has no enabled flag at all: upstream removed the bool toggles in
       favour of "a non-zero value implicitly enables it", so 0 is off on every row. */
    MESH_UI_FIELD_TRAFFIC_POSITION_INTERVAL,
    MESH_UI_FIELD_TRAFFIC_NODEINFO_HOPS,
    MESH_UI_FIELD_TRAFFIC_RATE_WINDOW,
    MESH_UI_FIELD_TRAFFIC_RATE_PACKETS,
    MESH_UI_FIELD_TRAFFIC_UNKNOWN_THRESHOLD,
    MESH_UI_FIELD_COUNT,
};

/* What an ACTION row does when A is pressed. Rows of kind MESH_UI_SETTING_ACTION carry one in
   `number`, so the nav can raise the right action without knowing what the section means. */
enum mesh_ui_settings_action {
    MESH_UI_SETTINGS_ACTION_NONE = 0,
    MESH_UI_SETTINGS_ACTION_CHECK_UPDATE,
    MESH_UI_SETTINGS_ACTION_INSTALL_UPDATE,
    /* Steps the update channel to the next one and saves it. An ACTION rather than an
       editable ENUM field because About is not a radio section: there is nothing for Y to
       write, so a pending edit waiting on a save would never be applied. */
    MESH_UI_SETTINGS_ACTION_CYCLE_UPDATE_CHANNEL,
    /* Lets a build that is not a release install what it finds. Only emitted on such a build:
       the guard it lifts does not exist on a release, so neither does the row. */
    MESH_UI_SETTINGS_ACTION_TOGGLE_DEV_UPDATES,
    /* Radio actions. Every one of these goes through the confirm overlay, so A on the row
       opens the question rather than doing the thing. */
    MESH_UI_SETTINGS_ACTION_REBOOT,
    MESH_UI_SETTINGS_ACTION_SHUTDOWN,
    MESH_UI_SETTINGS_ACTION_RESET_NODEDB,
    MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG,
    MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE,
    /* Position section. Not destructive, so these two are the radio actions that do *not* go
       through the confirm overlay; they read the latitude/longitude/altitude rows above them,
       which is why a radio action carries the section's pending edits. */
    MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION,
    MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION,
};

/* Which press writes this field (mesh/ui/nav.h). */
enum mesh_ui_setting_consumer mesh_ui_settings_field_consumer(enum mesh_ui_setting_field field);

/* True for the rows above that ask the radio to do something rather than the client: they all
   reach the app as MESH_UI_ACTION_RADIO_ACTION. */
bool mesh_ui_settings_action_is_radio(enum mesh_ui_settings_action action);
/* True for the ones that cannot be undone by pressing the opposite row, which the nav puts
   behind the confirm overlay. */
bool mesh_ui_settings_action_needs_confirm(enum mesh_ui_settings_action action);

/* What a KEY edit asks for. KEEP is the radio's current key (no edit); TYPED carries hex in
   the edit's text. The random choices are generated by the app when the write is built. */
enum mesh_ui_psk_choice {
    MESH_UI_PSK_KEEP = 0,
    MESH_UI_PSK_DEFAULT,    /* the one-byte "default key" shorthand */
    MESH_UI_PSK_RANDOM_128, /* new random AES-128 */
    MESH_UI_PSK_RANDOM_256, /* new random AES-256 */
    MESH_UI_PSK_NONE,       /* no encryption */
    MESH_UI_PSK_TYPED,
    MESH_UI_PSK_CHOICE_COUNT,
};
#define MESH_UI_PSK_CHOICE_BIT(choice) (1U << (unsigned)(choice))

#define MESH_UI_SETTINGS_LABEL_MAX 24U
#define MESH_UI_SETTINGS_VALUE_MAX 48U
/* Telemetry is fifteen fields plus five headings; External notification will be worse. The
   list is built onto the stack every frame, so this is ~4.9 KB in a loop that has no threads
   to share it with. */
#define MESH_UI_SETTINGS_ITEMS_MAX 32U

struct mesh_ui_settings_item {
    char label[MESH_UI_SETTINGS_LABEL_MAX];
    char value[MESH_UI_SETTINGS_VALUE_MAX];
    enum mesh_ui_setting_kind kind;
    enum mesh_ui_setting_field field;    /* NONE: read-only */
    bool dirty;                          /* value shown is a pending edit */
    uint32_t number;                     /* toggle 0/1, enum index, raw number, or key choice */
    char text[MESH_UI_SETTING_TEXT_MAX]; /* TEXT: the raw string; KEY: the key as hex */
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

/* Sections whose write can cut this client off or the radio off the mesh (Bluetooth:
   re-pairing; Channels: a changed key; LoRa: region and preset; Security: identity and
   managed mode). The nav asks before saving them. */
bool mesh_ui_settings_section_needs_confirm(enum mesh_ui_settings_section section);

/*
 * What the confirm overlay says. It stands in front of two different things - a section save
 * and a radio action - so all three strings come from here rather than from the backend, and
 * `action` picks between them: MESH_UI_SETTINGS_ACTION_NONE is a save of `section` (with
 * `channel` naming the slot in the Channels section), anything else is that action.
 */
void mesh_ui_settings_confirm_title(enum mesh_ui_settings_section section, uint8_t channel,
                                    enum mesh_ui_settings_action action, char *out, size_t out_len);
void mesh_ui_settings_confirm_text(enum mesh_ui_settings_section section,
                                   enum mesh_ui_settings_action action, char *out, size_t out_len);
/* The verb on the overlay's first row ("Save to radio", "Reboot now", ...). */
const char *mesh_ui_settings_confirm_accept(enum mesh_ui_settings_action action);

/* KEY fields: which choices Left/Right offer (a bitmask of MESH_UI_PSK_CHOICE_BIT), and
   whether a key of `len` bytes is acceptable for the field. */
uint32_t mesh_ui_settings_key_choices(enum mesh_ui_setting_field field);
bool mesh_ui_settings_key_len_ok(enum mesh_ui_setting_field field, size_t len);

/* Keys as text. key_text() is base64, what the Meshtastic apps show and accept, so a key
   read off the Brick can be typed into a phone and vice versa. parse() takes base64 or hex
   (an even number of hex digits); an empty string is an empty key. */
/* Coordinates as decimal degrees, to and from Meshtastic's fixed-point 1e-7 form. parse()
   takes a plain decimal ("44.6488", "-63.57520") and rejects anything outside +/- `limit`
   degrees or with trailing rubbish; an empty string is not a coordinate. */
void mesh_ui_settings_coord_text(int32_t value_i, char *out, size_t out_len);
bool mesh_ui_settings_coord_parse(const char *text, int32_t limit_degrees, int32_t *out_i);

void mesh_ui_settings_key_text(const uint8_t *key, size_t len, char *out, size_t out_len);
void mesh_ui_settings_key_hex(const uint8_t *key, size_t len, char *out, size_t out_len);
bool mesh_ui_settings_key_parse(const char *text, uint8_t *out, size_t out_cap, size_t *out_len);

/* The channel slot behind row `row` of the Channels list, or -1. */
int mesh_ui_settings_channel_at_row(const struct mesh_ui_settings *settings,
                                    const struct mesh_ui_handshake_state *handshake, uint32_t row);

/* The pending edit for `field` among `edits`, or NULL. */
const struct mesh_ui_setting_edit *
mesh_ui_settings_find_edit(const struct mesh_ui_setting_edit *edits, size_t edit_count,
                           enum mesh_ui_setting_field field);

/* Whether the radio has sent the data this section shows. `handshake` may be NULL (no radio);
   it is only consulted for the Channels and Radio sections. */
bool mesh_ui_settings_section_loaded(const struct mesh_ui_settings *settings,
                                     const struct mesh_ui_handshake_state *handshake,
                                     enum mesh_ui_settings_section section);

/* Items in a section for the current data. Zero when the section has not loaded. `channel`
   is the open slot in the Channels section, MESH_UI_SETTINGS_NO_CHANNEL otherwise. */
uint32_t mesh_ui_settings_item_count(const struct mesh_ui_settings *settings,
                                     const struct mesh_ui_handshake_state *handshake,
                                     enum mesh_ui_settings_section section, uint8_t channel);

/* Describes one row, with pending `edits` (may be NULL) shown in place of the radio's values.
   Returns false when `row` is out of range. */
bool mesh_ui_settings_item(const struct mesh_ui_settings *settings,
                           const struct mesh_ui_handshake_state *handshake,
                           const struct mesh_ui_setting_edit *edits, size_t edit_count,
                           enum mesh_ui_settings_section section, uint8_t channel, uint32_t row,
                           struct mesh_ui_settings_item *out);

#ifdef __cplusplus
}
#endif
