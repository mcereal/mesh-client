#pragma once

/*
 * A MeshCore companion radio into a backup's sections, and back.
 *
 * **Most of what makes a MeshCore radio yours is its contact list.** Its settings are a dozen
 * numbers - a name, the four radio parameters, a power, a position, the four "other" bytes, a
 * Bluetooth PIN - and its channels are a name and a secret each. Its contacts are the keys it
 * trusts, the routes it has learned to each, and which ones are favourites: state the radio
 * keeps, that a factory reset or a reflash erases, and that comes back only as adverts are
 * heard again one by one. So a MeshCore backup is mostly contacts.
 *
 * **Each contact is kept as the radio sends it**: the 148-byte RESP_CONTACT record, route and
 * stamps included, so the record read back is the one mesh_meshcore_decode_contact() already
 * reads. Whether a restore keeps those routes is the restore's question, not the backup's.
 *
 * **The radio's own public key is kept; its private key cannot be.** The companion protocol
 * does not report a private key in anything this client asks for, and the public one is how a
 * restore will know it is talking to the same radio.
 *
 * **Only a radio that has finished syncing is captured**: the device query, SELF_INFO, the
 * whole contact list and every channel slot. Before that the contact book is a list being read,
 * and a backup of it would be a radio that had fewer contacts than it has.
 */

#include "mesh/core/meshcore.h"
#include "mesh/core/radio_backup.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The section tags. Numbers, because they are written into files and must never move. */
enum mesh_meshcore_backup_tag {
    MESH_MESHCORE_BACKUP_SELF = 1,    /* the radio's settings; see meshcore_backup.c */
    MESH_MESHCORE_BACKUP_PIN = 2,     /* the Bluetooth PIN, u32 little-endian */
    MESH_MESHCORE_BACKUP_CHANNEL = 3, /* slot, name[32], secret[16] */
    MESH_MESHCORE_BACKUP_CONTACT = 4, /* a RESP_CONTACT record, 148 bytes */
};

/* What a MeshCore backup holds, decoded. About sixty kilobytes: never on a stack. */
struct mesh_meshcore_backup_contents {
    bool has_self;
    /* SELF_INFO's settings and the radio's public key; `max_tx_power_dbm` is not kept. */
    struct mesh_meshcore_self_info self;
    bool has_pin;
    uint32_t ble_pin;
    bool has_channel[MESH_MESHCORE_CHANNELS_KEPT];
    struct mesh_meshcore_channel channels[MESH_MESHCORE_CHANNELS_KEPT];
    size_t contact_count;
    struct mesh_meshcore_contact contacts[MESH_MESHCORE_CONTACTS_MAX];
};

/* Whether the conversation has finished reading the radio: ready, with its identity known. */
bool mesh_meshcore_backup_ready(const struct mesh_meshcore *meshcore);

/*
 * Fills `backup` - header and sections - from what the conversation holds about the radio. The
 * caller sets the reason, the time and the device.
 *
 * 0, -EAGAIN before the sync has finished, -EOVERFLOW when the radio has more contacts than the
 * book could keep, -ENOSPC or -EINVAL from the container. `backup` is reset first either way.
 */
int mesh_meshcore_backup_capture(const struct mesh_meshcore *meshcore,
                                 struct mesh_radio_backup *backup);

/*
 * The other direction. 0, -EPROTO for a backup of another protocol, -EBADMSG for a section of
 * the wrong shape. Tags this build does not know are skipped; contacts past what `out` holds
 * are dropped.
 */
int mesh_meshcore_backup_read(const struct mesh_radio_backup *backup,
                              struct mesh_meshcore_backup_contents *out);

#ifdef __cplusplus
}
#endif
