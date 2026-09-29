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
#include "mesh/core/radio_backup_diff.h"

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

/*
 * The fields a MeshCore comparison names (struct mesh_radio_backup_change's `field`). Numbers of
 * this client's choosing - the companion protocol numbers none of them - and never written to a
 * file, so they are free to move.
 */
enum mesh_meshcore_backup_field {
    MESH_MESHCORE_BACKUP_FIELD_NONE = 0,
    MESH_MESHCORE_BACKUP_FIELD_NAME,
    MESH_MESHCORE_BACKUP_FIELD_FREQUENCY, /* kHz */
    MESH_MESHCORE_BACKUP_FIELD_BANDWIDTH, /* Hz */
    MESH_MESHCORE_BACKUP_FIELD_SPREADING,
    MESH_MESHCORE_BACKUP_FIELD_CODING,
    MESH_MESHCORE_BACKUP_FIELD_TX_POWER, /* dBm */
    MESH_MESHCORE_BACKUP_FIELD_LATITUDE,
    MESH_MESHCORE_BACKUP_FIELD_LONGITUDE,
    MESH_MESHCORE_BACKUP_FIELD_ADVERT_LOCATION,
    MESH_MESHCORE_BACKUP_FIELD_MANUAL_ADD,
    MESH_MESHCORE_BACKUP_FIELD_TELEMETRY,
    MESH_MESHCORE_BACKUP_FIELD_MULTI_ACKS,
    MESH_MESHCORE_BACKUP_FIELD_PUBLIC_KEY,
    MESH_MESHCORE_BACKUP_FIELD_PIN,
    MESH_MESHCORE_BACKUP_FIELD_CHANNEL_NAME,
    MESH_MESHCORE_BACKUP_FIELD_CHANNEL_SECRET,
    MESH_MESHCORE_BACKUP_FIELD_CONTACT_TYPE,
    MESH_MESHCORE_BACKUP_FIELD_CONTACT_FLAGS,
    MESH_MESHCORE_BACKUP_FIELD_ADV_TYPE, /* what it advertises itself as: chat, repeater, room */
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

/*
 * What differs between two MeshCore backups: the settings in the groups a screen shows them in
 * (the name under the owner, the four radio numbers and the power under LoRa, the position, the
 * other bytes, the PIN), each channel slot, and then the contacts - added, removed, or changed in
 * name, type or flags, matched by public key.
 *
 * A contact's route and its timestamps are not compared. Both move whenever the radio hears the
 * contact again, so a comparison that counted them would call every contact changed a day
 * later, and the point of the list is what somebody changed.
 *
 * 0, -EPROTO when either backup is not MeshCore's, -EBADMSG for one that does not read, -ENOMEM.
 */
int mesh_meshcore_backup_diff(const struct mesh_radio_backup *a, const struct mesh_radio_backup *b,
                              struct mesh_radio_backup_diff *out);

/* The most saves a plan is: the settings, then each channel slot on its own. */
#define MESH_MESHCORE_BACKUP_PLAN_MAX (1U + MESH_MESHCORE_CHANNELS_KEPT)

/*
 * A restore of `backup` onto the radio `meshcore` holds, as the saves that would make it so:
 * `writes[0]` carries every settings group that differs, when one does, and each channel slot
 * that differs follows as a save of its own - a save takes one slot, and one save at a time is
 * all the conversation will have outstanding, so the caller sends the next as the last is
 * answered. A group is written whole with the backup's values, and only when some part of it
 * differs: a radio already matching the backup is sent nothing.
 *
 * **What cannot be written is counted, not sent.** A value outside the firmware's bounds -
 * radio parameters it no longer accepts, a power past what this radio reports it can do, a PIN
 * outside the range - would be refused anyway, and a group is never quietly written with a
 * value other than the backup's. Nor can the advert type, which a companion radio fixes itself,
 * a slot this radio does not have, or - not yet - a contact. Each such difference adds one to
 * `unwritable`; the comparison after the restore is where they show.
 *
 * How many saves were planned (0 when nothing writable differs), -EAGAIN before the radio has
 * finished syncing, -ENODEV for a backup of another radio (its public key is not this one's),
 * -EPROTO for a backup of another protocol, -EBADMSG for one that does not read, -ENOSPC when
 * `max` is short of what the plan needs, -ENOMEM.
 */
int mesh_meshcore_backup_plan(const struct mesh_radio_backup *backup,
                              const struct mesh_meshcore *meshcore,
                              struct mesh_meshcore_settings_write *writes, size_t max,
                              size_t *unwritable);

#ifdef __cplusplus
}
#endif
