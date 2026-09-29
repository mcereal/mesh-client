/*
 * Profiles: the parts, the making, the comparison and the directory. See
 * include/mesh/core/radio_profile.h.
 */

#include "mesh/core/radio_profile.h"

#include "inkwell/base/file.h"
#include "inkwell/base/text.h"
#include "mesh/core/meshcore_backup.h"
#include "mesh/core/radio_backup_meshtastic.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RADIO_PROFILE_SUFFIX ".profile"
#define RADIO_PROFILE_CFG ".cfg"

_Static_assert(MESH_RADIO_BACKUP_TOPIC_COUNT <= 32U, "a topic is a bit of a uint32_t");

bool mesh_radio_profile_parts_has(const struct mesh_radio_backup_parts *parts, uint8_t topic,
                                  uint16_t index) {
    if (parts == NULL || topic >= MESH_RADIO_BACKUP_TOPIC_COUNT) {
        return false;
    }
    if (topic == MESH_RADIO_BACKUP_TOPIC_MODULE) {
        return index < 32U && (parts->modules & (1U << index)) != 0U;
    }
    return (parts->topics & (1U << topic)) != 0U;
}

void mesh_radio_profile_parts_set(struct mesh_radio_backup_parts *parts, uint8_t topic,
                                  uint16_t index, bool on) {
    if (parts == NULL || topic >= MESH_RADIO_BACKUP_TOPIC_COUNT) {
        return;
    }
    if (topic == MESH_RADIO_BACKUP_TOPIC_MODULE) {
        if (index >= 32U) {
            return;
        }
        parts->modules = on ? parts->modules | (1U << index) : parts->modules & ~(1U << index);
        /* The topic's own bit says "some module", which is what a heading asks. */
        on = parts->modules != 0U;
    }
    parts->topics = on ? parts->topics | (1U << topic) : parts->topics & ~(1U << topic);
}

bool mesh_radio_profile_part_allowed(uint8_t protocol, uint8_t topic) {
    switch (protocol) {
    case MESH_RADIO_BACKUP_MESHTASTIC:
        switch ((enum mesh_radio_backup_topic)topic) {
        case MESH_RADIO_BACKUP_TOPIC_DEVICE:
        case MESH_RADIO_BACKUP_TOPIC_POSITION:
        case MESH_RADIO_BACKUP_TOPIC_POWER:
        case MESH_RADIO_BACKUP_TOPIC_NETWORK:
        case MESH_RADIO_BACKUP_TOPIC_DISPLAY:
        case MESH_RADIO_BACKUP_TOPIC_LORA:
        case MESH_RADIO_BACKUP_TOPIC_BLUETOOTH:
        case MESH_RADIO_BACKUP_TOPIC_MODULE:
        case MESH_RADIO_BACKUP_TOPIC_CHANNEL:
        case MESH_RADIO_BACKUP_TOPIC_RADIO_UI:
        case MESH_RADIO_BACKUP_TOPIC_CANNED:
        case MESH_RADIO_BACKUP_TOPIC_RINGTONE:
            return true;
        default:
            /* The owner, the fixed position and the Security section's keys - which are most of
               what that section is, and the rest of it is whether this one radio is managed. */
            return false;
        }
    case MESH_RADIO_BACKUP_MESHCORE:
        /* The radio numbers and power, the other settings, the PIN and the channels; its name,
           key, position and contacts are the radio. */
        return topic == MESH_RADIO_BACKUP_TOPIC_LORA || topic == MESH_RADIO_BACKUP_TOPIC_DEVICE ||
               topic == MESH_RADIO_BACKUP_TOPIC_BLUETOOTH ||
               topic == MESH_RADIO_BACKUP_TOPIC_CHANNEL;
    default:
        return false;
    }
}

int mesh_radio_profile_offer(const struct mesh_radio_backup *backup,
                             struct mesh_radio_profile_part *out, size_t max) {
    if (backup == NULL) {
        return -EINVAL;
    }
    switch (backup->header.protocol) {
    case MESH_RADIO_BACKUP_MESHTASTIC:
        return mesh_radio_backup_meshtastic_offer(backup, out, max);
    case MESH_RADIO_BACKUP_MESHCORE:
        return mesh_meshcore_backup_offer(backup, out, max);
    default:
        return -EPROTO;
    }
}

/* `backup` cut down to `parts`, by its protocol. */
static int radio_profile_keep(const struct mesh_radio_backup *backup,
                              const struct mesh_radio_backup_parts *parts,
                              struct mesh_radio_backup *out) {
    switch (backup->header.protocol) {
    case MESH_RADIO_BACKUP_MESHTASTIC:
        return mesh_radio_backup_meshtastic_keep(backup, parts, out);
    case MESH_RADIO_BACKUP_MESHCORE:
        return mesh_meshcore_backup_keep(backup, parts, out);
    default:
        return -EPROTO;
    }
}

int mesh_radio_profile_make(const struct mesh_radio_backup *backup,
                            const struct mesh_radio_backup_parts *parts, const char *name,
                            struct mesh_radio_backup *out) {
    if (backup == NULL || parts == NULL || name == NULL || name[0] == '\0' || out == NULL ||
        backup == out) {
        return -EINVAL;
    }
    /* The parts asked for that the backup has and a profile may carry: the header says exactly
       what is in the file, not what was ticked. */
    struct mesh_radio_profile_part offer[MESH_RADIO_PROFILE_PARTS_MAX];
    const int offered = mesh_radio_profile_offer(backup, offer, MESH_RADIO_PROFILE_PARTS_MAX);
    if (offered < 0) {
        return offered;
    }
    struct mesh_radio_backup_parts kept = {0};
    for (size_t i = 0; i < (size_t)offered && i < MESH_RADIO_PROFILE_PARTS_MAX; ++i) {
        if (mesh_radio_profile_parts_has(parts, offer[i].topic, offer[i].index)) {
            mesh_radio_profile_parts_set(&kept, offer[i].topic, offer[i].index, true);
        }
    }
    if (kept.topics == 0U) {
        return -EINVAL;
    }
    const int result = radio_profile_keep(backup, &kept, out);
    if (result != 0) {
        return result;
    }
    struct mesh_radio_backup_header *header = &out->header;
    header->reason = MESH_RADIO_BACKUP_PROFILE;
    header->node_id = 0U;
    header->saved_at = 0U;
    header->device[0] = '\0';
    inkwell_str_copy(header->name, sizeof header->name, name);
    header->has_nodes_heard = false;
    header->nodes_heard = 0U;
    header->has_contacts = false;
    header->contacts = 0U;
    header->parts = kept;
    header->has_identity = false;
    if (!mesh_radio_profile_parts_has(&kept, MESH_RADIO_BACKUP_TOPIC_LORA, 0U)) {
        header->has_radio = false;
        header->frequency_khz = 0U;
        header->bandwidth_hz = 0U;
        header->spreading_factor = 0U;
        header->coding_rate = 0U;
        header->tx_power_dbm = 0;
        header->region[0] = '\0';
        header->preset[0] = '\0';
    }
    if (!mesh_radio_profile_parts_has(&kept, MESH_RADIO_BACKUP_TOPIC_CHANNEL, 0U)) {
        header->channel_count = 0U;
        memset(header->channel_names, 0, sizeof header->channel_names);
    }
    return 0;
}

int mesh_radio_profile_diff(const struct mesh_radio_backup *profile,
                            const struct mesh_radio_backup *live,
                            struct mesh_radio_backup_diff *out) {
    if (profile == NULL || live == NULL || out == NULL ||
        profile->header.reason != MESH_RADIO_BACKUP_PROFILE) {
        return -EINVAL;
    }
    mesh_radio_backup_diff_reset(out, profile->header.protocol);
    if (live->header.protocol != profile->header.protocol) {
        return -EPROTO;
    }
    struct mesh_radio_backup *masked = malloc(sizeof *masked);
    if (masked == NULL) {
        return -ENOMEM;
    }
    int result = radio_profile_keep(live, &profile->header.parts, masked);
    if (result == 0) {
        result = profile->header.protocol == MESH_RADIO_BACKUP_MESHCORE
                     ? mesh_meshcore_backup_diff(profile, masked, out)
                     : mesh_radio_backup_meshtastic_diff(profile, masked, out);
    }
    free(masked);
    return result;
}

/* ---- the store ----------------------------------------------------------------------------- */

int mesh_radio_profile_store_init(struct mesh_radio_profile_store *store, const char *dir) {
    if (store == NULL) {
        return -EINVAL;
    }
    memset(store, 0, sizeof *store);
    if (dir == NULL || dir[0] == '\0') {
        return -EINVAL;
    }
    const int written = snprintf(store->dir, sizeof store->dir, "%s", dir);
    if (written < 0 || (size_t)written + 1U + MESH_RADIO_PROFILE_FILE_MAX >= sizeof store->dir) {
        store->dir[0] = '\0';
        return -ENAMETOOLONG;
    }
    const int made = inkwell_file_mkdir(store->dir);
    if (made != 0 && !(made == -EEXIST && inkwell_file_is_dir(store->dir))) {
        store->dir[0] = '\0';
        return made == -EEXIST ? -ENOTDIR : made;
    }
    store->enabled = true;
    return 0;
}

bool mesh_radio_profile_store_enabled(const struct mesh_radio_profile_store *store) {
    return store != NULL && store->enabled;
}

bool mesh_radio_profile_store_path(const struct mesh_radio_profile_store *store, const char *file,
                                   char *out, size_t out_len) {
    if (!mesh_radio_profile_store_enabled(store) || file == NULL || out == NULL ||
        file[0] == '\0' || file[0] == '.' || strchr(file, '/') != NULL ||
        strchr(file, '\\') != NULL) {
        return false;
    }
    const int written = snprintf(out, out_len, "%s/%s", store->dir, file);
    return written > 0 && (size_t)written < out_len;
}

/* "00000003.profile" into its sequence; false for anything else in the directory. */
static bool radio_profile_parse_name(const char *name, uint32_t *sequence) {
    const size_t suffix = sizeof RADIO_PROFILE_SUFFIX - 1U;
    if (strlen(name) != 8U + suffix || strcmp(name + 8, RADIO_PROFILE_SUFFIX) != 0) {
        return false;
    }
    uint32_t value = 0U;
    for (size_t i = 0; i < 8U; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        value = value * 10U + (uint32_t)(name[i] - '0');
    }
    *sequence = value;
    return value != 0U;
}

struct radio_profile_listing {
    uint32_t *out;
    size_t max;
    size_t total;
    uint32_t highest;
};

static void radio_profile_collect(void *context, const char *name) {
    struct radio_profile_listing *listing = context;
    uint32_t sequence = 0U;
    if (!radio_profile_parse_name(name, &sequence)) {
        return;
    }
    /*
     * The directory hands names over in no order, so a full list keeps the newest it has seen
     * rather than the first: taking the first would drop a profile just saved, and it would
     * vanish from the screen the moment it was made.
     */
    const size_t kept = listing->total < listing->max ? listing->total : listing->max;
    if (kept < listing->max) {
        listing->out[kept] = sequence;
    } else if (listing->max > 0U) {
        size_t oldest = 0U;
        for (size_t i = 1; i < kept; ++i) {
            if (listing->out[i] < listing->out[oldest]) {
                oldest = i;
            }
        }
        if (sequence > listing->out[oldest]) {
            listing->out[oldest] = sequence;
        }
    }
    ++listing->total;
    if (sequence > listing->highest) {
        listing->highest = sequence;
    }
}

static int radio_profile_ascending(const void *a, const void *b) {
    const uint32_t left = *(const uint32_t *)a;
    const uint32_t right = *(const uint32_t *)b;
    return left < right ? -1 : left > right ? 1 : 0;
}

int mesh_radio_profile_store_list(const struct mesh_radio_profile_store *store, uint32_t *out,
                                  size_t max) {
    if (!mesh_radio_profile_store_enabled(store)) {
        return -ENODEV;
    }
    if (out == NULL && max > 0U) {
        return -EINVAL;
    }
    struct radio_profile_listing listing = {.out = out, .max = max};
    const int result = inkwell_file_list(store->dir, radio_profile_collect, &listing);
    if (result < 0) {
        return result;
    }
    const size_t kept = listing.total < max ? listing.total : max;
    if (kept > 1U) {
        qsort(out, kept, sizeof *out, radio_profile_ascending);
    }
    return (int)listing.total;
}

static void radio_profile_file(uint32_t sequence, char *out, size_t out_len) {
    snprintf(out, out_len, "%08" PRIu32 RADIO_PROFILE_SUFFIX, sequence);
}

int mesh_radio_profile_store_save(struct mesh_radio_profile_store *store,
                                  const struct mesh_radio_backup *profile, uint32_t *sequence) {
    if (!mesh_radio_profile_store_enabled(store)) {
        return -ENODEV;
    }
    if (profile == NULL || profile->header.reason != MESH_RADIO_BACKUP_PROFILE) {
        return -EINVAL;
    }
    struct radio_profile_listing listing = {0};
    const int listed = inkwell_file_list(store->dir, radio_profile_collect, &listing);
    if (listed < 0) {
        return listed;
    }
    if (listing.highest >= 99999999U) {
        return -EOVERFLOW;
    }
    const uint32_t next = listing.highest + 1U;
    char file[MESH_RADIO_PROFILE_FILE_MAX];
    radio_profile_file(next, file, sizeof file);
    char path[MESH_RADIO_BACKUP_PATH_MAX + MESH_RADIO_PROFILE_FILE_MAX];
    if (!mesh_radio_profile_store_path(store, file, path, sizeof path)) {
        return -ENAMETOOLONG;
    }
    const int result = mesh_radio_backup_write_file(profile, path);
    if (result == 0 && sequence != NULL) {
        *sequence = next;
    }
    return result;
}

int mesh_radio_profile_store_load(const struct mesh_radio_profile_store *store, uint32_t sequence,
                                  struct mesh_radio_backup *profile) {
    if (!mesh_radio_profile_store_enabled(store)) {
        return -ENODEV;
    }
    if (profile == NULL || sequence == 0U) {
        return -EINVAL;
    }
    char file[MESH_RADIO_PROFILE_FILE_MAX];
    radio_profile_file(sequence, file, sizeof file);
    char path[MESH_RADIO_BACKUP_PATH_MAX + MESH_RADIO_PROFILE_FILE_MAX];
    if (!mesh_radio_profile_store_path(store, file, path, sizeof path)) {
        return -ENAMETOOLONG;
    }
    const int result = mesh_radio_backup_read_file(profile, path);
    if (result == 0 && profile->header.reason != MESH_RADIO_BACKUP_PROFILE) {
        /* A backup copied in under a profile's name is a radio, and would be applied as one. */
        mesh_radio_backup_reset(profile);
        return -EBADMSG;
    }
    return result;
}

int mesh_radio_profile_store_remove(struct mesh_radio_profile_store *store, uint32_t sequence) {
    if (!mesh_radio_profile_store_enabled(store)) {
        return -ENODEV;
    }
    char file[MESH_RADIO_PROFILE_FILE_MAX];
    radio_profile_file(sequence, file, sizeof file);
    char path[MESH_RADIO_BACKUP_PATH_MAX + MESH_RADIO_PROFILE_FILE_MAX];
    if (sequence == 0U || !mesh_radio_profile_store_path(store, file, path, sizeof path)) {
        return -EINVAL;
    }
    if (remove(path) != 0) {
        return errno == ENOENT ? -ENOENT : -errno;
    }
    return 0;
}

struct radio_profile_cfgs {
    char (*out)[MESH_RADIO_PROFILE_FILE_MAX];
    size_t max;
    size_t total;
};

static bool radio_profile_is_cfg(const char *name) {
    const size_t len = strlen(name);
    const size_t suffix = sizeof RADIO_PROFILE_CFG - 1U;
    if (len <= suffix || len >= MESH_RADIO_PROFILE_FILE_MAX || name[0] == '.') {
        return false;
    }
    /* Any case: a file named on a phone or a PC may come as PROFILE.CFG. */
    for (size_t i = 0; i < suffix; ++i) {
        const char c = name[len - suffix + i];
        const char lower = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (lower != RADIO_PROFILE_CFG[i]) {
            return false;
        }
    }
    return true;
}

static void radio_profile_note_cfg(void *context, const char *name) {
    struct radio_profile_cfgs *cfgs = context;
    if (!radio_profile_is_cfg(name)) {
        return;
    }
    /* A full list keeps the first by name, which is the order it is shown in - not whichever
       the directory happened to hand over first. */
    const size_t kept = cfgs->total < cfgs->max ? cfgs->total : cfgs->max;
    if (kept < cfgs->max) {
        inkwell_str_copy(cfgs->out[kept], MESH_RADIO_PROFILE_FILE_MAX, name);
    } else if (cfgs->max > 0U) {
        size_t last = 0U;
        for (size_t i = 1; i < kept; ++i) {
            if (strcmp(cfgs->out[i], cfgs->out[last]) > 0) {
                last = i;
            }
        }
        if (strcmp(name, cfgs->out[last]) < 0) {
            inkwell_str_copy(cfgs->out[last], MESH_RADIO_PROFILE_FILE_MAX, name);
        }
    }
    ++cfgs->total;
}

static int radio_profile_by_name(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

int mesh_radio_profile_store_cfgs(const struct mesh_radio_profile_store *store,
                                  char (*out)[MESH_RADIO_PROFILE_FILE_MAX], size_t max) {
    if (!mesh_radio_profile_store_enabled(store)) {
        return -ENODEV;
    }
    if (out == NULL && max > 0U) {
        return -EINVAL;
    }
    struct radio_profile_cfgs cfgs = {.out = out, .max = max};
    const int result = inkwell_file_list(store->dir, radio_profile_note_cfg, &cfgs);
    if (result < 0) {
        return result;
    }
    const size_t kept = cfgs.total < max ? cfgs.total : max;
    if (kept > 1U) {
        qsort(out, kept, sizeof *out, radio_profile_by_name);
    }
    return (int)cfgs.total;
}
