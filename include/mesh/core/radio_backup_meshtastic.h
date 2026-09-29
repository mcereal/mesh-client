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

#ifdef __cplusplus
}
#endif
