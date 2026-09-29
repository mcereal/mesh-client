#define _POSIX_C_SOURCE 200809L

/*
 * The Backups section's levels and the words for a comparison. See include/mesh/ui/backups.h.
 */

#include "mesh/ui/backups.h"

#include "inkcell/ui/fb_draw.h"
#include "inkwell/base/text.h"
#include "mesh/core/meshcore_backup.h"
#include "mesh/ui/chrome.h"
#include "mesh/ui/settings.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The byte, split three ways: 0..63 a radio, 64..127 a backup, 128..191 a comparison. The
   radio list is 0xFF, which is past all three. */
#define BACKUPS_VIEW_ENTRY 64U
#define BACKUPS_VIEW_COMPARE 128U
#define BACKUPS_VIEW_SPAN 64U

_Static_assert(MESH_UI_BACKUP_RADIOS_MAX <= BACKUPS_VIEW_SPAN &&
                   MESH_UI_BACKUP_ENTRIES_MAX <= BACKUPS_VIEW_SPAN,
               "every radio and every backup has a view byte of its own");

uint8_t mesh_ui_backups_view(enum mesh_ui_backups_level level, uint8_t index) {
    switch (level) {
    case MESH_UI_BACKUPS_RADIO:
        return index < BACKUPS_VIEW_SPAN ? index : MESH_UI_SETTINGS_NO_CHANNEL;
    case MESH_UI_BACKUPS_ENTRY:
        return index < BACKUPS_VIEW_SPAN ? (uint8_t)(BACKUPS_VIEW_ENTRY + index)
                                         : MESH_UI_SETTINGS_NO_CHANNEL;
    case MESH_UI_BACKUPS_COMPARE:
        return index < BACKUPS_VIEW_SPAN ? (uint8_t)(BACKUPS_VIEW_COMPARE + index)
                                         : MESH_UI_SETTINGS_NO_CHANNEL;
    case MESH_UI_BACKUPS_RADIOS:
    default:
        return MESH_UI_SETTINGS_NO_CHANNEL;
    }
}

enum mesh_ui_backups_level mesh_ui_backups_level_of(uint8_t view, uint8_t *index) {
    uint8_t at = 0U;
    enum mesh_ui_backups_level level = MESH_UI_BACKUPS_RADIOS;
    if (view < BACKUPS_VIEW_ENTRY) {
        level = MESH_UI_BACKUPS_RADIO;
        at = view;
    } else if (view < BACKUPS_VIEW_COMPARE) {
        level = MESH_UI_BACKUPS_ENTRY;
        at = (uint8_t)(view - BACKUPS_VIEW_ENTRY);
    } else if (view < BACKUPS_VIEW_COMPARE + BACKUPS_VIEW_SPAN) {
        level = MESH_UI_BACKUPS_COMPARE;
        at = (uint8_t)(view - BACKUPS_VIEW_COMPARE);
    }
    if (index != NULL) {
        *index = at;
    }
    return level;
}

int mesh_ui_backups_find_radio(const struct mesh_ui_backups *backups, uint32_t node) {
    if (backups == NULL || node == 0U) {
        return -1;
    }
    for (size_t i = 0; i < backups->radio_count && i < MESH_UI_BACKUP_RADIOS_MAX; ++i) {
        if (backups->radios[i].node == node) {
            return (int)i;
        }
    }
    return -1;
}

int mesh_ui_backups_find_entry(const struct mesh_ui_backups *backups, uint32_t node,
                               uint32_t sequence) {
    if (backups == NULL || node == 0U) {
        return -1;
    }
    for (size_t i = 0; i < backups->entry_count && i < MESH_UI_BACKUP_ENTRIES_MAX; ++i) {
        const struct mesh_ui_backup_entry *entry = &backups->entries[i];
        if (entry->header.node_id == node && entry->sequence == sequence) {
            return (int)i;
        }
    }
    return -1;
}

bool mesh_ui_backups_listed_under(const struct mesh_ui_backups *backups, size_t r, size_t e) {
    if (backups == NULL || r >= backups->radio_count || e >= backups->entry_count) {
        return false;
    }
    const struct mesh_ui_backup_radio *radio = &backups->radios[r];
    const struct mesh_radio_backup_header *header = &backups->entries[e].header;
    if (header->node_id == radio->node) {
        return true;
    }
    /* The other firmware, on the same link: a switch, not a second radio that happened to be
       plugged into the same port under the same protocol. */
    return radio->device[0] != '\0' && strcmp(header->device, radio->device) == 0 &&
           header->protocol != radio->protocol;
}

bool mesh_ui_backups_can_compare(const struct mesh_ui_backups *backups, size_t e) {
    if (backups == NULL || e >= backups->entry_count || backups->live_node == 0U) {
        return false;
    }
    const struct mesh_radio_backup_header *header = &backups->entries[e].header;
    return header->node_id == backups->live_node && header->protocol == backups->live_protocol;
}

void mesh_ui_backups_when(const struct mesh_radio_backup_header *header, uint32_t sequence,
                          char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    struct tm when;
    const time_t stamp = header != NULL ? (time_t)header->saved_at : 0;
    if (stamp == 0 || localtime_r(&stamp, &when) == NULL) {
        inkcell_str_format(out, out_len, MESH_STR_BACKUPS_NUMBERED, (unsigned)sequence);
        return;
    }
    static const inkcell_str_id kMonths[] = {
        MESH_STR_DATE_JAN, MESH_STR_DATE_FEB, MESH_STR_DATE_MAR, MESH_STR_DATE_APR,
        MESH_STR_DATE_MAY, MESH_STR_DATE_JUN, MESH_STR_DATE_JUL, MESH_STR_DATE_AUG,
        MESH_STR_DATE_SEP, MESH_STR_DATE_OCT, MESH_STR_DATE_NOV, MESH_STR_DATE_DEC,
    };
    static const inkcell_str_id kWeekdays[] = {
        MESH_STR_DATE_SUN, MESH_STR_DATE_MON, MESH_STR_DATE_TUE, MESH_STR_DATE_WED,
        MESH_STR_DATE_THU, MESH_STR_DATE_FRI, MESH_STR_DATE_SAT,
    };
    char day[32];
    inkcell_str_format(day, sizeof day, MESH_STR_DATE_WEEKDAY_DAY_MONTH,
                       inkcell_str(kWeekdays[when.tm_wday]), when.tm_mday,
                       inkcell_str(kMonths[when.tm_mon]));
    char clock[16];
    inkcell_fb_format_clock(header->saved_at, clock, sizeof clock);
    inkcell_str_format(out, out_len, MESH_STR_BACKUPS_WHEN, day, clock);
}

inkcell_str_id mesh_ui_backups_reason(uint8_t reason) {
    switch ((enum mesh_radio_backup_reason)reason) {
    case MESH_RADIO_BACKUP_MANUAL:
        return MESH_STR_BACKUPS_REASON_MANUAL;
    case MESH_RADIO_BACKUP_FIRST_CONNECT:
        return MESH_STR_BACKUPS_REASON_FIRST;
    case MESH_RADIO_BACKUP_BEFORE_WRITE:
        return MESH_STR_BACKUPS_REASON_WRITE;
    case MESH_RADIO_BACKUP_BEFORE_FIRMWARE:
        return MESH_STR_BACKUPS_REASON_FIRMWARE;
    case MESH_RADIO_BACKUP_REASON_NONE:
    default:
        return MESH_STR_COMMON_UNKNOWN;
    }
}

inkcell_str_id mesh_ui_backups_protocol(uint8_t protocol) {
    switch ((enum mesh_radio_backup_protocol)protocol) {
    case MESH_RADIO_BACKUP_MESHTASTIC:
        return MESH_STR_BACKUPS_PROTOCOL_MESHTASTIC;
    case MESH_RADIO_BACKUP_MESHCORE:
        return MESH_STR_BACKUPS_PROTOCOL_MESHCORE;
    case MESH_RADIO_BACKUP_PROTOCOL_NONE:
    default:
        return MESH_STR_COMMON_UNKNOWN;
    }
}

void mesh_ui_backups_radio_name(const struct mesh_ui_backup_radio *radio, char *out,
                                size_t out_len) {
    if (radio->name[0] != '\0') {
        inkwell_str_copy(out, out_len, radio->name);
    } else {
        snprintf(out, out_len, "!%08" PRIx32, radio->node);
    }
}

bool mesh_ui_backups_title(const struct mesh_ui_backups *backups, uint8_t view, char *title,
                           size_t title_len, char *parent, size_t parent_len) {
    if (backups == NULL || title == NULL || title_len == 0U || parent == NULL ||
        parent_len == 0U) {
        return false;
    }
    title[0] = '\0';
    parent[0] = '\0';
    uint8_t index = 0U;
    switch (mesh_ui_backups_level_of(view, &index)) {
    case MESH_UI_BACKUPS_RADIO: {
        if (index >= backups->radio_count) {
            return false;
        }
        const struct mesh_ui_backup_radio *radio = &backups->radios[index];
        char name[MESH_RADIO_BACKUP_TEXT];
        mesh_ui_backups_radio_name(radio, name, sizeof name);
        mesh_ui_chrome_list_title(title, title_len, name, radio->listed, radio->count, 0U);
        return true;
    }
    case MESH_UI_BACKUPS_ENTRY:
    case MESH_UI_BACKUPS_COMPARE: {
        if (index >= backups->entry_count) {
            return false;
        }
        const struct mesh_ui_backup_entry *entry = &backups->entries[index];
        char when[MESH_RADIO_BACKUP_TEXT];
        mesh_ui_backups_when(&entry->header, entry->sequence, when, sizeof when);
        if (mesh_ui_backups_level_of(view, NULL) == MESH_UI_BACKUPS_COMPARE) {
            inkwell_str_copy(parent, parent_len, when);
            inkwell_str_copy(title, title_len, inkcell_str(MESH_STR_BACKUPS_COMPARE));
        } else {
            if (entry->radio < backups->radio_count) {
                mesh_ui_backups_radio_name(&backups->radios[entry->radio], parent, parent_len);
            }
            inkwell_str_copy(title, title_len, when);
        }
        return true;
    }
    case MESH_UI_BACKUPS_RADIOS:
    default:
        return false;
    }
}

/* ---- the words for a change ---------------------------------------------------------------- */

/* The Settings tab's section for a topic, which is where its heading comes from. */
static enum mesh_ui_settings_section backups_topic_section(uint8_t topic) {
    switch ((enum mesh_radio_backup_topic)topic) {
    case MESH_RADIO_BACKUP_TOPIC_DEVICE:
        return MESH_UI_SETTINGS_DEVICE;
    case MESH_RADIO_BACKUP_TOPIC_POSITION:
    case MESH_RADIO_BACKUP_TOPIC_FIXED_POSITION:
        return MESH_UI_SETTINGS_POSITION;
    case MESH_RADIO_BACKUP_TOPIC_POWER:
        return MESH_UI_SETTINGS_POWER;
    case MESH_RADIO_BACKUP_TOPIC_NETWORK:
        return MESH_UI_SETTINGS_NETWORK;
    case MESH_RADIO_BACKUP_TOPIC_DISPLAY:
        return MESH_UI_SETTINGS_DISPLAY;
    case MESH_RADIO_BACKUP_TOPIC_LORA:
        return MESH_UI_SETTINGS_LORA;
    case MESH_RADIO_BACKUP_TOPIC_BLUETOOTH:
        return MESH_UI_SETTINGS_BLUETOOTH;
    case MESH_RADIO_BACKUP_TOPIC_SECURITY:
        return MESH_UI_SETTINGS_SECURITY;
    case MESH_RADIO_BACKUP_TOPIC_OWNER:
        return MESH_UI_SETTINGS_USER;
    case MESH_RADIO_BACKUP_TOPIC_RADIO_UI:
        return MESH_UI_SETTINGS_RADIO_UI;
    case MESH_RADIO_BACKUP_TOPIC_CANNED:
        return MESH_UI_SETTINGS_CANNED;
    default:
        return MESH_UI_SETTINGS_SECTION_COUNT;
    }
}

void mesh_ui_backups_topic(const struct mesh_radio_backup_change *change, char *out,
                           size_t out_len) {
    if (change == NULL || out == NULL || out_len == 0U) {
        return;
    }
    switch ((enum mesh_radio_backup_topic)change->topic) {
    case MESH_RADIO_BACKUP_TOPIC_CHANNEL:
        inkcell_str_format(out, out_len, MESH_STR_SETTINGS_TITLE_CHANNEL,
                           (unsigned)change->index);
        return;
    case MESH_RADIO_BACKUP_TOPIC_MODULE:
        /* The app has turned the protocol's module number into the section that edits it. */
        inkwell_str_copy(out, out_len,
                         change->index < MESH_UI_SETTINGS_SECTION_COUNT
                             ? mesh_ui_settings_section_name(
                                   (enum mesh_ui_settings_section)change->index)
                             : mesh_ui_settings_section_name(MESH_UI_SETTINGS_MODULES));
        return;
    case MESH_RADIO_BACKUP_TOPIC_CONTACT:
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_BACKUPS_TOPIC_CONTACTS));
        return;
    case MESH_RADIO_BACKUP_TOPIC_RINGTONE:
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_BACKUPS_TOPIC_RINGTONE));
        return;
    default:
        break;
    }
    const enum mesh_ui_settings_section section = backups_topic_section(change->topic);
    inkwell_str_copy(out, out_len,
                     section < MESH_UI_SETTINGS_SECTION_COUNT
                         ? mesh_ui_settings_section_name(section)
                         : inkcell_str(MESH_STR_BACKUPS_TOPIC_OTHER));
}

/* How a field's number is shown when it is not a plain number. */
enum backups_format {
    BACKUPS_PLAIN = 0,
    BACKUPS_FLOAT, /* a protobuf float, carried as its bits */
};

/*
 * Meshtastic's fields, by topic and protobuf tag, onto the Settings tab's own labels.
 *
 * The tags are literals because this side of the fence does not include nanopb; the ones a test
 * names are pinned against the generated constants there, the way the excluded-module bits are.
 * A field missing here is not lost - it is listed by number.
 */
struct backups_label {
    uint8_t topic;
    uint16_t field;
    inkcell_str_id label;
    uint8_t format;
};

static const struct backups_label k_meshtastic_labels[] = {
    {MESH_RADIO_BACKUP_TOPIC_DEVICE, 1, MESH_STR_SETTINGS_FIELD_DEVICE_ROLE, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DEVICE, 6, MESH_STR_SETTINGS_FIELD_DEVICE_REBROADCAST, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DEVICE, 7, MESH_STR_SETTINGS_FIELD_DEVICE_NODEINFO_SECS,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DEVICE, 8, MESH_STR_SETTINGS_FIELD_DEVICE_DOUBLE_TAP, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DEVICE, 11, MESH_STR_SETTINGS_FIELD_DEVICE_TZDEF, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DEVICE, 12, MESH_STR_SETTINGS_FIELD_DEVICE_LED_HEARTBEAT,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_POSITION, 1, MESH_STR_SETTINGS_FIELD_POSITION_BROADCAST_SECS,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_POSITION, 2, MESH_STR_SETTINGS_FIELD_POSITION_SMART, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_POSITION, 5, MESH_STR_SETTINGS_FIELD_POSITION_GPS_INTERVAL,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_POSITION, 10, MESH_STR_SETTINGS_FIELD_POSITION_SMART_DISTANCE,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_POSITION, 11, MESH_STR_SETTINGS_FIELD_POSITION_SMART_INTERVAL,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_POSITION, 13, MESH_STR_SETTINGS_FIELD_POSITION_GPS_MODE,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_POWER, 1, MESH_STR_SETTINGS_FIELD_POWER_SAVING, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_POWER, 2, MESH_STR_SETTINGS_FIELD_POWER_SHUTDOWN, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_POWER, 4, MESH_STR_SETTINGS_FIELD_POWER_WAIT_BT, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_POWER, 7, MESH_STR_SETTINGS_FIELD_POWER_LS_SECS, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_POWER, 8, MESH_STR_SETTINGS_FIELD_POWER_MIN_WAKE, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DISPLAY, 1, MESH_STR_SETTINGS_FIELD_DISPLAY_SCREEN_ON, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DISPLAY, 3, MESH_STR_SETTINGS_FIELD_DISPLAY_CAROUSEL, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DISPLAY, 4, MESH_STR_SETTINGS_FIELD_DISPLAY_COMPASS, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DISPLAY, 5, MESH_STR_SETTINGS_FIELD_DISPLAY_FLIP, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DISPLAY, 6, MESH_STR_SETTINGS_FIELD_DISPLAY_UNITS, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DISPLAY, 7, MESH_STR_SETTINGS_FIELD_DISPLAY_OLED, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DISPLAY, 8, MESH_STR_SETTINGS_FIELD_DISPLAY_MODE, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DISPLAY, 9, MESH_STR_SETTINGS_FIELD_DISPLAY_HEADING_BOLD,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DISPLAY, 10, MESH_STR_SETTINGS_FIELD_DISPLAY_WAKE_ON_MOTION,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DISPLAY, 13, MESH_STR_SETTINGS_FIELD_DISPLAY_LONG_NAMES,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_DISPLAY, 14, MESH_STR_SETTINGS_FIELD_DISPLAY_MESSAGE_BUBBLES,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 1, MESH_STR_SETTINGS_FIELD_LORA_USE_PRESET, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 2, MESH_STR_SETTINGS_FIELD_LORA_PRESET, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 3, MESH_STR_SETTINGS_FIELD_LORA_BANDWIDTH, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 4, MESH_STR_SETTINGS_FIELD_LORA_SPREAD, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 5, MESH_STR_SETTINGS_FIELD_LORA_CODING, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 6, MESH_STR_SETTINGS_FIELD_LORA_FREQUENCY_TRIM, BACKUPS_FLOAT},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 7, MESH_STR_SETTINGS_FIELD_LORA_REGION, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 8, MESH_STR_SETTINGS_FIELD_LORA_HOPS, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 9, MESH_STR_SETTINGS_FIELD_LORA_TX_ENABLED, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 10, MESH_STR_SETTINGS_FIELD_LORA_TX_POWER, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 11, MESH_STR_SETTINGS_FIELD_LORA_CHANNEL_NUM, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 12, MESH_STR_SETTINGS_FIELD_LORA_OVERRIDE_DUTY, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 13, MESH_STR_SETTINGS_FIELD_LORA_BOOST_GAIN, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 14, MESH_STR_SETTINGS_FIELD_LORA_OVERRIDE_FREQ, BACKUPS_FLOAT},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 104, MESH_STR_SETTINGS_FIELD_LORA_IGNORE_MQTT, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_LORA, 105, MESH_STR_SETTINGS_FIELD_LORA_OK_TO_MQTT, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_BLUETOOTH, 1, MESH_STR_SETTINGS_FIELD_BT_ENABLED, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_BLUETOOTH, 2, MESH_STR_SETTINGS_FIELD_BT_MODE, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_BLUETOOTH, 3, MESH_STR_SETTINGS_FIELD_BT_PIN, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_OWNER, 2, MESH_STR_SETTINGS_FIELD_USER_LONG_NAME, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_OWNER, 3, MESH_STR_SETTINGS_FIELD_USER_SHORT_NAME, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_OWNER, 6, MESH_STR_SETTINGS_FIELD_USER_LICENSED, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_OWNER, 9, MESH_STR_SETTINGS_FIELD_USER_UNMESSAGEABLE, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_CHANNEL, 3, MESH_STR_SETTINGS_FIELD_CHANNEL_ROLE, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_CHANNEL, 202, MESH_STR_SETTINGS_FIELD_CHANNEL_KEY, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_CHANNEL, 203, MESH_STR_SETTINGS_FIELD_CHANNEL_NAME, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_CHANNEL, 205, MESH_STR_SETTINGS_FIELD_CHANNEL_UPLINK, BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_CHANNEL, 206, MESH_STR_SETTINGS_FIELD_CHANNEL_DOWNLINK,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_CHANNEL, 20701, MESH_STR_SETTINGS_FIELD_CHANNEL_POSITION,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_CHANNEL, 20702, MESH_STR_SETTINGS_FIELD_CHANNEL_MUTED,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_FIXED_POSITION, 1, MESH_STR_SETTINGS_FIELD_POSITION_LATITUDE,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_FIXED_POSITION, 2, MESH_STR_SETTINGS_FIELD_POSITION_LONGITUDE,
     BACKUPS_PLAIN},
    {MESH_RADIO_BACKUP_TOPIC_FIXED_POSITION, 3, MESH_STR_SETTINGS_FIELD_POSITION_ALTITUDE,
     BACKUPS_PLAIN},
};

/* MeshCore's fields are this client's own numbers, one label each whatever the topic. */
static const struct backups_label k_meshcore_labels[] = {
    {0, MESH_MESHCORE_BACKUP_FIELD_NAME, MESH_STR_SETTINGS_FIELD_USER_LONG_NAME, BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_FREQUENCY, MESH_STR_SETTINGS_FIELD_LORA_FREQUENCY,
     BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_BANDWIDTH, MESH_STR_SETTINGS_FIELD_LORA_BANDWIDTH,
     BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_SPREADING, MESH_STR_SETTINGS_FIELD_LORA_SPREAD, BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_CODING, MESH_STR_SETTINGS_FIELD_LORA_CODING, BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_TX_POWER, MESH_STR_SETTINGS_FIELD_LORA_TX_POWER,
     BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_LATITUDE, MESH_STR_SETTINGS_FIELD_POSITION_LATITUDE,
     BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_LONGITUDE, MESH_STR_SETTINGS_FIELD_POSITION_LONGITUDE,
     BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_ADVERT_LOCATION, MESH_STR_SETTINGS_FIELD_ADVERT_LOCATION,
     BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_MANUAL_ADD, MESH_STR_SETTINGS_FIELD_AUTO_ADD, BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_TELEMETRY, MESH_STR_SETTINGS_FIELD_ASK_TELEMETRY,
     BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_MULTI_ACKS, MESH_STR_SETTINGS_FIELD_EXTRA_ACKS, BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_PUBLIC_KEY, MESH_STR_BACKUPS_FIELD_PUBLIC_KEY, BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_PIN, MESH_STR_SETTINGS_FIELD_BT_PIN, BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_CHANNEL_NAME, MESH_STR_SETTINGS_FIELD_CHANNEL_NAME,
     BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_CHANNEL_SECRET, MESH_STR_SETTINGS_FIELD_CHANNEL_KEY,
     BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_CONTACT_TYPE, MESH_STR_BACKUPS_FIELD_CONTACT_TYPE,
     BACKUPS_PLAIN},
    {0, MESH_MESHCORE_BACKUP_FIELD_CONTACT_FLAGS, MESH_STR_BACKUPS_FIELD_CONTACT_FLAGS,
     BACKUPS_PLAIN},
};

static const struct backups_label *backups_label(uint8_t protocol,
                                                 const struct mesh_radio_backup_change *change) {
    if (protocol == MESH_RADIO_BACKUP_MESHCORE) {
        for (size_t i = 0; i < sizeof k_meshcore_labels / sizeof k_meshcore_labels[0]; ++i) {
            if (k_meshcore_labels[i].field == change->field) {
                return &k_meshcore_labels[i];
            }
        }
        return NULL;
    }
    for (size_t i = 0; i < sizeof k_meshtastic_labels / sizeof k_meshtastic_labels[0]; ++i) {
        if (k_meshtastic_labels[i].topic == change->topic &&
            k_meshtastic_labels[i].field == change->field) {
            return &k_meshtastic_labels[i];
        }
    }
    return NULL;
}

void mesh_ui_backups_field(uint8_t protocol, const struct mesh_radio_backup_change *change,
                           char *out, size_t out_len) {
    if (change == NULL || out == NULL || out_len == 0U) {
        return;
    }
    /* A contact is named by who it is; which of its fields moved is the value's to say. */
    if (change->topic == MESH_RADIO_BACKUP_TOPIC_CONTACT && change->subject[0] != '\0') {
        inkwell_str_copy(out, out_len, change->subject);
        return;
    }
    const struct backups_label *label = backups_label(protocol, change);
    if (label != NULL) {
        inkwell_str_copy(out, out_len, inkcell_str(label->label));
    } else if (change->field == 0U ||
               (change->topic == MESH_RADIO_BACKUP_TOPIC_CHANNEL && change->field == 2U)) {
        /* The whole of it - a section, or a channel's settings - there on one side only. */
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_BACKUPS_FIELD_WHOLE));
    } else {
        inkcell_str_format(out, out_len, MESH_STR_BACKUPS_FIELD_NUMBER, (unsigned)change->field);
    }
}

static void backups_value(const struct mesh_radio_backup_value *value, uint8_t format, char *out,
                          size_t out_len) {
    switch ((enum mesh_radio_backup_value_kind)value->kind) {
    case MESH_RADIO_BACKUP_VALUE_BOOL:
        inkwell_str_copy(out, out_len,
                         inkcell_str(value->number != 0 ? MESH_STR_COMMON_ON : MESH_STR_COMMON_OFF));
        return;
    case MESH_RADIO_BACKUP_VALUE_INT:
        snprintf(out, out_len, "%" PRId64, value->number);
        return;
    case MESH_RADIO_BACKUP_VALUE_UINT:
        if (format == BACKUPS_FLOAT) {
            const uint32_t bits = (uint32_t)value->number;
            float f = 0.0f;
            memcpy(&f, &bits, sizeof f);
            snprintf(out, out_len, "%.3f", (double)f);
            return;
        }
        snprintf(out, out_len, "%" PRIu64, (uint64_t)value->number);
        return;
    case MESH_RADIO_BACKUP_VALUE_DECIMAL: {
        int64_t scale = 1;
        for (uint8_t i = 0; i < value->decimals && i < 12U; ++i) {
            scale *= 10;
        }
        const int64_t whole = value->number / scale;
        const int64_t part = value->number % scale;
        snprintf(out, out_len, "%s%" PRId64 ".%0*" PRId64,
                 value->number < 0 && whole == 0 ? "-" : "", whole, (int)value->decimals,
                 part < 0 ? -part : part);
        return;
    }
    case MESH_RADIO_BACKUP_VALUE_TEXT:
        inkwell_str_copy(out, out_len,
                         value->text[0] != '\0' ? value->text
                                                : inkcell_str(MESH_STR_BACKUPS_VALUE_EMPTY));
        return;
    case MESH_RADIO_BACKUP_VALUE_OPAQUE:
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_BACKUPS_VALUE_SET));
        return;
    case MESH_RADIO_BACKUP_VALUE_ABSENT:
    default:
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_BACKUPS_VALUE_ABSENT));
        return;
    }
}

void mesh_ui_backups_change(uint8_t protocol, const struct mesh_radio_backup_change *change,
                            char *out, size_t out_len) {
    if (change == NULL || out == NULL || out_len == 0U) {
        return;
    }
    switch ((enum mesh_radio_backup_change_kind)change->kind) {
    case MESH_RADIO_BACKUP_ADDED:
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_BACKUPS_CHANGE_ADDED));
        return;
    case MESH_RADIO_BACKUP_REMOVED:
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_BACKUPS_CHANGE_REMOVED));
        return;
    case MESH_RADIO_BACKUP_CHANGED:
    default:
        break;
    }
    const struct backups_label *label = backups_label(protocol, change);
    const uint8_t format = label != NULL ? label->format : BACKUPS_PLAIN;
    char before[MESH_RADIO_BACKUP_VALUE_TEXT_MAX];
    char after[MESH_RADIO_BACKUP_VALUE_TEXT_MAX];
    backups_value(&change->before, format, before, sizeof before);
    backups_value(&change->after, format, after, sizeof after);
    inkcell_str_format(out, out_len, MESH_STR_BACKUPS_CHANGE_VALUE, before, after);
}
