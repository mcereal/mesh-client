#include "mesh/core/radio_backup.h"

#include "inkwell/base/file.h"
#include "inkwell/base/record_file.h"
#include "inkwell/base/wipe.h"
#include "inkwell/codec/sha256.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One record's line can hold the largest section in hex, its tag and the key before it. */
#define RADIO_BACKUP_LINE_MAX (MESH_RADIO_BACKUP_SECTION_MAX * 2U + 64U)
#define RADIO_BACKUP_SUFFIX ".backup"

/* ---- the in-memory backup ------------------------------------------------------------------ */

void mesh_radio_backup_reset(struct mesh_radio_backup *backup) {
    if (backup == NULL) {
        return;
    }
    memset(&backup->header, 0, sizeof backup->header);
    memset(backup->sections, 0, sizeof backup->sections);
    backup->section_count = 0U;
    backup->used = 0U;
}

int mesh_radio_backup_add(struct mesh_radio_backup *backup, uint16_t tag, const void *data,
                          size_t len) {
    if (backup == NULL || (data == NULL && len > 0U) || len > MESH_RADIO_BACKUP_SECTION_MAX) {
        return -EINVAL;
    }
    if (backup->section_count >= MESH_RADIO_BACKUP_SECTIONS_MAX ||
        len > MESH_RADIO_BACKUP_PAYLOAD_MAX - backup->used) {
        return -ENOSPC;
    }
    struct mesh_radio_backup_section *section = &backup->sections[backup->section_count++];
    section->tag = tag;
    section->len = (uint16_t)len;
    section->offset = (uint32_t)backup->used;
    if (len > 0U) {
        memcpy(backup->payload + backup->used, data, len);
    }
    backup->used += len;
    if (tag == MESH_RADIO_BACKUP_IDENTITY) {
        backup->header.has_identity = true;
    }
    return 0;
}

const struct mesh_radio_backup_section *
mesh_radio_backup_section_at(const struct mesh_radio_backup *backup, size_t index,
                             const uint8_t **data) {
    if (backup == NULL || index >= backup->section_count) {
        return NULL;
    }
    const struct mesh_radio_backup_section *section = &backup->sections[index];
    if (data != NULL) {
        *data = backup->payload + section->offset;
    }
    return section;
}

size_t mesh_radio_backup_count_tag(const struct mesh_radio_backup *backup, uint16_t tag) {
    size_t count = 0U;
    for (size_t i = 0; backup != NULL && i < backup->section_count; ++i) {
        count += backup->sections[i].tag == tag ? 1U : 0U;
    }
    return count;
}

size_t mesh_radio_backup_identity(const struct mesh_radio_backup *backup, const uint8_t **data) {
    for (size_t i = 0; backup != NULL && i < backup->section_count; ++i) {
        if (backup->sections[i].tag == MESH_RADIO_BACKUP_IDENTITY) {
            if (data != NULL) {
                *data = backup->payload + backup->sections[i].offset;
            }
            return backup->sections[i].len;
        }
    }
    return 0U;
}

void mesh_radio_backup_wipe(struct mesh_radio_backup *backup) {
    if (backup == NULL) {
        return;
    }
    inkwell_wipe(backup->payload, sizeof backup->payload);
    mesh_radio_backup_reset(backup);
}

/* The next section at or after `*index` that is a setting, and not the key. */
static const struct mesh_radio_backup_section *
radio_backup_next_setting(const struct mesh_radio_backup *backup, size_t *index) {
    while (*index < backup->section_count &&
           backup->sections[*index].tag == MESH_RADIO_BACKUP_IDENTITY) {
        ++*index;
    }
    return *index < backup->section_count ? &backup->sections[*index] : NULL;
}

bool mesh_radio_backup_same_payload(const struct mesh_radio_backup *a,
                                    const struct mesh_radio_backup *b) {
    if (a == NULL || b == NULL) {
        return false;
    }
    size_t i = 0U;
    size_t j = 0U;
    for (;; ++i, ++j) {
        const struct mesh_radio_backup_section *x = radio_backup_next_setting(a, &i);
        const struct mesh_radio_backup_section *y = radio_backup_next_setting(b, &j);
        if (x == NULL || y == NULL) {
            return x == y;
        }
        if (x->tag != y->tag || x->len != y->len ||
            memcmp(a->payload + x->offset, b->payload + y->offset, x->len) != 0) {
            return false;
        }
    }
}

/* ---- names --------------------------------------------------------------------------------- */

static const char *const k_reason_keys[] = {
    [MESH_RADIO_BACKUP_MANUAL] = "manual",
    [MESH_RADIO_BACKUP_FIRST_CONNECT] = "first_connect",
    [MESH_RADIO_BACKUP_BEFORE_WRITE] = "before_write",
    [MESH_RADIO_BACKUP_BEFORE_FIRMWARE] = "before_firmware",
    [MESH_RADIO_BACKUP_PROFILE] = "profile",
};

static const char *const k_protocol_keys[] = {
    [MESH_RADIO_BACKUP_MESHTASTIC] = "meshtastic",
    [MESH_RADIO_BACKUP_MESHCORE] = "meshcore",
};

#define RADIO_BACKUP_COUNT(table) (sizeof(table) / sizeof((table)[0]))

const char *mesh_radio_backup_reason_key(uint8_t reason) {
    return reason < RADIO_BACKUP_COUNT(k_reason_keys) ? k_reason_keys[reason] : NULL;
}

const char *mesh_radio_backup_protocol_key(uint8_t protocol) {
    return protocol < RADIO_BACKUP_COUNT(k_protocol_keys) ? k_protocol_keys[protocol] : NULL;
}

static uint8_t radio_backup_lookup(const char *const *table, size_t count, const char *key) {
    for (size_t i = 1; i < count; ++i) {
        if (table[i] != NULL && strcmp(table[i], key) == 0) {
            return (uint8_t)i;
        }
    }
    return 0U;
}

/* ---- writing ------------------------------------------------------------------------------- */

/*
 * One writer for every record, which is what keeps the digest honest: the bytes hashed are the
 * key and the value exactly as a reader will be handed them - unescaped - so the two sides
 * cannot disagree about what a record was.
 */
struct radio_backup_writer {
    FILE *file;
    struct inkwell_sha256 digest;
};

static void radio_backup_hash(struct inkwell_sha256 *digest, const char *key, const char *value) {
    inkwell_sha256_update(digest, key, strlen(key));
    inkwell_sha256_update(digest, "=", 1U);
    inkwell_sha256_update(digest, value, strlen(value));
    inkwell_sha256_update(digest, "\n", 1U);
}

static void radio_backup_put(struct radio_backup_writer *writer, const char *key,
                             const char *value) {
    fprintf(writer->file, "%s=", key);
    inkwell_record_write_escaped(writer->file, value);
    fputc('\n', writer->file);
    radio_backup_hash(&writer->digest, key, value);
}

static void radio_backup_put_u32(struct radio_backup_writer *writer, const char *key,
                                 uint32_t value) {
    char text[16];
    snprintf(text, sizeof text, "%" PRIu32, value);
    radio_backup_put(writer, key, text);
}

static void radio_backup_hex(char *out, const uint8_t *data, size_t len) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i * 2U] = digits[data[i] >> 4U];
        out[i * 2U + 1U] = digits[data[i] & 0x0FU];
    }
    out[len * 2U] = '\0';
}

static void radio_backup_write_records(FILE *file, void *context) {
    const struct mesh_radio_backup *backup = context;
    const struct mesh_radio_backup_header *header = &backup->header;
    struct radio_backup_writer writer = {.file = file};
    inkwell_sha256_init(&writer.digest);

    radio_backup_put_u32(&writer, "format", MESH_RADIO_BACKUP_FORMAT);
    radio_backup_put(&writer, "protocol", mesh_radio_backup_protocol_key(header->protocol));
    const char *reason = mesh_radio_backup_reason_key(header->reason);
    radio_backup_put(&writer, "reason", reason != NULL ? reason : "manual");
    radio_backup_put_u32(&writer, "saved_at", header->saved_at);
    char node[12];
    snprintf(node, sizeof node, "%08" PRIx32, header->node_id);
    radio_backup_put(&writer, "node", node);
    radio_backup_put(&writer, "device", header->device);
    radio_backup_put(&writer, "name", header->name);
    radio_backup_put(&writer, "model", header->model);
    radio_backup_put(&writer, "firmware", header->firmware);
    radio_backup_put(&writer, "region", header->region);
    radio_backup_put(&writer, "preset", header->preset);
    if (header->has_radio) {
        char radio[64];
        snprintf(radio, sizeof radio, "%" PRIu32 ",%" PRIu32 ",%u,%u,%d", header->frequency_khz,
                 header->bandwidth_hz, (unsigned)header->spreading_factor,
                 (unsigned)header->coding_rate, (int)header->tx_power_dbm);
        radio_backup_put(&writer, "radio", radio);
    }
    for (size_t i = 0; i < header->channel_count && i < MESH_RADIO_BACKUP_CHANNELS; ++i) {
        /* The slot first, so a name holding a comma is still one name. */
        char channel[MESH_RADIO_BACKUP_CHANNEL_NAME + 8U];
        snprintf(channel, sizeof channel, "%zu,%s", i, header->channel_names[i]);
        radio_backup_put(&writer, "channel", channel);
    }
    if (header->has_nodes_heard) {
        radio_backup_put_u32(&writer, "nodes_heard", header->nodes_heard);
    }
    if (header->has_contacts) {
        radio_backup_put_u32(&writer, "contacts", header->contacts);
    }
    if (header->reason == MESH_RADIO_BACKUP_PROFILE) {
        char parts[24];
        snprintf(parts, sizeof parts, "%08" PRIx32 ",%08" PRIx32, header->parts.topics,
                 header->parts.modules);
        radio_backup_put(&writer, "parts", parts);
    }

    char line[RADIO_BACKUP_LINE_MAX];
    for (size_t i = 0; i < backup->section_count; ++i) {
        const struct mesh_radio_backup_section *section = &backup->sections[i];
        const int prefix = snprintf(line, sizeof line, "%u,", (unsigned)section->tag);
        radio_backup_hex(line + prefix, backup->payload + section->offset, section->len);
        radio_backup_put(&writer, "section", line);
    }
    inkwell_wipe(line, sizeof line); /* the last section may have been a private key, in hex */

    uint8_t digest[INKWELL_SHA256_DIGEST_LEN];
    inkwell_sha256_final(&writer.digest, digest);
    char digest_hex[INKWELL_SHA256_DIGEST_LEN * 2U + 1U];
    radio_backup_hex(digest_hex, digest, sizeof digest);
    fprintf(file, "sha256=%s\n", digest_hex);
}

/* A backup is of a radio, and says which; a profile is for any radio, and says none. */
static bool radio_backup_node_ok(const struct mesh_radio_backup_header *header) {
    return header->node_id != 0U || header->reason == MESH_RADIO_BACKUP_PROFILE;
}

int mesh_radio_backup_write_file(const struct mesh_radio_backup *backup, const char *path) {
    if (backup == NULL || path == NULL || path[0] == '\0' ||
        mesh_radio_backup_protocol_key(backup->header.protocol) == NULL ||
        !radio_backup_node_ok(&backup->header)) {
        return -EINVAL;
    }
    /* Whatever path made it: two radios with one key are one node on the mesh. */
    if (backup->header.reason == MESH_RADIO_BACKUP_PROFILE &&
        mesh_radio_backup_identity(backup, NULL) > 0U) {
        return -EPERM;
    }
    char temp[MESH_RADIO_BACKUP_PATH_MAX + 8U];
    return inkwell_record_replace(path, temp, sizeof temp, radio_backup_write_records,
                                  (void *)backup, true);
}

/* ---- reading ------------------------------------------------------------------------------- */

struct radio_backup_reader {
    struct mesh_radio_backup *backup;
    struct inkwell_sha256 digest;
    bool saw_format;
    bool saw_digest;
    /* The first thing wrong with the file, which is what the read reports. */
    int error;
};

static int radio_backup_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Hex into `out`; the byte count, or -1 for an odd length, a stray character or too many. */
static long radio_backup_unhex(const char *text, uint8_t *out, size_t max) {
    const size_t len = strlen(text);
    if (len % 2U != 0U || len / 2U > max) {
        return -1;
    }
    for (size_t i = 0; i < len / 2U; ++i) {
        const int hi = radio_backup_nibble(text[i * 2U]);
        const int lo = radio_backup_nibble(text[i * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (long)(len / 2U);
}

static bool radio_backup_u32(const char *text, uint32_t *out, int base) {
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, base);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static void radio_backup_fail(struct radio_backup_reader *reader, int error) {
    if (reader->error == 0) {
        reader->error = error;
    }
}

static void radio_backup_text(char *out, size_t out_len, const char *value) {
    snprintf(out, out_len, "%s", value);
}

static void radio_backup_read_radio(struct radio_backup_reader *reader, const char *value) {
    struct mesh_radio_backup_header *header = &reader->backup->header;
    unsigned long frequency = 0UL;
    unsigned long bandwidth = 0UL;
    unsigned sf = 0U;
    unsigned cr = 0U;
    int power = 0;
    if (sscanf(value, "%lu,%lu,%u,%u,%d", &frequency, &bandwidth, &sf, &cr, &power) != 5 ||
        frequency > UINT32_MAX || bandwidth > UINT32_MAX || sf > UINT8_MAX || cr > UINT8_MAX ||
        power < INT8_MIN || power > INT8_MAX) {
        radio_backup_fail(reader, -EBADMSG);
        return;
    }
    header->has_radio = true;
    header->frequency_khz = (uint32_t)frequency;
    header->bandwidth_hz = (uint32_t)bandwidth;
    header->spreading_factor = (uint8_t)sf;
    header->coding_rate = (uint8_t)cr;
    header->tx_power_dbm = (int8_t)power;
}

static void radio_backup_read_channel(struct radio_backup_reader *reader, const char *value) {
    struct mesh_radio_backup_header *header = &reader->backup->header;
    char *end = NULL;
    const unsigned long slot = strtoul(value, &end, 10);
    if (end == value || *end != ',' || slot >= MESH_RADIO_BACKUP_CHANNELS) {
        radio_backup_fail(reader, -EBADMSG);
        return;
    }
    radio_backup_text(header->channel_names[slot], sizeof header->channel_names[slot], end + 1);
    if (slot + 1U > header->channel_count) {
        header->channel_count = (uint8_t)(slot + 1U);
    }
}

static void radio_backup_read_section(struct radio_backup_reader *reader, const char *value) {
    char *end = NULL;
    const unsigned long tag = strtoul(value, &end, 10);
    uint8_t bytes[MESH_RADIO_BACKUP_SECTION_MAX];
    const long len = (end != value && *end == ',' && tag <= UINT16_MAX)
                         ? radio_backup_unhex(end + 1, bytes, sizeof bytes)
                         : -1;
    if (len < 0 || mesh_radio_backup_add(reader->backup, (uint16_t)tag, bytes, (size_t)len) != 0) {
        radio_backup_fail(reader, -EBADMSG);
    }
    inkwell_wipe(bytes, sizeof bytes); /* the section may have been a private key */
}

static void radio_backup_visit(void *context, const char *key, char *value) {
    struct radio_backup_reader *reader = context;
    struct mesh_radio_backup_header *header = &reader->backup->header;
    if (reader->saw_digest) {
        /* Nothing may follow the digest: it would be a record nobody vouched for. */
        radio_backup_fail(reader, -EBADMSG);
        return;
    }
    if (strcmp(key, "sha256") == 0) {
        reader->saw_digest = true;
        uint8_t expected[INKWELL_SHA256_DIGEST_LEN];
        uint8_t actual[INKWELL_SHA256_DIGEST_LEN];
        inkwell_sha256_final(&reader->digest, actual);
        if (radio_backup_unhex(value, expected, sizeof expected) != (long)sizeof expected ||
            memcmp(expected, actual, sizeof expected) != 0) {
            radio_backup_fail(reader, -EBADMSG);
        }
        return;
    }
    radio_backup_hash(&reader->digest, key, value);

    if (!reader->saw_format) {
        /* The first record says what the rest are; anything else first is not a backup. */
        uint32_t format = 0U;
        if (strcmp(key, "format") != 0 || !radio_backup_u32(value, &format, 10) || format == 0U) {
            radio_backup_fail(reader, -EBADMSG);
        } else if (format > MESH_RADIO_BACKUP_FORMAT) {
            radio_backup_fail(reader, -EPROTO);
        }
        reader->saw_format = true;
        return;
    }
    if (reader->error != 0) {
        return;
    }
    if (strcmp(key, "protocol") == 0) {
        header->protocol =
            radio_backup_lookup(k_protocol_keys, RADIO_BACKUP_COUNT(k_protocol_keys), value);
    } else if (strcmp(key, "reason") == 0) {
        header->reason =
            radio_backup_lookup(k_reason_keys, RADIO_BACKUP_COUNT(k_reason_keys), value);
    } else if (strcmp(key, "saved_at") == 0) {
        if (!radio_backup_u32(value, &header->saved_at, 10)) {
            radio_backup_fail(reader, -EBADMSG);
        }
    } else if (strcmp(key, "node") == 0) {
        if (!radio_backup_u32(value, &header->node_id, 16)) {
            radio_backup_fail(reader, -EBADMSG);
        }
    } else if (strcmp(key, "device") == 0) {
        radio_backup_text(header->device, sizeof header->device, value);
    } else if (strcmp(key, "name") == 0) {
        radio_backup_text(header->name, sizeof header->name, value);
    } else if (strcmp(key, "model") == 0) {
        radio_backup_text(header->model, sizeof header->model, value);
    } else if (strcmp(key, "firmware") == 0) {
        radio_backup_text(header->firmware, sizeof header->firmware, value);
    } else if (strcmp(key, "region") == 0) {
        radio_backup_text(header->region, sizeof header->region, value);
    } else if (strcmp(key, "preset") == 0) {
        radio_backup_text(header->preset, sizeof header->preset, value);
    } else if (strcmp(key, "radio") == 0) {
        radio_backup_read_radio(reader, value);
    } else if (strcmp(key, "channel") == 0) {
        radio_backup_read_channel(reader, value);
    } else if (strcmp(key, "nodes_heard") == 0) {
        header->has_nodes_heard = radio_backup_u32(value, &header->nodes_heard, 10);
    } else if (strcmp(key, "contacts") == 0) {
        header->has_contacts = radio_backup_u32(value, &header->contacts, 10);
    } else if (strcmp(key, "parts") == 0) {
        char *end = NULL;
        const unsigned long topics = strtoul(value, &end, 16);
        const unsigned long modules = *end == ',' ? strtoul(end + 1, NULL, 16) : 0UL;
        header->parts.topics = (uint32_t)topics;
        header->parts.modules = (uint32_t)modules;
    } else if (strcmp(key, "section") == 0) {
        radio_backup_read_section(reader, value);
    }
    /* Anything else is a field from a later build of the same format: skipped, still hashed. */
}

int mesh_radio_backup_read_file(struct mesh_radio_backup *backup, const char *path) {
    if (backup == NULL || path == NULL) {
        return -EINVAL;
    }
    mesh_radio_backup_reset(backup);
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return errno == ENOENT ? -ENOENT : -errno;
    }
    struct radio_backup_reader reader = {.backup = backup};
    inkwell_sha256_init(&reader.digest);
    char line[RADIO_BACKUP_LINE_MAX + 16U];
    const int read = inkwell_record_read(file, line, sizeof line, radio_backup_visit, &reader);
    fclose(file);
    inkwell_wipe(line, sizeof line); /* and here, the last line read */

    int result = reader.error;
    if (result == 0 && read < 0) {
        result = read;
    }
    /* A file with no digest was cut short: the digest is always the last thing written. */
    if (result == 0 && (!reader.saw_format || !reader.saw_digest)) {
        result = -EBADMSG;
    }
    if (result == 0 && (backup->header.protocol == MESH_RADIO_BACKUP_PROTOCOL_NONE ||
                        !radio_backup_node_ok(&backup->header))) {
        result = -EBADMSG;
    }
    /* A profile with a key in it was not written by this client, and is not one to apply. */
    if (result == 0 && backup->header.reason == MESH_RADIO_BACKUP_PROFILE &&
        backup->header.has_identity) {
        result = -EBADMSG;
    }
    if (result != 0) {
        mesh_radio_backup_wipe(backup); /* a half-read key is still a key */
    }
    return result;
}

/* ---- the store ----------------------------------------------------------------------------- */

int mesh_radio_backup_store_init(struct mesh_radio_backup_store *store, const char *dir) {
    if (store == NULL) {
        return -EINVAL;
    }
    memset(store, 0, sizeof *store);
    store->keep_automatic = MESH_RADIO_BACKUP_KEEP_AUTOMATIC;
    if (dir == NULL || dir[0] == '\0') {
        return -EINVAL;
    }
    const int written = snprintf(store->dir, sizeof store->dir, "%s", dir);
    /* Room for "/<node>/<file>" under it, or no file in it could ever be named. */
    if (written < 0 || (size_t)written + 1U + 8U + 1U + 64U >= sizeof store->dir) {
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

bool mesh_radio_backup_store_enabled(const struct mesh_radio_backup_store *store) {
    return store != NULL && store->enabled;
}

/* The store's init left room for this, so truncation cannot happen - but the answer is still
   checked rather than assumed, and a caller refuses on it. */
static int radio_backup_node_dir(const struct mesh_radio_backup_store *store, uint32_t node_id,
                                 char *out, size_t out_len) {
    const int written = snprintf(out, out_len, "%s/%08" PRIx32, store->dir, node_id);
    return written < 0 || (size_t)written >= out_len ? -ENAMETOOLONG : 0;
}

/* "00000007.before_write.backup" into its parts; false for anything else in the directory. */
static bool radio_backup_parse_name(const char *name, struct mesh_radio_backup_entry *entry) {
    const size_t len = strlen(name);
    const size_t suffix = sizeof RADIO_BACKUP_SUFFIX - 1U;
    if (len >= sizeof entry->file || len < 10U + suffix ||
        strcmp(name + len - suffix, RADIO_BACKUP_SUFFIX) != 0 || name[8] != '.') {
        return false;
    }
    char digits[9];
    memcpy(digits, name, 8U);
    digits[8] = '\0';
    uint32_t sequence = 0U;
    if (!radio_backup_u32(digits, &sequence, 10)) {
        return false;
    }
    char reason[32];
    const size_t reason_len = len - 9U - suffix;
    if (reason_len == 0U || reason_len >= sizeof reason) {
        return false;
    }
    memcpy(reason, name + 9, reason_len);
    reason[reason_len] = '\0';
    entry->sequence = sequence;
    entry->reason = radio_backup_lookup(k_reason_keys, RADIO_BACKUP_COUNT(k_reason_keys), reason);
    snprintf(entry->file, sizeof entry->file, "%s", name);
    return entry->reason != MESH_RADIO_BACKUP_REASON_NONE;
}

/* Every backup in a directory, collected unsorted; how many there were, kept or not. */
struct radio_backup_listing {
    struct mesh_radio_backup_entry *entries;
    size_t max;
    size_t count;
    size_t total;
};

static void radio_backup_collect(void *context, const char *name) {
    struct radio_backup_listing *listing = context;
    struct mesh_radio_backup_entry entry;
    if (!radio_backup_parse_name(name, &entry)) {
        return;
    }
    ++listing->total;
    if (listing->count < listing->max) {
        listing->entries[listing->count++] = entry;
        return;
    }
    /*
     * Full: keep the newest, which is what every caller asks for first - and, in a list of two or
     * more, the first-connect backup too. The prune never removes that one and it is never taken
     * again, so it is the oldest a radio has; left to the cap, a radio with ten automatic backups
     * and two pressed would keep it on the card and never list it, where nobody could open it.
     */
    const bool reserve = listing->max >= 2U;
    const bool first = reserve && entry.reason == MESH_RADIO_BACKUP_FIRST_CONNECT;
    size_t oldest = listing->count;
    for (size_t i = 0; i < listing->count; ++i) {
        if (reserve && listing->entries[i].reason == MESH_RADIO_BACKUP_FIRST_CONNECT) {
            continue;
        }
        if (oldest == listing->count ||
            listing->entries[i].sequence < listing->entries[oldest].sequence) {
            oldest = i;
        }
    }
    if (oldest < listing->count && (first || entry.sequence > listing->entries[oldest].sequence)) {
        listing->entries[oldest] = entry;
    }
}

static int radio_backup_newest_first(const void *a, const void *b) {
    const uint32_t left = ((const struct mesh_radio_backup_entry *)a)->sequence;
    const uint32_t right = ((const struct mesh_radio_backup_entry *)b)->sequence;
    return left < right ? 1 : left > right ? -1 : 0;
}

int mesh_radio_backup_store_list(const struct mesh_radio_backup_store *store, uint32_t node_id,
                                 struct mesh_radio_backup_entry *out, size_t max) {
    if (!mesh_radio_backup_store_enabled(store)) {
        return -ENODEV;
    }
    if (out == NULL && max > 0U) {
        return -EINVAL;
    }
    char dir[MESH_RADIO_BACKUP_PATH_MAX];
    const int named = radio_backup_node_dir(store, node_id, dir, sizeof dir);
    if (named != 0) {
        return named;
    }
    if (!inkwell_file_is_dir(dir)) {
        return 0;
    }
    struct radio_backup_listing listing = {.entries = out, .max = max};
    const int result = inkwell_file_list(dir, radio_backup_collect, &listing);
    if (result < 0) {
        return result;
    }
    if (listing.count > 1U) {
        qsort(out, listing.count, sizeof *out, radio_backup_newest_first);
    }
    return (int)listing.total;
}

/* The next sequence in a directory: one past the highest there, whatever its reason. */
struct radio_backup_highest {
    uint32_t sequence;
    bool any;
};

static void radio_backup_note_highest(void *context, const char *name) {
    struct radio_backup_highest *highest = context;
    struct mesh_radio_backup_entry entry;
    if (radio_backup_parse_name(name, &entry) &&
        (!highest->any || entry.sequence > highest->sequence)) {
        highest->sequence = entry.sequence;
        highest->any = true;
    }
}

/*
 * Down to `keep` automatic backups, oldest first. Listed in full rather than through the capped
 * listing above, because the ones to remove are exactly the ones a capped list would leave out.
 */
struct radio_backup_prune {
    uint32_t protected_sequence; /* 0 for none */
    uint32_t sequences[MESH_RADIO_BACKUP_KEEP_AUTOMATIC * 4U];
    char files[MESH_RADIO_BACKUP_KEEP_AUTOMATIC * 4U][64];
    size_t count;
};

static void radio_backup_note_automatic(void *context, const char *name) {
    struct radio_backup_prune *prune = context;
    struct mesh_radio_backup_entry entry;
    /* The first-connect backup is taken once, when a radio has none on the card, so a prune
       that removed it could never be followed by another: the radio as it came to this client
       would be gone after ten saves. It is kept like a pressed one. */
    if (!radio_backup_parse_name(name, &entry) || entry.reason == MESH_RADIO_BACKUP_MANUAL ||
        entry.reason == MESH_RADIO_BACKUP_FIRST_CONNECT ||
        (prune->protected_sequence != 0U && entry.sequence == prune->protected_sequence)) {
        return;
    }
    size_t slot = prune->count;
    if (prune->count >= sizeof prune->sequences / sizeof prune->sequences[0]) {
        /* Over what one pass holds, which only a directory left to grow by hand reaches:
           forget the newest here, so the oldest are still the ones removed. */
        slot = 0U;
        for (size_t i = 1; i < prune->count; ++i) {
            if (prune->sequences[i] > prune->sequences[slot]) {
                slot = i;
            }
        }
        if (entry.sequence >= prune->sequences[slot]) {
            return;
        }
    } else {
        ++prune->count;
    }
    prune->sequences[slot] = entry.sequence;
    snprintf(prune->files[slot], sizeof prune->files[slot], "%s", entry.file);
}

static void radio_backup_prune(const struct mesh_radio_backup_store *store, uint32_t node_id,
                               const char *dir) {
    struct radio_backup_prune prune;
    for (;;) {
        memset(&prune, 0, sizeof prune);
        prune.protected_sequence = store->protect_node == node_id ? store->protect_sequence : 0U;
        if (inkwell_file_list(dir, radio_backup_note_automatic, &prune) < 0 ||
            prune.count <= store->keep_automatic) {
            return;
        }
        size_t oldest = 0U;
        for (size_t i = 1; i < prune.count; ++i) {
            if (prune.sequences[i] < prune.sequences[oldest]) {
                oldest = i;
            }
        }
        char path[MESH_RADIO_BACKUP_PATH_MAX + 80U];
        snprintf(path, sizeof path, "%s/%s", dir, prune.files[oldest]);
        if (remove(path) != 0) {
            return;
        }
    }
}

int mesh_radio_backup_store_save(struct mesh_radio_backup_store *store,
                                 const struct mesh_radio_backup *backup,
                                 struct mesh_radio_backup_entry *entry) {
    if (!mesh_radio_backup_store_enabled(store)) {
        return -ENODEV;
    }
    if (backup == NULL || backup->header.node_id == 0U ||
        mesh_radio_backup_reason_key(backup->header.reason) == NULL) {
        return -EINVAL;
    }
    char dir[MESH_RADIO_BACKUP_PATH_MAX];
    const int named = radio_backup_node_dir(store, backup->header.node_id, dir, sizeof dir);
    if (named != 0) {
        return named;
    }
    const int made = inkwell_file_mkdir(dir);
    if (made != 0 && !(made == -EEXIST && inkwell_file_is_dir(dir))) {
        return made == -EEXIST ? -ENOTDIR : made;
    }

    struct radio_backup_highest highest = {0};
    const int listed = inkwell_file_list(dir, radio_backup_note_highest, &highest);
    if (listed < 0) {
        return listed;
    }
    const uint32_t sequence = highest.any ? highest.sequence + 1U : 1U;
    if (sequence > 99999999U) {
        return -EOVERFLOW;
    }
    struct mesh_radio_backup_entry written;
    memset(&written, 0, sizeof written);
    written.sequence = sequence;
    written.reason = backup->header.reason;
    snprintf(written.file, sizeof written.file, "%08" PRIu32 ".%s" RADIO_BACKUP_SUFFIX, sequence,
             mesh_radio_backup_reason_key(backup->header.reason));

    char path[MESH_RADIO_BACKUP_PATH_MAX + 80U];
    snprintf(path, sizeof path, "%s/%s", dir, written.file);
    const int result = mesh_radio_backup_write_file(backup, path);
    if (result != 0) {
        return result;
    }
    radio_backup_prune(store, backup->header.node_id, dir);
    if (entry != NULL) {
        *entry = written;
    }
    return 0;
}

int mesh_radio_backup_store_load(const struct mesh_radio_backup_store *store, uint32_t node_id,
                                 const struct mesh_radio_backup_entry *entry,
                                 struct mesh_radio_backup *backup) {
    if (!mesh_radio_backup_store_enabled(store)) {
        return -ENODEV;
    }
    if (entry == NULL || backup == NULL || strchr(entry->file, '/') != NULL ||
        strchr(entry->file, '\\') != NULL) {
        return -EINVAL;
    }
    char path[MESH_RADIO_BACKUP_PATH_MAX + 80U];
    snprintf(path, sizeof path, "%s/%08" PRIx32 "/%s", store->dir, node_id, entry->file);
    return mesh_radio_backup_read_file(backup, path);
}

/* Every subdirectory named like a node, holding at least one backup. */
struct radio_backup_radios {
    const struct mesh_radio_backup_store *store;
    uint32_t *out;
    size_t max;
    size_t total;
};

static void radio_backup_note_radio(void *context, const char *name) {
    struct radio_backup_radios *radios = context;
    if (strlen(name) != 8U) {
        return;
    }
    uint32_t node = 0U;
    if (!radio_backup_u32(name, &node, 16) || node == 0U ||
        mesh_radio_backup_store_list(radios->store, node, NULL, 0U) <= 0) {
        return;
    }
    if (radios->total < radios->max) {
        radios->out[radios->total] = node;
    }
    ++radios->total;
}

int mesh_radio_backup_store_radios(const struct mesh_radio_backup_store *store, uint32_t *out,
                                   size_t max) {
    if (!mesh_radio_backup_store_enabled(store)) {
        return -ENODEV;
    }
    if (out == NULL && max > 0U) {
        return -EINVAL;
    }
    struct radio_backup_radios radios = {.store = store, .out = out, .max = max};
    const int result = inkwell_file_list(store->dir, radio_backup_note_radio, &radios);
    return result < 0 ? result : (int)radios.total;
}

/* The file in a directory carrying one sequence number, whatever its reason. */
struct radio_backup_find {
    uint32_t sequence;
    struct mesh_radio_backup_entry entry;
    bool found;
};

static void radio_backup_note_sequence(void *context, const char *name) {
    struct radio_backup_find *find = context;
    struct mesh_radio_backup_entry entry;
    if (!find->found && radio_backup_parse_name(name, &entry) && entry.sequence == find->sequence) {
        find->entry = entry;
        find->found = true;
    }
}

int mesh_radio_backup_store_remove(struct mesh_radio_backup_store *store, uint32_t node_id,
                                   uint32_t sequence) {
    if (!mesh_radio_backup_store_enabled(store)) {
        return -ENODEV;
    }
    char dir[MESH_RADIO_BACKUP_PATH_MAX];
    const int named = radio_backup_node_dir(store, node_id, dir, sizeof dir);
    if (named != 0) {
        return named;
    }
    if (!inkwell_file_is_dir(dir)) {
        return -ENOENT;
    }
    struct radio_backup_find find = {.sequence = sequence};
    const int listed = inkwell_file_list(dir, radio_backup_note_sequence, &find);
    if (listed < 0) {
        return listed;
    }
    if (!find.found) {
        return -ENOENT;
    }
    char path[MESH_RADIO_BACKUP_PATH_MAX + 80U];
    snprintf(path, sizeof path, "%s/%s", dir, find.entry.file);
    return remove(path) == 0 ? 0 : -errno;
}
