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

#include "mesh/i18n/strings.h"
#include "mesh/ui/icon.h"
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
    /* "About radio", the counterpart: firmware, hardware, node number, the admin session.
       Read-only like the one above it, which is the whole of what the two names promise - a
       row that can be changed lives in the section that owns it, never on an About screen. */
    MESH_UI_SETTINGS_RADIO,
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
       cursor that overshoots the list should land on nothing worse than the row above it, and
       grouped under headings that say whose node list each row would empty. */
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
    /*
     * The radio's own screen (DeviceUIConfig), which is not DisplayConfig and not this client's
     * theme: Display is the panel - how long it stays lit, which way up, metric or imperial -
     * and this is the graphical UI drawn on it. Sits beside Display in the root list so the
     * pair reads together; declared here, last, for the reason every section since phase 9 has
     * been - the enum's order is what the persisted cursor and the tests are written against,
     * and the list's order is mesh_ui_settings_root_at()'s business.
     */
    MESH_UI_SETTINGS_RADIO_UI,
    /*
     * The radio's canned message list. Under Modules, because that is the question it answers -
     * what is this radio running - even though it is the one entry there that is not a
     * ModuleConfig at all: the list is its own pair of admin verbs, and CannedMessageConfig
     * (which this client does not ship) is the module's *wiring*, not its words.
     */
    MESH_UI_SETTINGS_CANNED,
    MESH_UI_SETTINGS_SECTION_COUNT,
};

/*
 * How many canned messages the section offers, and how long each may be.
 *
 * The two are chosen together and cannot be chosen apart: the wire carries the whole list as
 * one 200-byte string, so six slots of 32 characters plus the five '|' separators is 197 and
 * seven of them would not fit. A radio already holding more than six keeps them - the extras
 * are carried across the save untouched rather than being dropped by a screen that could not
 * show them.
 */
#define MESH_UI_CANNED_SLOTS 6U
#define MESH_UI_CANNED_SLOT_MAX 32U

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
    /*
     * A read-only quantity whose *level* is the point: how far an update has downloaded, how
     * much of something is used up. `number` is permille, or MESH_UI_METER_UNKNOWN when work is
     * happening whose extent cannot be known.
     *
     * `value` is still filled in with the same fact in words, and that is deliberate rather than
     * redundant: a backend that cannot draw a bar - the CLI one - shows the row as an ordinary
     * fact and loses nothing. A kind is a description of the content, and it stays a description
     * of the content even when only one backend can act on it.
     */
    MESH_UI_SETTING_METER,
};

/* MESH_UI_SETTING_METER: the `number` for a step that is running with no fraction to report. */
#define MESH_UI_METER_UNKNOWN UINT32_MAX

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
    /*
     * DeviceUIConfig. `screen_lock`, `settings_lock` and `pin_code` are deliberately absent:
     * a client that can lock a radio's screen has no verb to unlock it again and the PIN is
     * not on the wire, so the two locks are shown read-only and the PIN not at all. So is
     * `language` - it is the one enum in this client whose wire values are not 0..n-1 (they
     * run 0..19 and then jump to 30), and every enum row here steps by modulo over the count
     * and names by value, so offering it would mean an index/value split across the nav, the
     * row builder and the segmented button rather than a row.
     */
    MESH_UI_FIELD_UI_THEME,
    MESH_UI_FIELD_UI_BRIGHTNESS,
    MESH_UI_FIELD_UI_SCREEN_TIMEOUT,
    MESH_UI_FIELD_UI_ALERT,
    MESH_UI_FIELD_UI_BANNER,
    MESH_UI_FIELD_UI_RING_TONE,
    MESH_UI_FIELD_UI_COMPASS_MODE,
    MESH_UI_FIELD_UI_GPS_FORMAT,
    MESH_UI_FIELD_UI_CLOCKFACE,
    /* One per canned-message slot. Separate fields rather than one indexed field because the
       edit record names a field and nothing else - the same reason the three admin keys are
       three fields. */
    MESH_UI_FIELD_CANNED_0,
    MESH_UI_FIELD_CANNED_1,
    MESH_UI_FIELD_CANNED_2,
    MESH_UI_FIELD_CANNED_3,
    MESH_UI_FIELD_CANNED_4,
    MESH_UI_FIELD_CANNED_5,
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
    /* Steps to the next theme and saves it. An ACTION for the same reason the update channel
       is one: About has no radio behind it, so there is nothing for Y to write. Not emitted
       when MESHCLIENT_THEME is holding the choice. */
    MESH_UI_SETTINGS_ACTION_CYCLE_THEME,
    MESH_UI_SETTINGS_ACTION_CYCLE_LANGUAGE,
    /* About's crash-report row. Local, and offered only when there is a report to discard. */
    MESH_UI_SETTINGS_ACTION_DISCARD_CRASH_REPORT,
    /* Radio actions. Every one of these goes through the confirm overlay, so A on the row
       opens the question rather than doing the thing. */
    MESH_UI_SETTINGS_ACTION_REBOOT,
    MESH_UI_SETTINGS_ACTION_SHUTDOWN,
    MESH_UI_SETTINGS_ACTION_RESET_NODEDB,
    /* The two rows that sit under it and are not radio actions at all: they drop this client's
       own roster, which a NodeDB reset deliberately leaves standing. Here rather than in About
       because this is the section somebody who has just reset the radio's database is already
       looking at. Both still go through the confirm sheet - a forgotten node comes back only
       when it speaks again. */
    MESH_UI_SETTINGS_ACTION_FORGET_OFF_RADIO_NODES,
    MESH_UI_SETTINGS_ACTION_FORGET_ALL_NODES,
    MESH_UI_SETTINGS_ACTION_FACTORY_RESET_CONFIG,
    MESH_UI_SETTINGS_ACTION_FACTORY_RESET_DEVICE,
    /* Position section. Not destructive, so these two are the radio actions that do *not* go
       through the confirm overlay; they read the latitude/longitude/altitude rows above them,
       which is why a radio action carries the section's pending edits. */
    MESH_UI_SETTINGS_ACTION_SET_FIXED_POSITION,
    MESH_UI_SETTINGS_ACTION_CLEAR_FIXED_POSITION,
    /*
     * The radio's whole configuration, copied to its own flash and brought back. Radio actions
     * like the resets above - nothing is read back, and what they move is not a section this
     * tab has rows for - and behind the confirm overlay for the same reason: a restore
     * overwrites every setting on the radio with whatever the backup held, and a backup
     * overwrites the previous one. Flash rather than SD because nothing on the wire says
     * whether a board has a card, and a press that silently does nothing is worse than a
     * press that is not offered.
     */
    MESH_UI_SETTINGS_ACTION_BACKUP_CONFIG,
    MESH_UI_SETTINGS_ACTION_RESTORE_CONFIG,
    MESH_UI_SETTINGS_ACTION_REMOVE_BACKUP,
    /*
     * Store & Forward: ask a router for the traffic that arrived while this client was off.
     *
     * A radio action in the sense that matters - it puts a packet on the air and the answer
     * comes back over minutes - but not one of the destructive ones, so no confirm overlay: it
     * asks for messages, and the worst a mistaken press costs is one small packet. It carries
     * no edits either; the row reads nothing above it.
     */
    MESH_UI_SETTINGS_ACTION_REQUEST_HISTORY,
    /*
     * About radio: ask what firmware exists for this board.
     *
     * A client action rather than a radio one - it reads two documents over HTTPS and touches
     * the radio not at all - and no confirm overlay, because nothing it does can be regretted.
     * The press that *installs* firmware is a different row that does not exist yet
     * (docs/radio-firmware-roadmap.md), and it will need the sheet this one does not.
     */
    MESH_UI_SETTINGS_ACTION_CHECK_RADIO_FIRMWARE,
    /*
     * Steps which of upstream's two release lists the firmware rows read.
     *
     * An ACTION rather than an editable ENUM for the reason About's own update channel is one:
     * the value is this client's, not the radio's, so there is nothing for Y to write and a
     * pending edit waiting on a save would never be applied.
     */
    MESH_UI_SETTINGS_ACTION_CYCLE_FIRMWARE_CHANNEL,
    /*
     * Install what the check found, over whichever bus the radio is on.
     *
     * **Two actions rather than one with the bus read off the snapshot**, because everything
     * the sheet in front of them says is different: over USB the worst case is a board sitting
     * in its bootloader that any computer can write again, and over Bluetooth the radio leaves
     * the mesh for a loader it cannot come back out of on its own. The confirm sheet's title,
     * body and accept label are all tables keyed on the action (see mesh_ui_settings_confirm_*),
     * so making the bus part of the action is what lets those tables stay tables - the
     * alternative is three of them growing a parameter they would each have to be right about.
     *
     * The row that emits one is the only place that has to know which bus this is, and it
     * already does: it is the row that would otherwise be drawing the refusal.
     */
    MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_USB,
    MESH_UI_SETTINGS_ACTION_INSTALL_FIRMWARE_BLE,
};

/* Which press writes this field (mesh/ui/nav.h). */
enum mesh_ui_setting_consumer mesh_ui_settings_field_consumer(enum mesh_ui_setting_field field);

/* True for the rows above that ask the radio to do something rather than the client: they all
   reach the app as MESH_UI_ACTION_RADIO_ACTION. */
bool mesh_ui_settings_action_is_radio(enum mesh_ui_settings_action action);
/* True for the two that ask this client to drop cached nodes. They share the Radio actions
   section and the confirm sheet with the rows above, and nothing else: they send nothing, so
   they work with no link at all and reach the app as MESH_UI_ACTION_FORGET_NODES. */
bool mesh_ui_settings_action_is_forget(enum mesh_ui_settings_action action);
/* True for the ones that cannot be undone by pressing the opposite row, which the nav puts
   behind the confirm overlay. */
bool mesh_ui_settings_action_needs_confirm(enum mesh_ui_settings_action action);
/* True for the two that install firmware on the radio. They are a radio action in every sense
   that matters and in none that this client's plumbing recognises: nothing goes through the
   admin queue that the app does not send itself, and what comes back is a bus rather than a
   read-back - so they reach the app as MESH_UI_ACTION_INSTALL_RADIO_FIRMWARE of their own. */
bool mesh_ui_settings_action_is_install_firmware(enum mesh_ui_settings_action action);

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
    /*
     * What this row is *about*, for the leading slot: the cloud on MQTT, the shield on
     * Security. MESH_UI_ICON_NONE on a row that is a setting rather than a subject, which is
     * every row of every section except the one that lists the modules.
     *
     * A section gives every row an icon or gives none, and that is a rule rather than an
     * observation: a leading slot is reserved for a whole list, so a list whose rows disagreed
     * would start its words in two different columns. mesh_ui_settings_section_icons_rows()
     * answers it for a caller, and a test holds every section to it.
     */
    enum mesh_ui_icon icon;
};

/*
 * The radio's canned message list, which the wire carries as one '|'-separated string.
 *
 * Two accessors rather than a parsed array because both readers want one entry at a time: the
 * section draws slot n, and the write builder walks every entry the radio holds - including
 * the ones past MESH_UI_CANNED_SLOTS, which it copies across untouched so a radio with more
 * messages than this screen has rows does not lose them to a save.
 */
/*
 * A character this TEXT field's value may not contain, or 0 when anything goes.
 *
 * Only the canned slots reserve one, and the reason is the shape of the wire rather than
 * anything about the words: the radio's list is a single '|'-separated string, so a slot
 * holding a '|' is read back as two messages, shifts every slot after it, and pushes the
 * entries this screen never showed off the end. The keyboard's symbols layer has a '|' on it,
 * so this is reachable by typing rather than only in theory.
 *
 * Filtered as the edit is committed rather than rejected at the save, so the row shows exactly
 * what the radio will be sent - the same bargain the length cap already makes.
 */
char mesh_ui_settings_field_reserved_char(enum mesh_ui_setting_field field);

uint32_t mesh_ui_settings_canned_count(const char *list);
void mesh_ui_settings_canned_entry(const char *list, uint32_t index, char *out, size_t out_len);

const char *mesh_ui_settings_section_name(enum mesh_ui_settings_section section);
/* The same name as a catalog id, for a caller that has to carry it rather than draw it - the
   help topic's subject, which is ids the whole way down so that a test can read one with no
   locale in force. MESH_STR_NONE for a section past the end. */
enum mesh_str_id mesh_ui_settings_section_label(enum mesh_ui_settings_section section);

/*
 * What a section is about, as an icon: the leading slot on a row that *opens* that section.
 *
 * Beside the name because it is the same kind of fact - what this section is - answered for the
 * same two lists: the settings root, and Modules, which is a list of sections wearing a
 * section's clothes. A backend with no icons (the CLI) ignores it exactly as it ignores the
 * chevron.
 *
 * Three sections answer with an icon another part of the UI already owns, because they are
 * saying the same thing it says: "About radio" with MESH_UI_ICON_RADIO, Bluetooth and Channels
 * with their own runes.
 */
enum mesh_ui_icon mesh_ui_settings_section_icon(enum mesh_ui_settings_section section);

/*
 * What a section is *for*, as a catalog id: the paragraph the help screen opens with.
 *
 * An id rather than a `const char *` so a caller can ask whether there is anything to say
 * without a strlen, and so the fb backend, the CLI and the tests cannot each invent their own
 * idea of what an absent note looks like. Never MESH_STR_NONE for a real section - see the
 * table in settings.c for why that is a rule rather than an observation.
 */
enum mesh_str_id mesh_ui_settings_section_note(enum mesh_ui_settings_section section);

/* Whether this section's *items* carry a leading icon - true only of Modules, whose rows are
   sections. What lets a renderer declare the slot once for the list instead of testing a row. */
bool mesh_ui_settings_section_icons_rows(enum mesh_ui_settings_section section);

/*
 * Position precision as a distance rather than a bit count: 0 is off, 32 or more is precise,
 * and 10..19 are the ten steps the phone apps label ("~23 km" down to "~45 m"). Public
 * because the node detail asks the same question of a *received* fix that the channel's
 * position_precision row asks of an outgoing one - and a rounded location described two
 * different ways on two screens is how a client comes to disagree with itself about how much
 * it knows. Writes at most `out_len` bytes including the NUL.
 */
void mesh_ui_settings_format_precision(uint32_t bits, char *out, size_t out_len);
/*
 * The same answer as a number of metres, for a caller that has to draw the footprint rather than
 * name it - the map's ring around an approximate marker.
 *
 * 0 whenever there is no distance to give: `bits` of 0 is "the sender did not say" rather than
 * "exact", and anything outside the range the channel's own setting offers is a count this
 * client has no table for. All three mean the same thing to a caller: draw nothing.
 */
uint32_t mesh_ui_settings_precision_metres(uint32_t bits);

/* Field descriptions for the nav and the keyboard title. */
const char *mesh_ui_settings_field_label(enum mesh_ui_setting_field field);
/* The same label as a catalog id, for a caller assembling a structure of ids rather than a row
   of text - the help topic is the one, and holding ids there is what lets a test read it with no
   locale in force. */
enum mesh_str_id mesh_ui_settings_field_label_id(enum mesh_ui_setting_field field);
enum mesh_ui_setting_kind mesh_ui_settings_field_kind(enum mesh_ui_setting_field field);
enum mesh_ui_settings_section mesh_ui_settings_field_section(enum mesh_ui_setting_field field);
/*
 * What one setting does, as a catalog id, or MESH_STR_NONE for a row whose label is already the
 * whole explanation - which is most of them, on purpose. A note is for the row where knowing the
 * name does not tell you what happens if you get it wrong. See docs/help.md.
 */
enum mesh_str_id mesh_ui_settings_field_note(enum mesh_ui_setting_field field);
/* ENUM fields: how many values and their names. */
uint32_t mesh_ui_settings_enum_count(enum mesh_ui_setting_field field);
const char *mesh_ui_settings_enum_name(enum mesh_ui_setting_field field, uint32_t value);
/* NUMBER fields step through a preset list: the next preset above (delta > 0) or below
   (delta < 0) `value`, or `value` itself at either end. */
uint32_t mesh_ui_settings_number_step(enum mesh_ui_setting_field field, uint32_t value, int delta);
/*
 * Where a NUMBER field's value sits on the field's own scale, for a row that draws it as one.
 *
 * `position` is on the same 0..1000 a meter's fill is on and `stops` is how many choices there
 * are to mark. `unplaced` is the answer for a value the track has no room for - see
 * mesh_ui_settings_number_track().
 */
struct mesh_ui_settings_track {
    int32_t position;
    uint32_t stops;
    bool unplaced;
};

/*
 * Fills `out` for a NUMBER field whose presets measure something, and returns false for every
 * other kind of field - including a NUMBER field whose numbers *name* something rather than
 * measure it (a GPIO pin, a spreading factor, a count of coordinate bits), because a length
 * drawn across one of those is a claim about magnitude that the number does not make. A field
 * says which it is in its own table entry; nothing here derives it, because nothing in a list of
 * integers says whether they are seconds or pins.
 *
 * The stops are evenly spaced and a value between two of them is interpolated, so a preset list
 * that climbs geometrically is a track the reader can aim at rather than eight choices crowded
 * into its first sixth.
 *
 * `unplaced` comes back true for a value below the track's bottom stop, which happens two ways.
 * The field's leading 0 may be a word - "default", "max" - stood outside the scale by
 * SCALE_PRESETS_AFTER_ZERO(). Or the list may simply start above zero, because whatever receives
 * the setting refuses anything below that (the public map drops a report under an hour, the
 * firmware floors neighbour info at four) while a radio nobody has configured still reports 0.
 * Either way the value is not at the bottom of the scale, it is not on the scale at all, and the
 * honest drawing is a track with nothing on it rather than a handle somewhere it does not
 * belong. `position` is 0 and `stops` still counts the marks, so a caller can lay the control out
 * without testing first.
 *
 * One function answers position, count and placement together, for the reason
 * fb_segmented_cols() answers width and form together: a control measured twice is a control
 * that disagrees with itself.
 */
bool mesh_ui_settings_number_track(enum mesh_ui_setting_field field, uint32_t value,
                                   struct mesh_ui_settings_track *out);
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

/*
 * The whole section at once: fills `out` and returns how many rows were written, at most `max`.
 *
 * The same rows mesh_ui_settings_item() answers one at a time, and the reason to have both is
 * that a screen which has to *measure* before it draws needs every row before it places the
 * first one - the shape mesh_ui_node_detail_build() already has, for the same reason: a list
 * whose rows are not all one height is a list the model must be told about up front.
 *
 * It is also the cheaper of the two by a whole order. Each singular call rebuilds the section
 * from the radio's config, so a screen asking row by row builds it once per row; this builds it
 * once.
 */
uint32_t mesh_ui_settings_items(const struct mesh_ui_settings *settings,
                                const struct mesh_ui_handshake_state *handshake,
                                const struct mesh_ui_setting_edit *edits, size_t edit_count,
                                enum mesh_ui_settings_section section, uint8_t channel,
                                struct mesh_ui_settings_item *out, uint32_t max);

#ifdef __cplusplus
}
#endif
