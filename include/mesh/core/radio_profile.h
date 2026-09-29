#pragma once

/*
 * Profiles: a radio's settings with the radio taken out, to put on any radio of the same protocol.
 *
 * A backup is one radio as it was, and restoring one is putting that radio back. A profile is the
 * other use of the same bytes - setting up a new radio like an old one, or keeping several alike -
 * and it is made from a backup by keeping the parts somebody picked (LoRa, the channels, MQTT,
 * say) and never the ones that make a radio itself: its owner and name, its fixed position, its
 * keys, and on MeshCore its contacts. Two radios given the same profile are two radios on the
 * same mesh, not one radio twice.
 *
 * **The same container as a backup** (mesh/core/radio_backup.h), under the `profile` reason, with
 * no node and the profile's own name, and the parts it carries in the header. So a profile is
 * read and written, compared and planned by the code that already does it for backups: a
 * Meshtastic profile holds only the sections it keeps, and mesh_radio_backup_meshtastic_plan()
 * writes only the sections a backup holds; a MeshCore profile holds the settings record with the
 * radio's own fields and the groups it leaves out emptied on both sides of a comparison, and
 * mesh_meshcore_backup_plan_profile() writes only the groups it names.
 *
 * **One protocol per profile.** Nothing translates a Meshtastic LoRa config into a MeshCore one;
 * a profile applied to a radio of the other protocol is refused (-EPROTO), before anything is
 * planned.
 *
 * **Their own directory.** Profiles are not a radio's history, and nothing prunes them. The
 * directory is also where Meshtastic's `.cfg` files are read from and written to
 * (mesh/core/radio_profile_cfg.h): the one folder on the card a person has to find.
 */

#include "mesh/core/radio_backup.h"
#include "mesh/core/radio_backup_diff.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The most parts one profile can offer: a Meshtastic radio's seven Config sections, every module,
   the channels, the UI config, the canned messages and the ringtone. */
#define MESH_RADIO_PROFILE_PARTS_MAX 32U
#define MESH_RADIO_PROFILE_FILE_MAX 64U

/* One part, as a picker lists it: a topic, and for a module which one (its variant tag). */
struct mesh_radio_profile_part {
    uint8_t topic; /* enum mesh_radio_backup_topic */
    uint16_t index;
};

bool mesh_radio_profile_parts_has(const struct mesh_radio_backup_parts *parts, uint8_t topic,
                                  uint16_t index);
void mesh_radio_profile_parts_set(struct mesh_radio_backup_parts *parts, uint8_t topic,
                                  uint16_t index, bool on);

/*
 * Whether a part may go into a profile of `protocol` at all. The owner, a fixed position and the
 * keys are the radio; so are a MeshCore radio's contacts. Nothing asked for can put one back in.
 */
bool mesh_radio_profile_part_allowed(uint8_t protocol, uint8_t topic);

/*
 * The parts `backup` has to offer, in the order a picker lists them. How many, or -EPROTO for a
 * protocol with no profiles, -EBADMSG for a backup that does not read.
 */
int mesh_radio_profile_offer(const struct mesh_radio_backup *backup,
                             struct mesh_radio_profile_part *out, size_t max);

/*
 * A profile named `name` out of `backup`, carrying the parts in `parts` it has and may carry. The
 * header keeps what the backup was taken from - model, firmware - and the LoRa numbers and
 * channel names only when those parts are in it; the time is the caller's.
 *
 * 0; -EINVAL for no name or no part left once the identity is taken out; -EPROTO, -EBADMSG,
 * -ENOSPC from the container.
 */
int mesh_radio_profile_make(const struct mesh_radio_backup *backup,
                            const struct mesh_radio_backup_parts *parts, const char *name,
                            struct mesh_radio_backup *out);

/*
 * What differs between `profile` and the radio captured into `live`, over the profile's parts
 * only: `live` is cut down the way the profile was before the two are compared, so a module the
 * profile leaves out is not "only on the radio" and a MeshCore radio's name is not a change.
 *
 * 0; -EPROTO when `live` is another protocol's (the cross-protocol refusal); -EINVAL for a
 * `profile` that is not one; -ENOMEM; the protocol diff's own errors.
 */
int mesh_radio_profile_diff(const struct mesh_radio_backup *profile,
                            const struct mesh_radio_backup *live,
                            struct mesh_radio_backup_diff *out);

/* ---- the store ----------------------------------------------------------------------------- */

/*
 * One flat directory: `00000003.profile` per profile, by sequence number, and whatever `.cfg`
 * files somebody copied there or an export wrote. Disabled, like the backup store, when the
 * directory cannot be made, and every call is then a quiet -ENODEV.
 */
struct mesh_radio_profile_store {
    char dir[MESH_RADIO_BACKUP_PATH_MAX];
    bool enabled;
};

int mesh_radio_profile_store_init(struct mesh_radio_profile_store *store, const char *dir);
bool mesh_radio_profile_store_enabled(const struct mesh_radio_profile_store *store);

/* Saves a profile as the next in sequence. 0 and the sequence in *sequence, or a negative errno;
   -EINVAL for a backup that is not a profile. */
int mesh_radio_profile_store_save(struct mesh_radio_profile_store *store,
                                  const struct mesh_radio_backup *profile, uint32_t *sequence);

/* The profiles' sequence numbers, oldest first, up to `max`; how many there are in all, or a
   negative errno. */
int mesh_radio_profile_store_list(const struct mesh_radio_profile_store *store, uint32_t *out,
                                  size_t max);

/* Reads one back (mesh_radio_backup_read_file()'s errors), refusing a file that is a backup. */
int mesh_radio_profile_store_load(const struct mesh_radio_profile_store *store, uint32_t sequence,
                                  struct mesh_radio_backup *profile);

int mesh_radio_profile_store_remove(struct mesh_radio_profile_store *store, uint32_t sequence);

/*
 * The `.cfg` files in the directory, by file name, sorted, up to `max`; how many in all, or a
 * negative errno. A name is at most MESH_RADIO_PROFILE_FILE_MAX - 1 bytes; a longer one is not
 * listed, since it could not be named back to open it.
 */
int mesh_radio_profile_store_cfgs(const struct mesh_radio_profile_store *store,
                                  char (*out)[MESH_RADIO_PROFILE_FILE_MAX], size_t max);

/*
 * The path of `file` in the directory. False for a name that is not a plain file name - a slash,
 * a leading dot - so a name that came off a screen cannot reach outside it.
 */
bool mesh_radio_profile_store_path(const struct mesh_radio_profile_store *store, const char *file,
                                   char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
