#pragma once

/*
 * The Backups section's four screens, and the words for what a comparison found.
 *
 * **One section, four levels, one byte.** The radios, one radio's backups, one backup, and one
 * backup against the radio: the same shape as a Maps group, a level deeper. Every section's row
 * builder, bar and confirm sheet is handed the open section and one byte for "which one of it" -
 * the channel slot, the Maps group - so the four levels are that byte (mesh_ui_backups_view()),
 * and everything that serves a section serves these without learning a new question. The nav
 * holds the levels by what they are about - a node and a sequence number, which do not move when
 * a backup is added or pruned under the reader - and works the byte out on every frame against
 * the list that is actually published.
 *
 * **What a change is called is decided here, not in core.** A comparison names a topic and a
 * field by the protocol's own number; this table turns the pair into a label the Settings tab
 * already uses for the same field - so "Hop limit" in a comparison is the word on the LoRa
 * screen - and a field it has no row for is named by its number rather than dropped.
 */

#include "mesh/core/radio_backup_diff.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/store_settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mesh_ui_backups_level {
    MESH_UI_BACKUPS_RADIOS = 0, /* the radios with backups */
    MESH_UI_BACKUPS_RADIO,      /* one radio's backups; the index is the radio's */
    MESH_UI_BACKUPS_ENTRY,      /* one backup; the index is the entry's */
    MESH_UI_BACKUPS_COMPARE,    /* that backup against the radio */
};

/* The byte for a level and an index, and back. The radio list is MESH_UI_SETTINGS_NO_CHANNEL,
   which is what a zeroed nav and every other section's list already say. */
uint8_t mesh_ui_backups_view(enum mesh_ui_backups_level level, uint8_t index);
enum mesh_ui_backups_level mesh_ui_backups_level_of(uint8_t view, uint8_t *index);

/* Where a radio or a backup is in what was published; -1 when it is not there any more. */
int mesh_ui_backups_find_radio(const struct mesh_ui_backups *backups, uint32_t node);
int mesh_ui_backups_find_entry(const struct mesh_ui_backups *backups, uint32_t node,
                               uint32_t sequence);

/*
 * Whether entry `e` is listed under radio `r`: its own backups, and those taken of the same
 * device under the other firmware - a radio that was switched from one to the other has a new
 * node number, and its history before the switch is still this radio's history.
 */
bool mesh_ui_backups_listed_under(const struct mesh_ui_backups *backups, size_t r, size_t e);

/* Whether a backup can be compared with the radio on the link now: the same node, the same
   protocol, and a radio read far enough to capture. */
bool mesh_ui_backups_can_compare(const struct mesh_ui_backups *backups, size_t e);

/* A radio by the name its newest backup gave it, or "!a1b2c3d4" when it gave none. */
void mesh_ui_backups_radio_name(const struct mesh_ui_backup_radio *radio, char *out,
                                size_t out_len);

/* "Mon 3 Sep 14:05", or "Backup 7" for one taken with no clock. */
void mesh_ui_backups_when(const struct mesh_radio_backup_header *header, uint32_t sequence,
                          char *out, size_t out_len);
inkcell_str_id mesh_ui_backups_reason(uint8_t reason);
inkcell_str_id mesh_ui_backups_protocol(uint8_t protocol);

/*
 * The top bar's words past the section's own name, for the level `view` names: `title` is this
 * screen's and `parent` the level between it and the section, "" when there is none. False on
 * the radio list, which is titled by the section.
 *
 * A radio's list is titled through mesh_ui_chrome_list_title(), which says "(10 of 12)" when
 * the list is not showing all of a radio's backups and nothing when it is.
 */
bool mesh_ui_backups_title(const struct mesh_ui_backups *backups, uint8_t view, char *title,
                           size_t title_len, char *parent, size_t parent_len);

/*
 * Appends to a restore sheet's text the sentence saying the backup came from other firmware than
 * the radio runs now, when it did: a field the radio's firmware does not know is dropped by it,
 * and the comparison after the restart will list it. Nothing is appended when the two match or
 * either is unknown.
 */
void mesh_ui_backups_restore_note(const struct mesh_ui_settings *settings, uint8_t view, char *text,
                                  size_t text_len);

/*
 * A Meshtastic module change's index, once the app has named it (app_backup_name_modules()): the
 * Settings section that edits the module, or this plus its ModuleConfig tag for one no section
 * edits - the serial module, say - so the heading can still say which module it is.
 */
#define MESH_UI_BACKUPS_MODULE_UNPLACED ((uint16_t)MESH_UI_SETTINGS_SECTION_COUNT)

/*
 * Whether a restore can act on a change. Not on a contact only the radio has, and not, on
 * Meshtastic, on a whole section only the radio has - a module a newer firmware added, or one a
 * backup from an older build did not keep - which mesh_radio_backup_meshtastic_plan() leaves
 * where it is. Counted as either, such a change offered a restore that could not remove it, and
 * judged the restore as having failed on it.
 */
bool mesh_ui_backups_change_restorable(uint8_t protocol,
                                       const struct mesh_radio_backup_change *change);

/* A comparison's heading for a change: its section's name, or "Channel 2", or "Contacts". */
void mesh_ui_backups_topic(const struct mesh_radio_backup_change *change, char *out,
                           size_t out_len);
/* The change's label within that heading: the field's, the contact's name, or "Field 14". */
void mesh_ui_backups_field(uint8_t protocol, const struct mesh_radio_backup_change *change,
                           char *out, size_t out_len);
/* What it was and what it is: "5 → 3", "Only on the radio", "Only in the backup". */
void mesh_ui_backups_change(uint8_t protocol, const struct mesh_radio_backup_change *change,
                            char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
