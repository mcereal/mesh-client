#pragma once

/*
 * The Settings tab as data: a list of sections, each a list of items with a label, a value
 * already formatted for display, and a kind. Backends draw the list; the nav walks it.
 *
 * An item that can be changed names its `field`. The nav keeps pending edits per field (in `struct
 * mesh_ui_nav`) and this module renders them in place of the radio's value, marked dirty, until the
 * app writes them. Everything about a field the nav needs to edit it blind - its kind, enum names,
 * number presets, text cap - is answered here so the nav never has to know what a field means.
 */

#include "inkcell/ui/icon.h"
#include "inkcell/ui/theme.h"

#include "inkstand/form/field.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store_handshake.h"
#include "mesh/ui/store_settings.h"

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
    /* Things the radio does rather than keeps: reboot, shutdown, the resets. On the Settings tab
       only while it is administering another node - the radio on the link has these on the
       Radio tab (RADIO_DETAILS below). Last because a cursor that overshoots the list should
       land on nothing worse than the row above it. */
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
    /*
     * NetworkConfig, read-only: which interface the radio is meant to be using, at what
     * address, against which NTP server.
     *
     * Fetched and stored on every refresh since phase 1 and read by nothing until now, which is
     * a round trip per refresh spent on bytes that were dropped. Not editable, for the reason
     * the roadmap's "Later, maybe never" gives - WiFi credentials typed on a handheld that has
     * no WiFi of its own is a poor fit - and that is an argument against *setting* it rather
     * than against showing it: "why is this radio not reaching the broker" is a question About
     * radio can only half answer, because DeviceConnectionStatus is the interface's state and
     * this is its configuration. The two disagreeing is itself worth being able to see.
     *
     * Declared last for the reason every section since phase 9 has been: the enum's order is
     * what the persisted cursor and the tests are written against, and the list's order is
     * mesh_ui_settings_root_at()'s business.
     */
    MESH_UI_SETTINGS_NETWORK,
    /*
     * MeshBeaconConfig: the seventeenth ModuleConfig variant and the last one with rows.
     *
     * The module the roadmap held back from phase 11 because it needed row models this client
     * did not have. Two of the three arrived before it and for other callers - the FLAG kind
     * (PositionConfig.position_flags) and the measured edit buffer - so what is left here is a
     * section with one repeated submessage in it, and that is the shape the four Target groups
     * are.
     *
     * Declared last for the reason every section since phase 9 has been: the enum's order is
     * what the persisted cursor and the tests are written against, and the list's order is
     * mesh_ui_settings_module_at()'s business.
     */
    MESH_UI_SETTINGS_BEACON,
    /*
     * The two pages the Radio tab opens from its cards, which are sections because they are
     * made of exactly the rows a section is made of - facts, verbs, headings, a confirm sheet in
     * front of the costly ones - and a second row model for the same rows would be a second
     * opinion about how a withdrawn reboot looks.
     *
     * RADIO_DETAILS is the Radio card's: what About radio says, and under it the things done *to*
     * the radio - power, backup, the factory resets. NODE_LISTS is the Mesh card's: the radio's
     * node database and this client's longer roster, the two lists "42 nodes" on that card is
     * counting. Neither is in mesh_ui_settings_root_at(): the Settings tab keeps what a radio
     * *keeps*, and the Radio tab what it *is* and what can be done to it.
     *
     * Both describe the radio on the end of the link, always. While the Settings tab is pointed
     * at somebody else's node they say so and offer the way back rather than acting on it - see
     * mesh_ui_settings_root_at() for where that node's own About and actions go instead.
     */
    MESH_UI_SETTINGS_RADIO_DETAILS,
    MESH_UI_SETTINGS_NODE_LISTS,
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
 *
 * The top level depends on one fact, which is why it takes the settings: whether the tab is
 * administering another node. About radio and Radio actions are listed only then. For the
 * radio on the link both live on the Radio tab, where the cards describing that radio are; a
 * remote node has no cards, so its facts and its reboot stay here under the banner that names
 * it - a Reboot on the Radio tab that took down somebody else's repeater would be a trap.
 * NULL answers for the radio on the link.
 */
uint32_t mesh_ui_settings_root_count(const struct mesh_ui_settings *settings);
enum mesh_ui_settings_section mesh_ui_settings_root_at(const struct mesh_ui_settings *settings,
                                                       uint32_t row);
uint32_t mesh_ui_settings_module_count(void);
enum mesh_ui_settings_section mesh_ui_settings_module_at(uint32_t row);
/* True for a section that lives under Modules rather than at the top level. */
bool mesh_ui_settings_section_is_module(enum mesh_ui_settings_section section);

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
    /*
     * The six that say what the radio's screen *is* and how it draws, rather than how long it
     * stays lit. `oled` overrides an autodetect that failed and `displaymode` picks the
     * layout, and those two are among the four DisplayConfig fields the firmware reboots for -
     * the other two being screen_on_secs and flip_screen, which have carried that note since
     * phase 2. The four toggles below them do not reboot anything.
     */
    MESH_UI_FIELD_DISPLAY_OLED,
    MESH_UI_FIELD_DISPLAY_MODE,
    MESH_UI_FIELD_DISPLAY_HEADING_BOLD,
    MESH_UI_FIELD_DISPLAY_WAKE_ON_MOTION,
    MESH_UI_FIELD_DISPLAY_LONG_NAMES,
    MESH_UI_FIELD_DISPLAY_MESSAGE_BUBBLES,
    MESH_UI_FIELD_POSITION_GPS_MODE,
    MESH_UI_FIELD_POSITION_BROADCAST_SECS,
    MESH_UI_FIELD_POSITION_SMART,
    MESH_UI_FIELD_POSITION_SMART_DISTANCE, /* number: metres */
    MESH_UI_FIELD_POSITION_SMART_INTERVAL,
    MESH_UI_FIELD_POSITION_GPS_INTERVAL,
    /*
     * PositionConfig.position_flags: what a position packet carries, as ten bits of one
     * uint32 rather than ten fields.
     *
     * Ten rows of kind INKSTAND_FORM_FLAG, each naming its own bit, because the wire being
     * one word is not a reason for the screen to be one row: "send the fix time" is a setting
     * a person has an opinion about and `0x0281` is not. The field table carries the mask
     * (see struct field_spec), so a bit upstream adds later is a row here rather than a
     * mechanism.
     *
     * All ten are listed whatever the others say, the rule the LoRa trio and the
     * smart-broadcast thresholds already follow: a row count that moves under the cursor
     * mid-edit moves the cursor. Two of them do nothing on their own - MSL refines the
     * altitude and the split refines the precision - and say so in their notes rather than by
     * disappearing.
     */
    MESH_UI_FIELD_POSITION_FLAG_ALTITUDE,
    MESH_UI_FIELD_POSITION_FLAG_ALTITUDE_MSL,
    MESH_UI_FIELD_POSITION_FLAG_GEOIDAL,
    MESH_UI_FIELD_POSITION_FLAG_DOP,
    MESH_UI_FIELD_POSITION_FLAG_HVDOP,
    MESH_UI_FIELD_POSITION_FLAG_SATINVIEW,
    MESH_UI_FIELD_POSITION_FLAG_SEQ_NO,
    MESH_UI_FIELD_POSITION_FLAG_TIMESTAMP,
    MESH_UI_FIELD_POSITION_FLAG_HEADING,
    MESH_UI_FIELD_POSITION_FLAG_SPEED,
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
    /*
     * Who holds the broker connection: the radio over its own WiFi, or whatever client it is
     * paired with. Directly under the toggle that turns MQTT on, because it is the second
     * question about the same connection and everything below it is a detail of the first.
     *
     * It was a read-only row until this client could act on it, and the reason was that turning
     * it on takes the radio's MQTT off the air and hands it to something that was ignoring the
     * messages. That is no longer true; what is still true is that this client has to be running
     * and connected, which is what the Status screen's Broker card is for.
     */
    MESH_UI_FIELD_MQTT_PROXY,
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
    /* ChannelSettings.module_settings.is_muted, the other field in the submessage the row above
       writes. Last in the channel's rows because it is the one that changes nothing about what
       the channel *is* - the four before it decide who can read it and where it is bridged. */
    MESH_UI_FIELD_CHANNEL_MUTED,
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
    /*
     * The advanced group: the five rows that change what the radio does with the band rather
     * than which band it is on.
     *
     * Held back from the rest of LoRa until the ham rows below them existed, because four of
     * them are the same feature as `set_ham_mode` - a client that offers an out-of-band
     * frequency without offering the mode that licenses it is offering half of something.
     *
     * Three are TEXT rather than NUMBER for the reason a coordinate is: a frequency slot has
     * a hundred equally likely values and a frequency has more than that, so a preset list is
     * either wrong or a hundred presses long. They are decimals, parsed by
     * mesh_ui_settings_decimal_parse() at the places their field wants.
     */
    MESH_UI_FIELD_LORA_BOOST_GAIN,
    MESH_UI_FIELD_LORA_OVERRIDE_DUTY,
    MESH_UI_FIELD_LORA_CHANNEL_NUM,    /* text: a slot number, 0 = worked out from the region */
    MESH_UI_FIELD_LORA_OVERRIDE_FREQ,  /* text: MHz, 0 = use the slot above */
    MESH_UI_FIELD_LORA_FREQUENCY_TRIM, /* text: Hz, a crystal's error either way */
    /*
     * LoRaConfig.ignore_incoming: up to three node numbers whose packets this radio drops as
     * though they were out of range.
     *
     * Three rows compacted on save, which is the shape the three admin keys already have. It
     * reads like the Nodes tab's Ignore row and is a different mechanism: that one is a NodeDB
     * flag set with `set_ignored_node`, this one is the LoRa layer refusing the packet, and a
     * row that did not say which would be two settings wearing one name.
     */
    MESH_UI_FIELD_LORA_IGNORE_NODE_0,
    MESH_UI_FIELD_LORA_IGNORE_NODE_1,
    MESH_UI_FIELD_LORA_IGNORE_NODE_2,
    /*
     * Ham mode: a call sign, a frequency and a power, read by the row under them rather than
     * by Y.
     *
     * The fixed-position shape exactly (MESH_UI_SETTING_CONSUMER_HAM_MODE), because it is the
     * same situation: `set_ham_mode` is its own admin verb, it writes three things this tab
     * keeps in three different sections - the owner's names, the primary channel's key and
     * LoRa's own frequency and power - and there is no set_config that would do it.
     *
     * There is no row for a short name. The firmware wants one and the radio already has one,
     * so the write carries the owner's own rather than asking a second time for something the
     * User section has always been where you change.
     */
    MESH_UI_FIELD_LORA_HAM_CALL_SIGN,   /* text: 7 bytes, what the firmware takes for a long name */
    MESH_UI_FIELD_LORA_HAM_FREQUENCY,   /* text: MHz, and the whole point of the mode */
    MESH_UI_FIELD_LORA_HAM_TX_POWER,    /* number: dBm, the same presets the LoRa row above uses */
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
    /*
     * Mesh beacon. The three flags are one word, the way Position's ten are; the offered
     * channel is a ChannelSettings embedded in the module, so its two rows are a name and a
     * key exactly as the Channels section's are.
     */
    MESH_UI_FIELD_BEACON_LISTEN,
    MESH_UI_FIELD_BEACON_BROADCAST,
    MESH_UI_FIELD_BEACON_LEGACY_SPLIT,
    MESH_UI_FIELD_BEACON_INTERVAL,
    MESH_UI_FIELD_BEACON_MESSAGE,
    MESH_UI_FIELD_BEACON_OFFER_NAME,
    MESH_UI_FIELD_BEACON_OFFER_KEY,
    MESH_UI_FIELD_BEACON_OFFER_REGION,
    MESH_UI_FIELD_BEACON_OFFER_PRESET,
    /*
     * The four broadcast targets, three rows each and in that order: the run is walked as
     * MESH_UI_FIELD_GROUP_BEACON_TARGETS, so the row builder emits four copies of one shape and
     * the write builder divides by three rather than either of them naming twelve fields.
     *
     * Contiguous is load-bearing here in a way it is not for the canned slots: the arithmetic
     * both ends do is (field - first) / 3 and % 3, and a field inserted in the middle would
     * move every target after it silently. The test that walks the group holds that.
     */
    MESH_UI_FIELD_BEACON_TARGET_0_PRESET,
    MESH_UI_FIELD_BEACON_TARGET_0_REGION,
    MESH_UI_FIELD_BEACON_TARGET_0_CHANNEL,
    MESH_UI_FIELD_BEACON_TARGET_1_PRESET,
    MESH_UI_FIELD_BEACON_TARGET_1_REGION,
    MESH_UI_FIELD_BEACON_TARGET_1_CHANNEL,
    MESH_UI_FIELD_BEACON_TARGET_2_PRESET,
    MESH_UI_FIELD_BEACON_TARGET_2_REGION,
    MESH_UI_FIELD_BEACON_TARGET_2_CHANNEL,
    MESH_UI_FIELD_BEACON_TARGET_3_PRESET,
    MESH_UI_FIELD_BEACON_TARGET_3_REGION,
    MESH_UI_FIELD_BEACON_TARGET_3_CHANNEL,
    MESH_UI_FIELD_COUNT,
};

/* What an ACTION row does when A is pressed. Rows of kind INKSTAND_FORM_ACTION carry one in
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
     * LoRa section: hand the radio a call sign and put it on an amateur band.
     *
     * A row that reads the three rows above it, exactly as "Set fixed position" does, because
     * `set_ham_mode` is one verb over three things this tab keeps apart - the owner's names,
     * the primary channel's key and LoRa's frequency and power. Behind the confirm overlay,
     * which the fixed-position pair are not: this one turns the primary channel's encryption
     * off, which is what makes the mode legal and is not undone by pressing the row again.
     *
     * There is no verb for leaving. The firmware has none, and the way back is the two rows
     * that made it - User's `Licensed operator` off and `Override frequency` back to 0 - so
     * the note says that rather than this offering a press that would have to invent it.
     */
    MESH_UI_SETTINGS_ACTION_SET_HAM_MODE,
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
     * A client action rather than a radio one - it reads two documents over HTTPS and touches the
     * radio not at all - and no confirm overlay, because nothing it does can be regretted. The
     * press that *installs* firmware is a different row that does not exist yet, and it will need
     * the sheet this one does not.
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
    /*
     * Channel sharing, at the foot of the Channels list: this radio's set as a link, and a
     * link typed in.
     *
     * Neither is a radio action. Sharing touches nothing at all - it opens a screen with a QR
     * code on it - and importing does not reach the radio on this press either: it opens the
     * keyboard, and what the radio hears about is whatever comes back from the sheet in front
     * of *that*. So they are rows the nav answers itself rather than verbs the app is handed,
     * which is why neither appears in mesh_ui_settings_action_is_radio().
     */
    MESH_UI_SETTINGS_ACTION_SHARE_CHANNELS,
    MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS,
    /*
     * At the foot of one channel's rows: empty the slot, rather than only stop listening to it.
     *
     * The table is eight fixed slots and the wire has no verb for removing one, so "delete a
     * channel" is a write like any other - and setting Role to Disabled, which is the obvious
     * way to reach it, leaves the name and the key sitting in the slot. That is the right
     * default for a channel being turned off for the afternoon and the wrong one for a key
     * being got rid of, and a role row that silently wiped a key to spare the difference would
     * be a worse surprise than either.
     *
     * So this is the second press rather than a change to the first: role, name, key and both
     * module settings go at once, and the sheet says what is lost. Not a radio action - what
     * goes out is a SET_CHANNEL, so it travels as a save of that slot with this verb in
     * `number` (see mesh_app_build_settings_write) and collects the same toast, the same ack
     * tracking and the same read-back a save does.
     */
    MESH_UI_SETTINGS_ACTION_CLEAR_CHANNEL,
    /*
     * Contact sharing, at the foot of the User list: this radio's own identity as a link, and
     * somebody else's typed in.
     *
     * The channel pair one directory over, for one node instead of a mesh, and neither is a
     * radio action for the same reasons: showing a code touches nothing at all, and adding one
     * does not reach the radio on this press either - it opens the keyboard, and what the radio
     * hears about is whatever comes back from the sheet in front of *that*. So neither appears
     * in mesh_ui_settings_action_is_radio().
     */
    MESH_UI_SETTINGS_ACTION_SHARE_CONTACT,
    MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT,
    /*
     * About radio: stop configuring somebody else's radio and come back to this one.
     *
     * The only row in this tab that is about *which* radio the rest of the tab describes, and
     * the return half of a press made on the Nodes tab - a node's own card is where remote
     * administration is entered, because that is where a node is a subject. It lives here
     * because this section is the one that says what the radio being configured is, so it is
     * the section somebody who has noticed the banner is already looking at.
     *
     * Not a radio action: nothing goes over the air, nothing on any radio changes, and it works
     * exactly as well when the remote node has stopped answering - which is one of the two
     * times it is most wanted. Drawn only while there is something to come back from.
     */
    MESH_UI_SETTINGS_ACTION_ADMIN_LOCAL,
    /* Not an action: what the two verb tables below are sized by, so a row added above without
       a symbol or a weight is a hole in an array rather than a row that quietly draws nothing.
       Last, so no existing value moves - nav->confirm_action carries one in a uint8_t. */
    MESH_UI_SETTINGS_ACTION_COUNT,
};

/*
 * What a verb *is*, for the leading slot, and what it *costs*, for the ink - one table each,
 * in the enum's own order.
 *
 * Here rather than in the renderer for the reason k_section_icons[] is here and the reason
 * node_detail.c grew the same pair: a row states once what it means, and the disc, the words
 * and the marker bar are three renderings of that one statement. A backend deciding which of
 * its verbs is the dangerous one is a backend holding an opinion the CLI backend cannot share.
 *
 * The weight is a tone rather than a family because it is the *row's* tone:
 * INKCELL_FB_LEADING_TONAL reads the family back out of it, the accent edge takes the same answer,
 * and so does the label. Three weights, and the first of them is the reason a card of verbs is
 * readable at all:
 *
 *   NORMAL   the ordinary verb. The words stay in the body ink and the disc takes the primary,
 *            which is what INKCELL_FB_LEADING_TONAL does with a tone that names no family. A verb
 * drawn in the accent is a verb shouting, and a card where all of them shout is a card where none
 * of them does - which is the state the node detail was in before its own table, and the state
 * Radio actions would be in with eleven coloured rows. WARNING  the radio goes away for a while, or
 * something takes time to come back. A reboot is the shape of it: nothing is lost, and you wait.
 *   ERROR    the floor drops out. Spent sparingly and on purpose: in a section that is nothing
 *            but things done *to* a radio, "this costs something" is the baseline rather than
 *            the exception, so the red marks where there is no way back rather than every row a
 *            confirm sheet stands in front of.
 */
enum inkcell_icon mesh_ui_settings_action_icon(enum mesh_ui_settings_action action);
enum inkcell_tone mesh_ui_settings_action_tone(enum mesh_ui_settings_action action);

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
/*
 * True for a verb whose press raises something - a confirm sheet, a screen, the keyboard -
 * rather than acting where it stands.
 *
 * What the chevron means, and the reason it is a question rather than a reading of the row: a
 * verb used to earn one by having no value, which is true of every row that opens something and
 * also of several that do not. "Check for firmware" sends a request and redraws when the answer
 * lands; Language and Theme cycle to the next one; the fixed-position pair goes straight to the
 * radio. Each of them promised a screen that was never coming, which is the action bar's own
 * rule - a keycap that does nothing is a bug - read one column further to the right.
 *
 * Kept beside the tone and the icon rather than derived in a backend, because the answer is the
 * nav's: these are exactly the presses mesh_ui_nav_handle_key() consumes by raising a sheet, a
 * screen or the keyboard instead of filling in an action. Two readers would be two opinions
 * about what a chevron promises, and the one that is wrong is the one the user acts on.
 */
bool mesh_ui_settings_action_opens(enum mesh_ui_settings_action action);
/*
 * True for the presses that step the row's own value rather than doing anything: the language,
 * the theme, this client's update channel and its dev-updates switch, and the radio's firmware
 * channel.
 *
 * A table rather than a reading of the row, for the reason the icons and the tones are one.
 * "Has a value in its value column" would catch the forget rows, whose figure is the size of
 * what the press costs; "opens nothing" would catch the two checks, which send a request and
 * redraw when the answer lands. Neither is the question. The question is whether pressing A
 * leaves the reader on the same row with a different setting on it, and only these five do.
 *
 * See struct mesh_ui_settings_item::cycle for what the answer is spent on.
 */
bool mesh_ui_settings_action_is_cycle(enum mesh_ui_settings_action action);
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
/* Telemetry is fifteen fields plus five headings; External notification will be worse, and the
   Radio tab's details page - About radio with the power, backup and factory verbs under it -
   is 40 rows when a radio reports every interface it has
   (ui_settings_radio_details_fits_at_its_longest). The list is built onto the stack every frame,
   so this is ~7 KB in a loop that has no threads to share it with. */
#define MESH_UI_SETTINGS_ITEMS_MAX 48U

struct mesh_ui_settings_item {
    char label[MESH_UI_SETTINGS_LABEL_MAX];
    char value[MESH_UI_SETTINGS_VALUE_MAX];
    enum inkstand_form_kind kind;
    enum mesh_ui_setting_field field;    /* NONE: read-only */
    bool dirty;                          /* value shown is a pending edit */
    uint32_t number;                     /* toggle 0/1, enum index, raw number, or key choice */
    char text[MESH_UI_SETTING_TEXT_MAX]; /* TEXT: the raw string; KEY: the key as hex */
    /*
     * Which values this row will take *right now*, one bit per value - and 0 meaning every
     * value the kind allows, which is what all but a handful of rows say.
     *
     * The kinds whose values are a **set** rather than a range share this, and that is the
     * point of it being on the item rather than on either kind. A KEY row's set is fixed by the
     * field table (keep / default / a new random key / none). An ENUM row's may be decided by
     * another row: the modem presets a LoRa region will take are the firmware's own table, so
     * the set moves as the region above it is edited, and the row it is read off is built after
     * that edit rather than before it.
     *
     * Left and Right step *within* the set - mesh_ui_settings_choice_step() is the one loop
     * both kinds walk. What is deliberately not done is hiding a value outside it: the radio's
     * own setting may be one, and a row that cannot show what the node is set to is worse than
     * a row showing something it should not be. Such a row says so instead - see `conflict`.
     */
    uint32_t choices;
    /*
     * The row is showing a value that disagrees with another row's, and the radio will not
     * honour the pair.
     *
     * Not the same thing as an invalid entry, and it is not this client's judgement either:
     * both callers are the firmware's own table saying so. A modem preset that is not legal in
     * the selected region is one; a region the firmware marks for licensed operators, on a node
     * whose owner record does not claim a licence, is the other.
     *
     * It never blocks a save. The row is a *warning*, drawn in the marker gutter and the row's
     * tone, because the pairing can be arrived at honestly - a radio configured somewhere else
     * arrives holding one - and a screen that refused to save would leave no way to correct the
     * half the user did not come to change.
     */
    bool conflict;
    /*
     * What this row is *about*, for the leading slot: the cloud on MQTT, the shield on
     * Security. INKCELL_ICON_NONE on a row that is a setting rather than a subject, which is
     * every row of every section except the one that lists the modules.
     *
     * A section gives every row an icon or gives none, and that is a rule rather than an
     * observation: a leading slot is reserved for a whole list, so a list whose rows disagreed
     * would start its words in two different columns. mesh_ui_settings_section_icons_rows()
     * answers it for a caller, and a test holds every section to it.
     *
     * **The disc marks a press that acts, and nothing else.** That is the rule the leading slot
     * answers to now, and `cycle` below is what it cost to state: a row that merely holds a
     * value is a setting whatever key steps it, so Language, Theme and the two update channels
     * carry no symbol and stand in the field column with their neighbours. Before that the slot
     * was spent on any row the nav would answer - which put a disc on two of About's four rows
     * and on two of About radio's fourteen, and left both screens with an icon column that
     * started and stopped down the page.
     *
     * **A heading never carries one**, which is the same rule one row over. A symbol on a
     * heading is a card *header*, out at the card's own edge where its rows begin two cells
     * further in, and it was optional per heading - so the tab divided into the eight groups
     * whose subject happened to own a rune and the thirty that had to say nothing, five of the
     * eight in Radio actions. A settings section is a list of fields and a list has one kind of
     * subheader; the node detail and Status are card screens and keep theirs.
     */
    enum inkcell_icon icon;
    /*
     * What this row *costs*, for an ACTION or an ACTION_OFF - mesh_ui_settings_action_tone()'s
     * answer, carried on the row so a backend reads one thing.
     *
     * Zero is INKCELL_TONE_NORMAL, which is what every row that is not a verb says and what the
     * settings rows have always drawn in. A renderer still layers the two marks that are about
     * the *value* over it - a conflict is a warning and an unsaved edit is strong, and both of
     * those outrank what the row would otherwise have been - because those describe the state of
     * this row now and this describes what the row is for.
     */
    enum inkcell_tone tone;
    /*
     * This row is a verb: something happens when A is pressed, as opposed to a list opening.
     *
     * Set by the action builders and by nothing else, which is what makes it a fact about how
     * the row was made rather than a second opinion about its kind. See
     * mesh_ui_settings_item_is_verb() just below for why it is not the kind itself.
     */
    bool verb;
    /*
     * A is what steps this row's own value: it is a setting wearing INKSTAND_FORM_ACTION's
     * clothes, not a verb.
     *
     * Five rows say it - Language, Theme, the client's update channel, the dev-updates switch
     * and the radio's firmware channel - and what they have in common is the thing the reader
     * sees: the value column holds the setting itself, and the press moves it to the next one.
     * A forget row's "21 nodes" is the opposite case and stays a verb, because that figure is
     * the size of what the press *costs* rather than what the row is set to.
     *
     * Why the distinction is on the row rather than left to a renderer: three things read it
     * and all three would otherwise be guessing from the kind. The leading slot is a disc for a
     * verb and empty here. The card split floats a group's verbs onto the panel and keeps its
     * settings on the card, and these belong with the settings - which is what stopped About
     * radio alternating card, bare row, card down the whole screen. And the marker gutter says
     * how a row is changed, so where a field takes the pencil this takes the swap rune: without
     * it, dropping the disc would leave a pressable row looking exactly like a fact.
     *
     * Set by item_action_named() off mesh_ui_settings_action_is_cycle(), so the five are named
     * once, in the table beside the icons and the tones.
     */
    bool cycle;
};

/*
 * Whether this row is a *verb* - a thing being done to the radio or to this client - rather than
 * a row that merely opens a list.
 *
 * Reads `verb` and exists so that one question has one asker: a backend needs the answer to
 * decide whether a row gets a tonal disc and drops its value column, and two backends working
 * it out for themselves is how they come to disagree.
 *
 * It is a stored flag rather than a kind because INKSTAND_FORM_ACTION is doing two jobs, and
 * separating them is a change to the nav rather than to the drawing. A channel row and a module
 * row are ACTION too - the nav answers all three with A, which is what the kind is for there -
 * and mesh_ui_settings_channel_at_row() tells a slot from a share row by reading `number`
 * against the radio's table. Worth untangling one day; not on the way past.
 */
bool mesh_ui_settings_item_is_verb(const struct mesh_ui_settings_item *item);

/*
 * Whether this row is a *stated fact* - something read off the radio or off this client, with
 * no press and no edit that changes it.
 *
 * The question a renderer asks to decide which of a row's two tiers recedes. On a fact the
 * label is the question ("Firmware", "Node number") and repeats down a column the reader is
 * scanning for the *answers*, so the label goes quiet and the value keeps the row's ink; on a
 * control the label is what the reader is choosing and the value is merely where it stands, so
 * the label leads. That is the split the node detail has drawn since it grew cards, and it is
 * asked here so that the settings sections - which are two thirds facts, and drew every one of
 * them at full strength - answer it the same way rather than by screen.
 *
 * "No press and no edit" is the whole of it: not a verb, not a cycle, no field behind it, and
 * not one of the ACTION rows that open a list (a channel slot, a module). A heading is not a
 * row of this kind at all - it names the card rather than standing on it.
 */
bool mesh_ui_settings_item_is_fact(const struct mesh_ui_settings_item *item);

/*
 * What this row's marker gutter says: a state the reader has to know, or how the row is changed.
 *
 * Two questions in one answer, and the order between them is the point rather than an accident
 * of the chain. A conflict and an unsaved edit are about the *value* - the radio will not honour
 * this, this is not written yet - and both outrank anything about the offer, because a gutter
 * holds one mark and a reader who is about to lose a change needs telling before they are told
 * how to make another one.
 *
 * Under those, how the row is changed, which is a fact about the *kind*:
 *
 *   TEXT, KEY          the pencil. A press opens the keyboard and the value is typed.
 *   ENUM, NUMBER       the stepper. Left and Right move along a set, where the row stands.
 *   TOGGLE, FLAG       nothing, and this is the whole reason the question moved here. A switch
 *                      and a checkbox *are* the offer: they read as controls, they are aimable,
 *                      and a mark beside one is a caption on something already legible. Eighty-
 *                      odd rows of this tab are one of the two, and every one of them used to
 *                      carry a pencil promising a keyboard that does not open.
 *   anything else      nothing. A fact, a heading, a meter, a verb - none is changed in place,
 *                      and the verbs carry a tonal disc and a chevron of their own.
 *
 * `cycle` sits with the kinds and answers before them: the swap rune, because A moves that row's
 * value where the d-pad moves a field's, and the gutter is where that difference is stated.
 *
 * Asked of the model rather than worked out by each backend, on the same terms as the two
 * predicates above: the CLI backend draws words and no marker at all, so if this lived in the fb
 * renderer it would be the only copy - and the next screen with a field row (the Nodes list has
 * two) would be free to answer it differently, which is exactly how that screen came to have a
 * strip of controls wearing no mark at all.
 *
 * It says nothing about whether a *control* is drawn beside the value, which is the backend's
 * own choice and depends on what will fit: a small enum becomes a segmented button and a number
 * on a scale becomes a slider, and both of those swallow the stepper for the same reason a
 * switch does. See the seam in fb_screens_settings.c.
 */
enum inkcell_icon mesh_ui_settings_item_marker(const struct mesh_ui_settings_item *item);

/*
 * How many groups a built section actually has: maximal runs of non-heading rows, counting only
 * the runs that hold at least one row. The rows ahead of the first heading are a group too - an
 * unnamed one - exactly as the block at the top of a phone's settings page is a card before any
 * label appears.
 *
 * One predicate because three things were asking this question and answering it differently, and
 * a reader can tell: the renderer decides whether to draw cards, the navigation decides whether
 * L2/R2 may cross one, and the help screen decides whether to say the pair exists. A section
 * with one group drew no cards while the help still promised the jump and R2 did nothing - which
 * is the help advertising a key that does nothing, the thing the action bar keeps a single table
 * to avoid.
 *
 * Groups rather than cards, and the two are the same number rather than merely close: a group is
 * one card whatever is in it, verbs included, so every group holding a row leaves exactly one
 * card behind. Counting groups is therefore what the renderer was already counting, and it is a
 * question the model can answer without knowing a card exists.
 */
uint32_t mesh_ui_settings_section_groups(const struct mesh_ui_settings_item *items, uint32_t count);

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
   locale in force. INKCELL_STR_NONE for a section past the end. */
inkcell_str_id mesh_ui_settings_section_label(enum mesh_ui_settings_section section);

/*
 * What a section is about, as an icon: the leading slot on a row that *opens* that section.
 *
 * Beside the name because it is the same kind of fact - what this section is - answered for the
 * same two lists: the settings root, and Modules, which is a list of sections wearing a
 * section's clothes. A backend with no icons (the CLI) ignores it exactly as it ignores the
 * chevron.
 *
 * Three sections answer with an icon another part of the UI already owns, because they are
 * saying the same thing it says: "About radio" with INKCELL_ICON_RADIO, Bluetooth and Channels
 * with their own runes.
 */
enum inkcell_icon mesh_ui_settings_section_icon(enum mesh_ui_settings_section section);

/*
 * What a section is *for*, as a catalog id: the paragraph the help screen opens with.
 *
 * An id rather than a `const char *` so a caller can ask whether there is anything to say
 * without a strlen, and so the fb backend, the CLI and the tests cannot each invent their own
 * idea of what an absent note looks like. Never INKCELL_STR_NONE for a real section - see the
 * table in settings.c for why that is a rule rather than an observation.
 */
inkcell_str_id mesh_ui_settings_section_note(enum mesh_ui_settings_section section);

/* Whether this section's *items* carry a leading icon - true only of Modules, whose rows are
   sections. What lets a renderer declare the slot once for the list instead of testing a row. */
bool mesh_ui_settings_section_icons_rows(enum mesh_ui_settings_section section);

/*
 * Position precision as a distance rather than a bit count: 0 is off, 32 or more is precise,
 * and 10..19 are the ten steps the phone apps label ("~23 km" down to "~45 m", or "~14 mi" down
 * to "~150 ft" when `imperial`). Public because the node detail asks the same question of a
 * *received* fix that the channel's position_precision row asks of an outgoing one - and a
 * rounded location described two different ways on two screens is how a client comes to disagree
 * with itself about how much it knows. `imperial` is mesh_ui_units_imperial() of the radio's
 * display units, so a footprint reads in the system every other length on the screen does.
 * Writes at most `out_len` bytes including the NUL.
 */
void mesh_ui_settings_format_precision(uint32_t bits, bool imperial, char *out, size_t out_len);

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
inkcell_str_id mesh_ui_settings_field_label_id(enum mesh_ui_setting_field field);
enum inkstand_form_kind mesh_ui_settings_field_kind(enum mesh_ui_setting_field field);
enum mesh_ui_settings_section mesh_ui_settings_field_section(enum mesh_ui_setting_field field);
/*
 * What one setting does, as a catalog id, or INKCELL_STR_NONE for a row whose label is already the
 * whole explanation - which is most of them, on purpose. A note is for the row where knowing the
 * name does not tell you what happens if you get it wrong. See docs/help.md.
 */
inkcell_str_id mesh_ui_settings_field_note(enum mesh_ui_setting_field field);
/*
 * FLAG fields: which bit of its group's word this row is, and 0 for every other kind.
 *
 * The one thing a caller outside this module needs in order to read or write a flag - the row
 * builder ANDs with it, the write builder sets or clears it - and it is answered here rather
 * than written out at either end, because a mask stated twice is a mask that will one day
 * disagree with itself.
 */
uint32_t mesh_ui_settings_field_bit(enum mesh_ui_setting_field field);

/*
 * The run of FLAG fields a group is made of: the first row and how many follow it.
 *
 * Groups are contiguous in the field enum, the same way the canned slots and the three admin
 * keys are, so a caller walks `first`..`first + count` and asks each one for its bit. A test
 * holds the run to exactly the fields the group's word has bits for, which is what keeps a
 * field added in the middle of one from being silently left out of its own group.
 */
enum mesh_ui_setting_field_group {
    /* PositionConfig.position_flags: what a position packet carries. */
    MESH_UI_FIELD_GROUP_POSITION_FLAGS = 0,
    /* MeshBeaconConfig.flags: listen, broadcast, and the legacy split. */
    MESH_UI_FIELD_GROUP_BEACON_FLAGS,
    /*
     * MeshBeaconConfig.broadcast_targets, which is not a run of flags at all.
     *
     * A group is "a contiguous run of fields that repeat one shape", and a set of bits in one
     * word was only the first thing that answered to it. The four targets are twelve fields in
     * four copies of preset/region/channel, and walking them through the same two accessors is
     * what keeps the row builder and the write builder from each writing the run out.
     */
    MESH_UI_FIELD_GROUP_BEACON_TARGETS,
    MESH_UI_FIELD_GROUP_COUNT,
};

/* How many rows one MESH_UI_FIELD_GROUP_BEACON_TARGETS record is: preset, region, channel. */
#define MESH_UI_BEACON_TARGET_FIELDS 3U
uint32_t mesh_ui_settings_group_count(enum mesh_ui_setting_field_group group);
enum mesh_ui_setting_field mesh_ui_settings_group_field(enum mesh_ui_setting_field_group group,
                                                        uint32_t index);

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
/*
 * Adds the sentence naming *which* radio, to a body the call above has already filled in.
 *
 * The confirm sheet is the one place the remote-administration banner cannot reach: a modal owns
 * the body, so the container that has been saying "this is somebody else's radio" on every other
 * frame is gone at exactly the moment the question is put. This puts it back, on the sheet
 * itself.
 *
 * Added rather than substituted because the question has not changed - "reboot the radio" is
 * still what is being asked - and because the two tables above are keyed on the verb and have no
 * node to name. A no-op with no remote target, and a no-op for the presses that stay home
 * whatever the target is: the two that drop this client's own roster, and the two that install
 * firmware over a bus rather than over the mesh.
 */
void mesh_ui_settings_confirm_add_subject(const struct mesh_ui_settings *settings,
                                          enum mesh_ui_settings_action action, char *text,
                                          size_t text_len);

/* The verb on the overlay's first row ("Save to radio", "Reboot now", ...). */
const char *mesh_ui_settings_confirm_accept(enum mesh_ui_settings_action action);

/* KEY fields: which choices Left/Right offer (a bitmask of MESH_UI_PSK_CHOICE_BIT), and
   whether a key of `len` bytes is acceptable for the field. */
uint32_t mesh_ui_settings_key_choices(enum mesh_ui_setting_field field);
bool mesh_ui_settings_key_len_ok(enum mesh_ui_setting_field field, size_t len);

/*
 * The two halves of a row whose values are a set: is this one in it, and what is the next one.
 *
 * `choices` is a bitmask over 0..count-1 and 0 means unconstrained, so a caller with nothing to
 * say passes 0 and gets the plain wrap-around it had before the set existed. Values from
 * `count` up are never in the set, whatever the mask says, which is what keeps a stale mask
 * from offering a value the field no longer has.
 *
 * step() walks in `delta`'s direction until it finds a value in the set, and answers `current`
 * when there is no other - a row with one legal value is a row Left and Right do nothing to,
 * which is the truth rather than a press that silently lands where it started.
 *
 * One loop for both kinds that have a set: the modem presets a region will take, and the
 * choices a KEY row offers. Those were two copies of one modulo walk written out separately,
 * and only the KEY copy knew what a set was - which is why the enums could offer a value the
 * rest of their section did not allow, and why making them able to say so is a deletion here
 * rather than an addition.
 */
bool mesh_ui_settings_choice_allowed(uint32_t choices, uint32_t count, uint32_t value);
uint32_t mesh_ui_settings_choice_step(uint32_t choices, uint32_t count, uint32_t current,
                                      int delta);

/*
 * What the firmware says one LoRa region will take, or NULL when it has said nothing about it.
 *
 * NULL is the answer for a radio whose firmware predates FromRadio.region_presets, for a region
 * that firmware left out of the map, and for a region code past the end of the table - three
 * different silences that all mean "constrain nothing", which is what the proto asks for. A
 * caller that treated NULL as "no preset is legal" would leave the row unsteppable on exactly
 * the radios that tell us least.
 */
const struct mesh_ui_region_preset *
mesh_ui_settings_region_preset(const struct mesh_ui_settings *settings, uint32_t region);

/* Keys as text. key_text() is base64, what the Meshtastic apps show and accept, so a key
   read off the Brick can be typed into a phone and vice versa. parse() takes base64 or hex
   (an even number of hex digits); an empty string is an empty key. */
/*
 * A decimal a person types, held as an integer scaled by a fixed number of places.
 *
 * The one row model behind every TEXT field that is really a number with a fraction: a
 * coordinate at seven places, a frequency in megahertz at four, an offset in hertz at one. It
 * is digits rather than a double because the wire wants an exact number of decimal places and
 * a double rounds the last of them somewhere nobody can see it happen - which is the same
 * reason a key is parsed byte by byte two declarations down.
 *
 * text() may show fewer places than are held (a coordinate is shown to five), rounding rather
 * than cutting. parse() rejects a whole part past `limit_whole`, anything with trailing
 * rubbish, and a fraction finer than the field can hold - there is no half-understood reading
 * of "44.6N", and a slot row taking "12.5" as 12 is the same mistake made quietly.
 */
void mesh_ui_settings_decimal_text(int64_t scaled, uint32_t held_digits, uint32_t shown_digits,
                                   char *out, size_t out_len);
bool mesh_ui_settings_decimal_parse(const char *text, uint32_t digits, int64_t limit_whole,
                                    int64_t *out_scaled);

/* The decimal places each kind of row is held to. Named here rather than written at every
   call, because the format and the parse have to agree and they are in different files. */
#define MESH_UI_COORD_DIGITS 7U     /* Meshtastic's fixed-point 1e-7 degrees */
#define MESH_UI_FREQUENCY_DIGITS 4U /* megahertz to 100 Hz, which is finer than any band plan */
#define MESH_UI_HERTZ_DIGITS 1U     /* a crystal offset, in hertz */

/* Coordinates as decimal degrees, to and from Meshtastic's fixed-point 1e-7 form - the decimal
   pair above at MESH_UI_COORD_DIGITS, shown to five places. An empty string is not a
   coordinate. */
void mesh_ui_settings_coord_text(int32_t value_i, char *out, size_t out_len);
bool mesh_ui_settings_coord_parse(const char *text, int32_t limit_degrees, int32_t *out_i);

/* A node number as "!433d1b2c", which is what the apps show and the logs print. parse() also
   takes "0x..." and a bare hex string, and reads an empty string as 0 - the row is empty, not
   wrong, and a caller reads 0 as an unused slot rather than as an address.

   Unmarked and all digits is **decimal**: "12345678" is a legal node number read either way,
   and the hex reading always has "!" or "0x" available to ask for it, so the ambiguous
   spelling goes to the one a reader can predict. */
void mesh_ui_settings_node_id_text(uint32_t node_id, char *out, size_t out_len);
bool mesh_ui_settings_node_id_parse(const char *text, uint32_t *out_id);

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

/*
 * Why a section has no rows - and, the part that matters to whoever is looking at it, whether
 * pressing X could change that.
 *
 * Three lists drew the same "not loaded" from three different tests before this existed: the
 * top level, the Modules list, and the empty-section screen. Two of them were also wrong for
 * the same radio, because a firmware built without a module never sends it and never will, and
 * a row that says "not loaded" is an invitation to keep refreshing. One predicate, asked in
 * three places, with the words chosen from the answer rather than from the caller.
 */
enum mesh_ui_settings_availability {
    MESH_UI_SETTINGS_SECTION_READY = 0, /* the radio sent it; there are rows */
    MESH_UI_SETTINGS_SECTION_WAITING,   /* not sent yet - a refresh may bring it */
    MESH_UI_SETTINGS_SECTION_EXCLUDED,  /* this firmware was built without it */
};

enum mesh_ui_settings_availability
mesh_ui_settings_section_availability(const struct mesh_ui_settings *settings,
                                      const struct mesh_ui_handshake_state *handshake,
                                      enum mesh_ui_settings_section section);

/* The word a list row puts in its value column, and INKCELL_STR_NONE for a section that is ready
   (which draws as an empty column rather than as a word for "fine"). */
inkcell_str_id mesh_ui_settings_availability_label(enum mesh_ui_settings_availability state);

/* Which bit of DeviceMetadata.excluded_modules stands for this section, and 0 for a section
   the mask has nothing to say about. Exported for the test that pins the table against the
   protobuf; nothing else should be comparing bits. */
uint32_t mesh_ui_settings_section_excluded_bit(enum mesh_ui_settings_section section);

/* The line an empty section screen draws instead of its rows. A ready section answers with the
   waiting line rather than with nothing: the only way to reach this while ready is a channel
   slot that went away under an open screen, and "not sent by the radio yet" is what that is. */
inkcell_str_id mesh_ui_settings_availability_reason(enum mesh_ui_settings_availability state);

/*
 * Whether anything in this section can be stepped in place, and whether anything in it is a
 * verb - which is the pair the action bar's two universal keycaps promise.
 *
 * The first is a fact about the field table and is answered from it; the second depends on the
 * radio's own data (About radio grows its install press only when there is one), so it is
 * answered from the rows as built. Both exist so the bar can stop naming sections: a read-only
 * section that offered "Left/Right edit" was advertising a press that worked on none of its
 * rows, which is the keycap-that-does-nothing this client refuses everywhere else.
 */
bool mesh_ui_settings_section_has_fields(enum mesh_ui_settings_section section);
bool mesh_ui_settings_section_has_verbs(const struct mesh_ui_settings *settings,
                                        const struct mesh_ui_handshake_state *handshake,
                                        enum mesh_ui_settings_section section, uint8_t channel);

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
