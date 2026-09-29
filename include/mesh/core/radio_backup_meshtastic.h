#pragma once

/*
 * A Meshtastic radio's settings into a backup's sections, and back.
 *
 * **Each section is one upstream protobuf, encoded as the radio sent it**: a `Config` or a
 * `ModuleConfig` with its variant tag set, a `Channel`, the owner's `User`, the radio's
 * `DeviceUIConfig`. So the file holds what the firmware's own types say, and a field a newer
 * firmware adds rides along the day the protobufs are regenerated, with no change here - the
 * same argument channel_share.c makes for comparing LoRa configs by their bytes.
 *
 * **The private key is dropped on the way in.** `SecurityConfig.private_key` is emptied before
 * the section is encoded: a backup is a file on a removable card, and whether one may carry the
 * key that *is* the radio's identity on the mesh is a separate decision. The public key and the
 * admin keys stay; they are not secrets.
 *
 * **Only a radio that has finished telling us about itself is captured.** The want_config
 * handshake streams every Config section, every module and every channel; a backup taken while
 * it is still arriving would be a radio with half its settings, and restoring it would wipe the
 * other half. The canned messages and the ringtone arrive later, over admin requests an older
 * firmware does not answer, and are carried when they are there.
 *
 * **Only the radio on the link.** While the Settings tab administers another node, the settings
 * record is that node's, and its sections cannot be told apart from ours by looking at them.
 */

#include "mesh/core/radio_backup.h"
#include "mesh/core/radio_backup_diff.h"
#include "mesh/core/radio_profile.h"
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The section tags. Numbers, because they are written into files and must never move. */
enum mesh_radio_backup_meshtastic_tag {
    MESH_RADIO_BACKUP_MT_CONFIG = 1,    /* meshtastic_Config */
    MESH_RADIO_BACKUP_MT_MODULE = 2,    /* meshtastic_ModuleConfig */
    MESH_RADIO_BACKUP_MT_CHANNEL = 3,   /* meshtastic_Channel */
    MESH_RADIO_BACKUP_MT_OWNER = 4,     /* meshtastic_User */
    MESH_RADIO_BACKUP_MT_UI_CONFIG = 5, /* meshtastic_DeviceUIConfig */
    MESH_RADIO_BACKUP_MT_CANNED = 6,    /* the canned message list, as text */
    MESH_RADIO_BACKUP_MT_RINGTONE = 7,  /* the ringtone, as RTTTL text */
    MESH_RADIO_BACKUP_MT_POSITION = 8,  /* meshtastic_Position: the fixed position, when set */
};

/*
 * Whether `settings` holds enough of the radio on the link to be worth keeping: every Config
 * section, the owner, the metadata and every channel slot. False while another node is being
 * administered.
 */
bool mesh_radio_backup_meshtastic_ready(const struct mesh_radio_settings *settings);

/*
 * Fills `backup` - header and sections - from what this client holds about the radio on the
 * link. `status` is the session's handshake, for the node number, how many nodes the radio
 * counts, and its own position. The caller sets the reason, the time and the device.
 *
 * 0, -EAGAIN when the radio is not ready (see above), -ENOSPC or -EINVAL from the container.
 * `backup` is reset first either way.
 */
int mesh_radio_backup_meshtastic_capture(const struct mesh_radio_settings *settings,
                                         const struct mesh_handshake_status *status,
                                         struct mesh_radio_backup *backup);

/*
 * The other direction: every section folded into `settings` as if the radio had sent it, through
 * the same apply calls the handshake uses. `settings` is reset first, so what it holds after is
 * the backup and nothing else. `position`, when not NULL, receives the fixed position if the
 * backup has one.
 *
 * 0, -EPROTO for a backup of another protocol, -EBADMSG for a section that does not decode.
 * Tags this build does not know are skipped.
 */
int mesh_radio_backup_meshtastic_read(const struct mesh_radio_backup *backup,
                                      struct mesh_radio_settings *settings,
                                      meshtastic_Position *position);

/*
 * What differs between two Meshtastic backups, section by section and then field by field.
 *
 * Sections are paired by what they are - a Config by its variant, a module by its, a channel by
 * its slot - rather than by position, so a backup from a firmware that sends its modules in
 * another order still pairs up. Identical bytes are the same section and cost one comparison;
 * only a pair that differs is decoded and walked, through nanopb's field iterator, so a field
 * the protobufs gain is compared the day they are regenerated. A field's number in the change is
 * its protobuf tag; one inside a nested message is the parent's times 100 plus its own.
 *
 * 0, -EPROTO when either backup is not Meshtastic's, -ENOMEM.
 */
int mesh_radio_backup_meshtastic_diff(const struct mesh_radio_backup *a,
                                      const struct mesh_radio_backup *b,
                                      struct mesh_radio_backup_diff *out);

/*
 * The writes that would put `backup` back onto the radio `settings` describes: one per section
 * whose encoded bytes differ from the radio's own, in the order the backup holds them (Config,
 * modules, channels, owner, the UI config, canned messages, ringtone, then the fixed position,
 * which has to follow the Position section it sets a flag in). A section the radio has and the
 * backup does not is left alone - there is nothing to put back.
 *
 * **The radio keeps its own keys.** A backup never holds the private key, and writing its
 * SecurityConfig as it stands would hand the radio an empty one, which the firmware answers by
 * making a new key pair - a new identity on the mesh, and every node that trusted the old one
 * refusing the new. So the Security write carries the radio's current key pair, and the owner
 * write its current public key, whatever else they restore. A radio that has not reported its
 * private key is not sent a Security write at all.
 *
 * `unwritable`, when not NULL, receives how many sections differ but have no write - a Security
 * section with no key of the radio's own to carry, a module this build keeps no binding for -
 * so a plan of 0 can be told apart from a radio that already matches.
 *
 * `backup` may be a profile (mesh/core/radio_profile.h), which holds only the sections it keeps -
 * so only those are written - and whose position settings take this radio's own fixed_position
 * flag rather than the cleared one the profile carries.
 *
 * Returns how many writes were planned, -EPROTO for a
 * backup of another protocol, -EAGAIN when the radio has not been read far enough to compare
 * (mesh_radio_backup_meshtastic_capture()'s refusal), -ENOSPC when more than `max` sections
 * differ, -ENOMEM.
 */
int mesh_radio_backup_meshtastic_plan(const struct mesh_radio_backup *backup,
                                      const struct mesh_radio_settings *settings,
                                      const struct mesh_handshake_status *status,
                                      struct mesh_admin_request *writes, size_t max,
                                      size_t *unwritable);

/*
 * The parts a profile made from `backup` could carry (mesh_radio_profile_offer()): each Config
 * section but Security, each module, the channels as one part, the UI config, the canned
 * messages, the ringtone - in the order the backup holds them. How many, -EPROTO, -ENOMEM.
 */
int mesh_radio_backup_meshtastic_offer(const struct mesh_radio_backup *backup,
                                       struct mesh_radio_profile_part *out, size_t max);

/*
 * `backup` cut down to `parts` into `out`, header and all: the sections whose part is in `parts`
 * and may be in a profile, with PositionConfig's fixed_position cleared. How a profile is made,
 * and how the radio is cut down to compare with one. 0, -EPROTO, -EBADMSG, -ENOSPC, -ENOMEM.
 */
int mesh_radio_backup_meshtastic_keep(const struct mesh_radio_backup *backup,
                                      const struct mesh_radio_backup_parts *parts,
                                      struct mesh_radio_backup *out);

#ifdef __cplusplus
}
#endif
