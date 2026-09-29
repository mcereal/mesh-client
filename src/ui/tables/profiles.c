/*
 * The Profiles section's levels and the names of a profile's parts. See
 * include/mesh/ui/profiles.h.
 */

#include "mesh/ui/profiles.h"

#include "inkwell/base/text.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/backups.h"
#include "mesh/ui/settings.h"

#include <stdio.h>
#include <string.h>

/* The byte: 0..63 a profile, 64..127 its comparison; the list is 0xFF. */
#define PROFILES_VIEW_COMPARE 64U
#define PROFILES_VIEW_SPAN 64U

_Static_assert(MESH_UI_PROFILES_MAX <= PROFILES_VIEW_SPAN, "every profile has a view byte");

uint8_t mesh_ui_profiles_view(enum mesh_ui_profiles_level level, uint8_t index) {
    if (index >= PROFILES_VIEW_SPAN) {
        return MESH_UI_SETTINGS_NO_CHANNEL;
    }
    switch (level) {
    case MESH_UI_PROFILES_ITEM:
        return index;
    case MESH_UI_PROFILES_COMPARE:
        return (uint8_t)(PROFILES_VIEW_COMPARE + index);
    case MESH_UI_PROFILES_LIST:
    default:
        return MESH_UI_SETTINGS_NO_CHANNEL;
    }
}

enum mesh_ui_profiles_level mesh_ui_profiles_level_of(uint8_t view, uint8_t *index) {
    uint8_t at = 0U;
    enum mesh_ui_profiles_level level = MESH_UI_PROFILES_LIST;
    if (view < PROFILES_VIEW_COMPARE) {
        level = MESH_UI_PROFILES_ITEM;
        at = view;
    } else if (view < PROFILES_VIEW_COMPARE + PROFILES_VIEW_SPAN) {
        level = MESH_UI_PROFILES_COMPARE;
        at = (uint8_t)(view - PROFILES_VIEW_COMPARE);
    }
    if (index != NULL) {
        *index = at;
    }
    return level;
}

int mesh_ui_profiles_find(const struct mesh_ui_profiles *profiles, uint32_t sequence) {
    if (profiles == NULL || sequence == 0U) {
        return -1;
    }
    for (size_t i = 0; i < profiles->count && i < MESH_UI_PROFILES_MAX; ++i) {
        if (profiles->items[i].sequence == sequence) {
            return (int)i;
        }
    }
    return -1;
}

bool mesh_ui_profiles_can_compare(const struct mesh_ui_profiles *profiles, size_t i,
                                  uint8_t live_protocol) {
    return profiles != NULL && i < profiles->count &&
           live_protocol != MESH_RADIO_BACKUP_PROTOCOL_NONE &&
           profiles->items[i].header.protocol == live_protocol;
}

void mesh_ui_profiles_part_name(const struct mesh_ui_profiles *profiles, uint8_t protocol,
                                const struct mesh_radio_profile_part *part, char *out,
                                size_t out_len) {
    if (part == NULL || out == NULL || out_len == 0U) {
        return;
    }
    /* The table, not a slot of it: the heading a comparison has no need of. */
    if (part->topic == MESH_RADIO_BACKUP_TOPIC_CHANNEL) {
        inkwell_str_copy(out, out_len, mesh_ui_settings_section_name(MESH_UI_SETTINGS_CHANNELS));
        return;
    }
    /* Otherwise the heading a comparison gives the same topic - on MeshCore too, whose "other"
       settings are named for the Device section that holds them. */
    (void)protocol;
    struct mesh_radio_backup_change change;
    memset(&change, 0, sizeof change);
    change.topic = part->topic;
    change.index = part->index;
    if (part->topic == MESH_RADIO_BACKUP_TOPIC_MODULE) {
        change.index = profiles != NULL && part->index < 32U
                           ? profiles->module_names[part->index]
                           : (uint16_t)(MESH_UI_BACKUPS_MODULE_UNPLACED + part->index);
    }
    mesh_ui_backups_topic(&change, out, out_len);
}

size_t mesh_ui_profiles_parts(const struct mesh_radio_backup_parts *parts,
                              struct mesh_radio_profile_part *out, size_t max) {
    size_t count = 0U;
    if (parts == NULL) {
        return 0U;
    }
    for (uint8_t topic = 0U; topic < MESH_RADIO_BACKUP_TOPIC_COUNT; ++topic) {
        if (topic == MESH_RADIO_BACKUP_TOPIC_MODULE) {
            for (uint16_t tag = 0U; tag < 32U; ++tag) {
                if (mesh_radio_profile_parts_has(parts, topic, tag) && count < max) {
                    out[count++] = (struct mesh_radio_profile_part){.topic = topic, .index = tag};
                }
            }
            continue;
        }
        if (mesh_radio_profile_parts_has(parts, topic, 0U) && count < max) {
            out[count++] = (struct mesh_radio_profile_part){.topic = topic};
        }
    }
    return count;
}

bool mesh_ui_profiles_title(const struct mesh_ui_profiles *profiles, uint8_t view, char *title,
                            size_t title_len, char *parent, size_t parent_len) {
    if (profiles == NULL || title == NULL || title_len == 0U || parent == NULL ||
        parent_len == 0U) {
        return false;
    }
    title[0] = '\0';
    parent[0] = '\0';
    uint8_t index = 0U;
    const enum mesh_ui_profiles_level level = mesh_ui_profiles_level_of(view, &index);
    if (level == MESH_UI_PROFILES_LIST || index >= profiles->count) {
        return false;
    }
    const char *name = profiles->items[index].header.name;
    if (level == MESH_UI_PROFILES_COMPARE) {
        inkwell_str_copy(parent, parent_len, name);
        inkwell_str_copy(title, title_len, inkcell_str(MESH_STR_BACKUPS_COMPARE));
    } else {
        inkwell_str_copy(title, title_len, name);
    }
    return true;
}

void mesh_ui_profiles_change(uint8_t protocol, const struct mesh_radio_backup_change *change,
                             char *out, size_t out_len) {
    if (change != NULL && change->kind == MESH_RADIO_BACKUP_REMOVED && out != NULL &&
        out_len > 0U) {
        inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_PROFILES_CHANGE_REMOVED));
        return;
    }
    mesh_ui_backups_change(protocol, change, out, out_len);
}
