#pragma once

/*
 * The Profiles section's three screens, and the names of a profile's parts.
 *
 * **The Backups section's shape, a level shorter** (mesh/ui/backups.h): the profiles on the card,
 * one profile, and one profile against the radio - told apart by the one byte every section's
 * rows are built from, held by the nav as a sequence number that does not move when a profile
 * is added or deleted under the reader.
 *
 * **A part is named by what a comparison calls the same thing** - its section's name, "Channels"
 * for the table - so the parts a profile lists and the headings over its comparison read alike.
 */

#include "mesh/core/radio_backup_diff.h"
#include "mesh/core/radio_profile.h"
#include "mesh/ui/store_settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mesh_ui_profiles_level {
    MESH_UI_PROFILES_LIST = 0, /* the profiles, and the `.cfg` files beside them */
    MESH_UI_PROFILES_ITEM,     /* one profile; the index is its in `items` */
    MESH_UI_PROFILES_COMPARE,  /* that profile against the radio */
};

uint8_t mesh_ui_profiles_view(enum mesh_ui_profiles_level level, uint8_t index);
enum mesh_ui_profiles_level mesh_ui_profiles_level_of(uint8_t view, uint8_t *index);

/* Where a profile is in what was published; -1 when it is not there any more. */
int mesh_ui_profiles_find(const struct mesh_ui_profiles *profiles, uint32_t sequence);

/*
 * Whether profile `i` can be compared with the radio on the link now: a radio read far enough
 * to capture (`live_protocol` is the Backups section's), running the profile's protocol.
 */
bool mesh_ui_profiles_can_compare(const struct mesh_ui_profiles *profiles, size_t i,
                                  uint8_t live_protocol);

/* A part's name: "LoRa", "MQTT", "Channels". */
void mesh_ui_profiles_part_name(const struct mesh_ui_profiles *profiles, uint8_t protocol,
                                const struct mesh_radio_profile_part *part, char *out,
                                size_t out_len);

/* The parts a profile carries, in the order a picker would list them; how many. */
size_t mesh_ui_profiles_parts(const struct mesh_radio_backup_parts *parts,
                              struct mesh_radio_profile_part *out, size_t max);

/* The top bar's words past the section's name, as mesh_ui_backups_title(). False on the list. */
bool mesh_ui_profiles_title(const struct mesh_ui_profiles *profiles, uint8_t view, char *title,
                            size_t title_len, char *parent, size_t parent_len);

/* A comparison's value column for a profile: mesh_ui_backups_change(), with a whole section only
   the profile has said as that rather than as "only in the backup". */
void mesh_ui_profiles_change(uint8_t protocol, const struct mesh_radio_backup_change *change,
                             char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
