#pragma once

/*
 * Meshtastic's `.cfg`: a profile to and from the file the official apps export and import.
 *
 * The file is one upstream `DeviceProfile` (clientonly.proto), binary, nothing around it: a
 * LocalConfig and a LocalModuleConfig with a member per section, the channels as a
 * `meshtastic.org/e/#` link, the ringtone and the canned messages as text - and the owner's two
 * names and a fixed position, which a profile never carries and so an export never writes and an
 * import leaves behind. So does the Security section, which is the radio's keys.
 *
 * **Import makes a profile; it never writes to a radio.** A file off somebody else's phone is
 * read into a profile on the card, where it can be looked at and compared before it is applied
 * like any other.
 *
 * Config and module sections are matched to LocalConfig's and LocalModuleConfig's members by
 * their message type rather than by a table here, so a module the protobufs gain is carried both
 * ways the day they are regenerated.
 */

#include "mesh/core/radio_backup.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Room for the largest DeviceProfile a profile can make: both local configs whole, eight
   channels as a link, the ringtone and the canned messages. */
#define MESH_RADIO_PROFILE_CFG_MAX 8192U

/*
 * `profile` as a DeviceProfile into `out`. How many bytes, or -EINVAL for a profile that is not
 * one or not Meshtastic's, -EBADMSG for a section that does not decode, -ENOSPC.
 */
int mesh_radio_profile_cfg_encode(const struct mesh_radio_backup *profile, uint8_t *out,
                                  size_t out_len);

/*
 * A DeviceProfile into a Meshtastic profile named `name`, carrying every part the file has that
 * a profile may. The channel link, when there is one, is the channel table whole: its first
 * channel the primary, the rest secondaries, every slot past them switched off.
 *
 * 0; -EBADMSG for bytes that are not a DeviceProfile or a link that does not parse; -EINVAL for
 * a file with nothing a profile carries in it; -ENOSPC.
 */
int mesh_radio_profile_cfg_decode(const uint8_t *data, size_t len, const char *name,
                                  struct mesh_radio_backup *out);

/* The same through a file: written whole through a temporary, and read whole. */
int mesh_radio_profile_cfg_write(const struct mesh_radio_backup *profile, const char *path);
int mesh_radio_profile_cfg_read(const char *path, const char *name, struct mesh_radio_backup *out);

#ifdef __cplusplus
}
#endif
