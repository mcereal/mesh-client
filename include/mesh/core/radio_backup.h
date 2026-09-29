#pragma once

/*
 * A radio's configuration kept on the card: one file per backup, one directory per radio.
 *
 * **Two halves, and only one of them knows a protocol.** A backup is a header every screen can
 * read - which radio, when, why, what it was running, the four radio numbers, the channel names,
 * how many nodes it knew - and a payload of tagged sections that only the protocol that wrote
 * them can read back. This file is the container and the store; what goes into the sections is
 * mesh/core/radio_backup_meshtastic.h and mesh/core/meshcore_backup.h. So a list of backups, a
 * detail screen and a prune never ask which firmware a radio runs, and a third protocol is a
 * third capture rather than a change here.
 *
 * **Not Meshtastic's DeviceProfile.** That message is the one Meshtastic's own apps export, and
 * it has no room for a MeshCore radio - nor for a contact list, which on MeshCore is the part of
 * a radio most worth keeping. It is an import and export format over the Meshtastic sections
 * here, not the shape of the file.
 *
 * **Text, not bytes.** The file is inkwell's key=value record, a section's bytes in hex. Twice
 * the size of a binary file - a Meshtastic backup is a few kilobytes, a MeshCore one with a full
 * contact list around a hundred - bought for three things: it is written through
 * inkwell_record_replace(), which is the one atomic, synced replace the stack has; a binary file
 * would need the text-mode-on-Windows question answered a second time; and a person can open
 * one and see what their radio was set to.
 *
 * **A backup either reads whole or not at all.** The last line is a SHA-256 over every record
 * before it, so a file cut short by a pulled battery - the handheld's normal way of stopping -
 * is refused rather than read as a radio with fewer channels than it had. A newer format number
 * is refused for the same reason: a field this build does not know may change what the ones it
 * does know mean. Unknown keys inside a known format are skipped, which is how a field is added
 * without a new format.
 *
 * **A private key only when somebody asked for it by name.** Every capture leaves the radio's key
 * out; a backup taken from the "with identity key" press carries it as one section of its own,
 * MESH_RADIO_BACKUP_IDENTITY, which no protocol's reader, comparison, plan or profile knows - so
 * a keyed backup is an ordinary one to everything but the two calls that put the key back. The
 * automatic backups never carry one: they are written without anybody watching, to a card
 * anyone can copy, and the key is the radio's identity on the mesh. A profile never carries one,
 * whatever made it: mesh_radio_backup_write_file() refuses a profile with the section in it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What `format=` a file this build writes carries, and the newest it will read. */
#define MESH_RADIO_BACKUP_FORMAT 1U

/*
 * Capacity. A section is one protobuf or one record - the largest today is a Meshtastic
 * ModuleConfig at 246 bytes - so 512 leaves room without letting a line grow unbounded. The
 * section count and the payload are sized for a MeshCore radio with a full contact list: the
 * protocol cannot report more than 510 contacts of 148 bytes each, and the store is given room
 * for the contact book's 512 (meshcore_backup.c checks the sum at compile time).
 */
#define MESH_RADIO_BACKUP_SECTION_MAX 512U
#define MESH_RADIO_BACKUP_SECTIONS_MAX 640U
#define MESH_RADIO_BACKUP_PAYLOAD_MAX (96U * 1024U)
#define MESH_RADIO_BACKUP_CHANNELS 8U
#define MESH_RADIO_BACKUP_CHANNEL_NAME 33U
#define MESH_RADIO_BACKUP_TEXT 48U
#define MESH_RADIO_BACKUP_DEVICE 64U
#define MESH_RADIO_BACKUP_PATH_MAX 512U
/*
 * The section a radio's private key rides in, the same number under either protocol, and above
 * every protocol's own tags so it is never mistaken for one. Its bytes are the key as that
 * protocol's radio takes it back: Meshtastic's 32-byte X25519 key, MeshCore's 64-byte Ed25519
 * one. An older build reads it as a tag it does not know, and skips it.
 */
#define MESH_RADIO_BACKUP_IDENTITY 0x100U

/* How many automatic backups one radio keeps; the oldest goes first. Manual ones are never
   pruned: somebody asked for each of them by name. Nor is the first-connect one, which is never
   taken twice. */
#define MESH_RADIO_BACKUP_KEEP_AUTOMATIC 10U

enum mesh_radio_backup_protocol {
    MESH_RADIO_BACKUP_PROTOCOL_NONE = 0,
    MESH_RADIO_BACKUP_MESHTASTIC = 1,
    MESH_RADIO_BACKUP_MESHCORE = 2,
};

/* Why a backup was taken. Written into the file by name, so the numbers are free to move. */
enum mesh_radio_backup_reason {
    MESH_RADIO_BACKUP_REASON_NONE = 0,
    /* Somebody pressed for it. A prune never removes one. */
    MESH_RADIO_BACKUP_MANUAL = 1,
    /* The first time this client finished reading this radio. Taken only when the radio has no
       backup, so it is never taken again - and a prune never removes one either. */
    MESH_RADIO_BACKUP_FIRST_CONNECT = 2,
    /* The settings as they stood just before this client wrote to them. */
    MESH_RADIO_BACKUP_BEFORE_WRITE = 3,
    /* Just before a firmware install or a switch to the other firmware, which erases it. */
    MESH_RADIO_BACKUP_BEFORE_FIRMWARE = 4,
    /*
     * Not a backup of a radio but a profile: settings to put on any radio of the protocol, with
     * who the radio was taken out (mesh/core/radio_profile.h). Its node is 0 and its name is the
     * profile's. It lives in a directory of its own, so no prune ever sees one.
     */
    MESH_RADIO_BACKUP_PROFILE = 5,
};

/*
 * Which parts of a radio a profile carries, by the topics a comparison names them with
 * (mesh/core/radio_backup_diff.h): a bit per `enum mesh_radio_backup_topic`, and for Meshtastic's
 * modules a bit per ModuleConfig variant as well, since "MQTT and nothing else" is a profile
 * somebody wants. The TOPIC_MODULE bit is set whenever a module bit is.
 */
struct mesh_radio_backup_parts {
    uint32_t topics;
    uint32_t modules;
};

/*
 * What any screen can say about a backup without knowing its protocol.
 *
 * `nodes_heard` and `contacts` are two questions, not one number under two names. A Meshtastic
 * radio's node list is whatever it has heard lately and rebuilds itself from the air; a MeshCore
 * radio's contact list is what it *keeps*, and is gone after a reset. A backup carries whichever
 * its protocol has; `has_` says which.
 */
struct mesh_radio_backup_header {
    uint8_t protocol; /* enum mesh_radio_backup_protocol */
    uint8_t reason;   /* enum mesh_radio_backup_reason */
    /* Wall-clock seconds when it was taken, 0 on a device that had no credible clock - the
       file's sequence number, not this, is what orders a radio's backups. */
    uint32_t saved_at;
    /* The radio's node number: its hardware's on Meshtastic, the front of its key on MeshCore.
       Changes when a radio switches firmware, which is why `device` is kept too. */
    uint32_t node_id;
    /* The link it was on - a BLE address, a serial port, a host - so a device's history can
       be followed across a switch of firmware. Empty when unknown. */
    char device[MESH_RADIO_BACKUP_DEVICE];
    char name[MESH_RADIO_BACKUP_TEXT];     /* the radio's long name */
    char model[MESH_RADIO_BACKUP_TEXT];    /* board, as the firmware names it */
    char firmware[MESH_RADIO_BACKUP_TEXT]; /* version string */
    /* Meshtastic's region and modem preset, by the names the firmware gives them; empty on a
       protocol that has neither, and `preset` empty when the numbers below are set by hand. */
    char region[MESH_RADIO_BACKUP_TEXT];
    char preset[MESH_RADIO_BACKUP_TEXT];
    bool has_radio;
    uint32_t frequency_khz; /* 0 when a Meshtastic radio uses its region's default slot */
    uint32_t bandwidth_hz;  /* 0, with the two below, when a preset sets them */
    uint8_t spreading_factor;
    uint8_t coding_rate;
    int8_t tx_power_dbm;
    uint8_t channel_count;
    char channel_names[MESH_RADIO_BACKUP_CHANNELS][MESH_RADIO_BACKUP_CHANNEL_NAME];
    bool has_nodes_heard;
    uint32_t nodes_heard;
    bool has_contacts;
    uint32_t contacts;
    /* A profile's parts; zero on a backup, which carries every part its radio had. */
    struct mesh_radio_backup_parts parts;
    /* Whether it carries the radio's private key (MESH_RADIO_BACKUP_IDENTITY). Not a line in the
       file: it is set whenever that section is added or read, so it cannot say otherwise. */
    bool has_identity;
};

/* One section: a protocol's tag, and where its bytes sit in the payload. */
struct mesh_radio_backup_section {
    uint16_t tag;
    uint16_t len;
    uint32_t offset;
};

/*
 * A whole backup in memory. About a hundred kilobytes, so it is never on a stack: the app holds
 * one, and a test declares its own static.
 */
struct mesh_radio_backup {
    struct mesh_radio_backup_header header;
    size_t section_count;
    size_t used;
    struct mesh_radio_backup_section sections[MESH_RADIO_BACKUP_SECTIONS_MAX];
    uint8_t payload[MESH_RADIO_BACKUP_PAYLOAD_MAX];
};

void mesh_radio_backup_reset(struct mesh_radio_backup *backup);

/*
 * Appends one section. 0, -EINVAL for an oversized one, -ENOSPC when full.
 *
 * An empty section is a section: a protobuf left at every default encodes to nothing, and "the
 * radio had this, all defaults" is not "the radio never said".
 */
int mesh_radio_backup_add(struct mesh_radio_backup *backup, uint16_t tag, const void *data,
                          size_t len);

/* The section at `index`, with its bytes in *data; NULL past the end. */
const struct mesh_radio_backup_section *
mesh_radio_backup_section_at(const struct mesh_radio_backup *backup, size_t index,
                             const uint8_t **data);

/* How many sections carry `tag`. */
size_t mesh_radio_backup_count_tag(const struct mesh_radio_backup *backup, uint16_t tag);

/* The private key a backup carries, its length (0 for none) with its bytes in *data. */
size_t mesh_radio_backup_identity(const struct mesh_radio_backup *backup, const uint8_t **data);

/* Overwrites the payload and resets the backup: for one that held a key, before it is freed. */
void mesh_radio_backup_wipe(struct mesh_radio_backup *backup);

/* Whether two backups carry the same sections in the same order - the header aside, which
   always differs by its time. How an automatic backup is skipped when nothing has changed.
   A private key is not a setting and is left out of the question: the automatic backup after
   a keyed one is still skipped when nothing else moved. */
bool mesh_radio_backup_same_payload(const struct mesh_radio_backup *a,
                                    const struct mesh_radio_backup *b);

const char *mesh_radio_backup_reason_key(uint8_t reason);
const char *mesh_radio_backup_protocol_key(uint8_t protocol);

/*
 * Writes `backup` to `path` through a temporary, synced, then renamed over it. 0 or a negative
 * errno; -EINVAL for a backup with no protocol or no node - a profile, which has no node, aside -
 * and -EPERM for a profile carrying a private key.
 */
int mesh_radio_backup_write_file(const struct mesh_radio_backup *backup, const char *path);

/*
 * Reads a file back. 0, or:
 *   -ENOENT   no such file
 *   -EBADMSG  not a backup, cut short, or its digest does not match - or a profile with a key
 *   -EPROTO   a format newer than MESH_RADIO_BACKUP_FORMAT
 * `backup` is left reset on any failure: a half-read one is never handed back.
 */
int mesh_radio_backup_read_file(struct mesh_radio_backup *backup, const char *path);

/* ---- the store ----------------------------------------------------------------------------- */

/*
 * The directory of backups, one subdirectory per node number (eight hex digits), one file per
 * backup named by a sequence number and its reason: `00000007.before_write.backup`.
 *
 * A sequence rather than a time, because the handheld this runs on often has no clock worth the
 * name: two backups taken in 1970 still sort in the order they were taken. The reason is in the
 * name so a list and a prune need not open every file.
 *
 * A store whose directory cannot be made is disabled, and every call on it is a quiet -ENODEV:
 * a client that cannot keep backups is still a client.
 */
struct mesh_radio_backup_store {
    char dir[MESH_RADIO_BACKUP_PATH_MAX];
    bool enabled;
    unsigned keep_automatic;
    /* One backup a prune must leave alone whatever its age, 0 for none: the one being
       restored, which the safety copy taken just before the restore would otherwise be the
       very save that pushes it out. Set and cleared by whoever is using it. */
    uint32_t protect_node;
    uint32_t protect_sequence;
};

/* One file in a radio's directory, as its name describes it. */
struct mesh_radio_backup_entry {
    uint32_t sequence;
    uint8_t reason;
    char file[64];
};

/* 0, or a negative errno with the store left disabled. */
int mesh_radio_backup_store_init(struct mesh_radio_backup_store *store, const char *dir);

bool mesh_radio_backup_store_enabled(const struct mesh_radio_backup_store *store);

/*
 * Saves `backup` under its node as the next file in sequence, then prunes that node's automatic
 * backups down to `keep_automatic`. Returns 0, or a negative errno. `entry`, when not NULL, is
 * what was written.
 */
int mesh_radio_backup_store_save(struct mesh_radio_backup_store *store,
                                 const struct mesh_radio_backup *backup,
                                 struct mesh_radio_backup_entry *entry);

/*
 * A node's backups, newest first, up to `max`. Returns how many there are in all (which may be
 * more than `max`), or a negative errno. A node with none is 0. A list cut short at two or more
 * still holds the first-connect backup, last: the prune keeps it, so the list must too.
 */
int mesh_radio_backup_store_list(const struct mesh_radio_backup_store *store, uint32_t node_id,
                                 struct mesh_radio_backup_entry *out, size_t max);

/*
 * The radios with backups on the card, by node number, in no particular order: up to `max` of
 * them, returning how many there are in all or a negative errno. A directory with nothing a
 * backup is named like in it is not a radio.
 */
int mesh_radio_backup_store_radios(const struct mesh_radio_backup_store *store, uint32_t *out,
                                   size_t max);

/*
 * Deletes one backup, named by its node and sequence number rather than by a file name, so a
 * caller cannot be talked into removing anything else in the directory. 0, -ENOENT when there is
 * no such backup, or a negative errno.
 */
int mesh_radio_backup_store_remove(struct mesh_radio_backup_store *store, uint32_t node_id,
                                   uint32_t sequence);

/* Reads one of them back; the errors are mesh_radio_backup_read_file()'s. */
int mesh_radio_backup_store_load(const struct mesh_radio_backup_store *store, uint32_t node_id,
                                 const struct mesh_radio_backup_entry *entry,
                                 struct mesh_radio_backup *backup);

#ifdef __cplusplus
}
#endif
