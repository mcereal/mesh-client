#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/preferences.h"

#include "inkstand/persist/recent.h"

#include "inkwell/base/file.h"
#include "inkwell/base/log.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* enum mesh_ui_device_kind's SERIAL. Spelled out here because preferences.h keeps the kind as
   a byte rather than pulling the whole UI store in for one enum. */
#define MESH_UI_PREFS_KIND_SERIAL 1U

static void mesh_ui_preferences_init(struct mesh_ui_preferences *prefs) {
    if (prefs == NULL) {
        return;
    }
    memset(prefs, 0, sizeof(*prefs));
}

int mesh_ui_preferences_default_path(char *buffer, size_t buffer_len) {
    if (buffer == NULL || buffer_len == 0U) {
        return -EINVAL;
    }

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        home = ".";
    }

    int written = snprintf(buffer, buffer_len, "%s/.meshclient", home);
    if (written < 0) {
        return -errno;
    }
    if ((size_t)written >= buffer_len) {
        return -ENAMETOOLONG;
    }

    struct stat st;
    if (stat(buffer, &st) < 0) {
        if (errno != ENOENT) {
            inkwell_log_warn("ui", "Failed to stat %s: %s", buffer, strerror(errno));
        }
        const int created = inkwell_file_mkdir(buffer);
        if (created < 0 && created != -EEXIST) {
            inkwell_log_warn("ui", "Failed to create %s: %s", buffer, strerror(-created));
            return created;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        inkwell_log_warn("ui", "%s exists but is not a directory", buffer);
        return -ENOTDIR;
    }

    written = snprintf(buffer, buffer_len, "%s/.meshclient/ui_prefs", home);
    if (written < 0) {
        return -errno;
    }
    if ((size_t)written >= buffer_len) {
        return -ENAMETOOLONG;
    }

    return 0;
}

static void strip_newline(char *line) {
    if (line == NULL) {
        return;
    }
    char *newline = strpbrk(line, "\r\n");
    if (newline != NULL) {
        *newline = '\0';
    }
}

/* The two lists as the record carries them - an array and a count byte each - seen through
   inkstand's recently-used list. The read-only questions build one over a const record; nothing
   on those paths writes through it. */
static void radios_list(const struct mesh_ui_preferences *prefs, struct inkstand_recent *list) {
    (void)inkstand_recent_init(list, (void *)prefs->known_radios, sizeof prefs->known_radios[0],
                               MESH_UI_MAX_KNOWN_RADIOS, prefs->known_radio_count, NULL, NULL);
}

/* One identity test for a device, shared by every list operation. The kind is part of it - the
   same string over the other link is another device, which is what stops a tty path being handed
   to BLE. A BLE address is case-insensitive (BlueZ writes them upper-case, a config file may
   not); a tty path is not. */
static bool device_same(const struct mesh_ui_known_device *entry, const char *identifier,
                        uint8_t kind) {
    if (entry->kind != kind) {
        return false;
    }
    return kind == MESH_UI_PREFS_KIND_SERIAL ? strcmp(entry->identifier, identifier) == 0
                                             : strcasecmp(entry->identifier, identifier) == 0;
}

static bool device_entry_same(const void *entry, const void *wanted, void *context) {
    (void)context;
    const struct mesh_ui_known_device *other = wanted;
    return device_same(entry, other->identifier, other->kind);
}

static void devices_list(const struct mesh_ui_preferences *prefs, struct inkstand_recent *list) {
    (void)inkstand_recent_init(list, (void *)prefs->known_devices, sizeof prefs->known_devices[0],
                               MESH_UI_MAX_KNOWN_DEVICES, prefs->known_device_count,
                               device_entry_same, NULL);
}

static struct mesh_ui_known_device device_entry(const char *identifier, uint8_t kind) {
    struct mesh_ui_known_device entry;
    memset(&entry, 0, sizeof entry);
    snprintf(entry.identifier, sizeof entry.identifier, "%s", identifier);
    entry.kind = kind;
    return entry;
}

bool mesh_ui_preferences_knows_radio(const struct mesh_ui_preferences *prefs, uint32_t node_num) {
    if (prefs == NULL || node_num == 0U) {
        return false;
    }
    struct inkstand_recent radios;
    radios_list(prefs, &radios);
    return inkstand_recent_rank(&radios, &node_num) >= 0;
}

bool mesh_ui_preferences_note_radio(struct mesh_ui_preferences *prefs, uint32_t node_num) {
    if (prefs == NULL || node_num == 0U) {
        return false;
    }
    struct inkstand_recent radios;
    radios_list(prefs, &radios);
    const bool changed = inkstand_recent_note(&radios, &node_num);
    prefs->known_radio_count = (uint8_t)radios.count;
    return changed;
}

int mesh_ui_preferences_device_rank(const struct mesh_ui_preferences *prefs, const char *identifier,
                                    uint8_t kind) {
    if (prefs == NULL || identifier == NULL || identifier[0] == '\0') {
        return -1;
    }
    struct inkstand_recent devices;
    devices_list(prefs, &devices);
    const struct mesh_ui_known_device wanted = device_entry(identifier, kind);
    return inkstand_recent_rank(&devices, &wanted);
}

bool mesh_ui_preferences_note_device(struct mesh_ui_preferences *prefs, const char *identifier,
                                     uint8_t kind) {
    if (prefs == NULL || identifier == NULL || identifier[0] == '\0') {
        return false;
    }

    /* By value before anything moves: the identifier may well point into the list this is about
       to shift, or at preferred_device itself, which would make the write below an overlapping
       copy. */
    const struct mesh_ui_known_device wanted = device_entry(identifier, kind);

    bool changed = false;
    if (strcmp(prefs->preferred_device, wanted.identifier) != 0 ||
        prefs->preferred_device_kind != kind) {
        snprintf(prefs->preferred_device, sizeof prefs->preferred_device, "%s", wanted.identifier);
        prefs->preferred_device_kind = kind;
        changed = true;
    }

    struct inkstand_recent devices;
    devices_list(prefs, &devices);
    if (inkstand_recent_note(&devices, &wanted)) {
        changed = true;
    }
    prefs->known_device_count = (uint8_t)devices.count;
    return changed;
}

bool mesh_ui_preferences_forget_device(struct mesh_ui_preferences *prefs, const char *identifier,
                                       uint8_t kind) {
    if (prefs == NULL || identifier == NULL || identifier[0] == '\0') {
        return false;
    }

    /* By value before anything moves, as note_device does: the natural way to call this is with
       a pointer to the entry being dropped, and the shift would leave that pointing at the radio
       that slid into its place - so the head test at the end would ask about the wrong one. */
    const struct mesh_ui_known_device wanted = device_entry(identifier, kind);

    struct inkstand_recent devices;
    devices_list(prefs, &devices);
    if (!inkstand_recent_forget(&devices, &wanted)) {
        return false;
    }
    prefs->known_device_count = (uint8_t)devices.count;

    /* A radio whose pairing is gone is not the one to reconnect to. The next device in the
       list takes the head, which is the same answer auto-connect would reach anyway. */
    if (prefs->preferred_device[0] != '\0' &&
        device_same(&wanted, prefs->preferred_device, prefs->preferred_device_kind)) {
        if (prefs->known_device_count > 0U) {
            snprintf(prefs->preferred_device, sizeof prefs->preferred_device, "%s",
                     prefs->known_devices[0].identifier);
            prefs->preferred_device_kind = prefs->known_devices[0].kind;
        } else {
            prefs->preferred_device[0] = '\0';
            prefs->preferred_device_kind = 0U;
        }
    }
    return true;
}

/* "kind:identifier". The kind comes first because a BLE address is full of colons and a tty path
   is full of slashes; only the first colon on an entry is the separator. */
static bool parse_device(const char *text, size_t len, void *entry, void *context) {
    (void)context;
    const char *separator = memchr(text, ':', len);
    if (separator == NULL || separator + 1 == text + len) {
        return false;
    }
    struct mesh_ui_known_device *device = entry;
    const size_t kind_len = (size_t)(separator - text);
    device->kind =
        (uint8_t)(kind_len == 6U && memcmp(text, "serial", 6U) == 0 ? MESH_UI_PREFS_KIND_SERIAL
                                                                    : 0U);
    const size_t id_len = len - kind_len - 1U;
    const size_t copy_len =
        id_len < sizeof device->identifier - 1U ? id_len : sizeof device->identifier - 1U;
    memcpy(device->identifier, separator + 1, copy_len);
    device->identifier[copy_len] = '\0';
    return true;
}

static int write_device(FILE *out, const void *entry, void *context) {
    (void)context;
    const struct mesh_ui_known_device *device = entry;
    return fprintf(out, "%s:%s", device->kind == MESH_UI_PREFS_KIND_SERIAL ? "serial" : "ble",
                   device->identifier) < 0
               ? -EIO
               : 0;
}

/* A node number in decimal. Node 0 is not a node, and is dropped like anything unreadable. */
static bool parse_radio(const char *text, size_t len, void *entry, void *context) {
    (void)context;
    char digits[16];
    if (len == 0U || len >= sizeof digits || text[0] < '0' || text[0] > '9') {
        return false;
    }
    memcpy(digits, text, len);
    digits[len] = '\0';
    char *end = NULL;
    const unsigned long parsed = strtoul(digits, &end, 10);
    if (end != digits + len || parsed == 0UL || parsed > UINT32_MAX) {
        return false;
    }
    *(uint32_t *)entry = (uint32_t)parsed;
    return true;
}

static int write_radio(FILE *out, const void *entry, void *context) {
    (void)context;
    return fprintf(out, "%" PRIu32, *(const uint32_t *)entry) < 0 ? -EIO : 0;
}

int mesh_ui_preferences_load(struct mesh_ui_preferences *prefs, const char *path) {
    if (prefs == NULL || path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    mesh_ui_preferences_init(prefs);

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -errno;
    }

    /* One known_devices line can carry eight identifiers of 64 bytes; a split line loses its
       tail to the "no = here" test above, which would silently shorten the list. */
    char line[640];
    while (fgets(line, sizeof line, file) != NULL) {
        strip_newline(line);
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }

        const char *delimiter = strchr(line, '=');
        if (delimiter == NULL) {
            continue;
        }

        size_t key_len = (size_t)(delimiter - line);
        const char *value = delimiter + 1;

        if (strncmp(line, "preferred_device", key_len) == 0) {
            snprintf(prefs->preferred_device, sizeof prefs->preferred_device, "%s", value);
        } else if (strncmp(line, "preferred_device_kind", key_len) == 0) {
            prefs->preferred_device_kind = (uint8_t)(strcmp(value, "serial") == 0 ? 1 : 0);
        } else if (strncmp(line, "network_host", key_len) == 0) {
            snprintf(prefs->network_host, sizeof prefs->network_host, "%s", value);
        } else if (strncmp(line, "preferred_channel", key_len) == 0) {
            snprintf(prefs->preferred_channel, sizeof prefs->preferred_channel, "%s", value);
        } else if (strncmp(line, "update_channel", key_len) == 0) {
            /* Written by name so the file stays readable and an enum renumbering cannot
               silently move someone onto the other channel. An unknown name is DEFAULT. */
            if (strcmp(value, "stable") == 0) {
                prefs->update_channel = 1U;
            } else if (strcmp(value, "prerelease") == 0) {
                prefs->update_channel = 2U;
            } else {
                prefs->update_channel = 0U;
            }
        } else if (strncmp(line, "firmware_channel", key_len) == 0) {
            /* By name, for the reason update_channel is. Anything unrecognised is stable,
               which is the channel that cannot surprise anybody. */
            prefs->firmware_channel = (uint8_t)(strcmp(value, "alpha") == 0 ? 1 : 0);
        } else if (key_len == 8U && strncmp(line, "language", key_len) == 0) {
            snprintf(prefs->language, sizeof prefs->language, "%s", value);
        } else if (strncmp(line, "theme", key_len) == 0) {
            /* Taken as written. An id no build knows resolves to the default when it is looked
               up, so a file from a newer version that had more themes degrades rather than
               failing, and keeps the name in case that version comes back. */
            snprintf(prefs->theme, sizeof prefs->theme, "%s", value);
        } else if (key_len == 9U && strncmp(line, "text_size", key_len) == 0) {
            /* Anything unrecognised is standard, which is the size nobody has to go looking
               for a way back from. */
            prefs->text_size = (int8_t)(strcmp(value, "small") == 0   ? MESH_UI_TEXT_SIZE_SMALL
                                        : strcmp(value, "large") == 0 ? MESH_UI_TEXT_SIZE_LARGE
                                                                      : MESH_UI_TEXT_SIZE_STANDARD);
        } else if (strncmp(line, "update_allow_dev", key_len) == 0) {
            prefs->update_allow_dev = strcmp(value, "1") == 0;
        } else if (strncmp(line, "known_devices", key_len) == 0) {
            /* Most recent first - the order is the value here, so it is read back in file order
               rather than through note_device(). */
            struct inkstand_recent devices;
            devices_list(prefs, &devices);
            prefs->known_device_count =
                (uint8_t)inkstand_recent_parse(&devices, value, ',', parse_device, NULL);
        } else if (strncmp(line, "known_radios", key_len) == 0) {
            /* One comma-separated line, most recent first, read back in file order for the same
               reason. */
            struct inkstand_recent radios;
            radios_list(prefs, &radios);
            prefs->known_radio_count =
                (uint8_t)inkstand_recent_parse(&radios, value, ',', parse_radio, NULL);
        }
    }

    fclose(file);

    /* Files written before the kind was recorded hold a bare identifier. A tty path can only
       have come from the serial link, so migrate rather than handing it to BLE. */
    if (prefs->preferred_device_kind == 0U && prefs->preferred_device[0] == '/') {
        prefs->preferred_device_kind = MESH_UI_PREFS_KIND_SERIAL;
    }

    /* A file written before the list existed carries one radio in preferred_device, and it is
       genuinely the most recent one. Seeding the list from it means the first launch after an
       update already knows about the node you were on, rather than starting empty and treating
       it as a stranger. The two are the same fact and the head is where they meet, so this also
       repairs a file whose list somehow lost its own head. */
    if (prefs->preferred_device[0] != '\0' &&
        mesh_ui_preferences_device_rank(prefs, prefs->preferred_device,
                                        prefs->preferred_device_kind) != 0) {
        (void)mesh_ui_preferences_note_device(prefs, prefs->preferred_device,
                                              prefs->preferred_device_kind);
    }
    return 0;
}

static int ensure_parent_directory(const char *path) {
    char directory[PATH_MAX];
    snprintf(directory, sizeof directory, "%s", path);
    char *last_slash = strrchr(directory, '/');
    if (last_slash == NULL) {
        return 0;
    }
    *last_slash = '\0';

    if (directory[0] == '\0') {
        return 0;
    }

    struct stat st;
    if (stat(directory, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            inkwell_log_warn("ui", "%s exists but is not a directory", directory);
            return -ENOTDIR;
        }
        return 0;
    }

    if (errno != ENOENT) {
        inkwell_log_warn("ui", "Failed to stat %s: %s", directory, strerror(errno));
    }

    const int created = inkwell_file_mkdir(directory);
    if (created < 0 && created != -EEXIST) {
        inkwell_log_warn("ui", "Failed to create %s: %s", directory, strerror(-created));
        return created;
    }

    return 0;
}

int mesh_ui_preferences_save(const struct mesh_ui_preferences *prefs, const char *path) {
    if (prefs == NULL || path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    int ensure_result = ensure_parent_directory(path);
    if (ensure_result < 0) {
        return ensure_result;
    }

    FILE *file = fopen(path, "w");
    if (file == NULL) {
        inkwell_log_warn("ui", "Failed to open %s for writing: %s", path, strerror(errno));
        return -errno;
    }

    fprintf(file, "preferred_device=%s\n", prefs->preferred_device);
    fprintf(file, "preferred_device_kind=%s\n",
            prefs->preferred_device_kind == 1U ? "serial" : "ble");
    fprintf(file, "network_host=%s\n", prefs->network_host);
    fprintf(file, "preferred_channel=%s\n", prefs->preferred_channel);
    fprintf(file, "update_channel=%s\n",
            prefs->update_channel == 1U   ? "stable"
            : prefs->update_channel == 2U ? "prerelease"
                                          : "default");
    fprintf(file, "update_allow_dev=%s\n", prefs->update_allow_dev ? "1" : "0");
    fprintf(file, "firmware_channel=%s\n", prefs->firmware_channel == 1U ? "alpha" : "stable");
    fprintf(file, "theme=%s\n", prefs->theme);
    fprintf(file, "text_size=%s\n",
            prefs->text_size == MESH_UI_TEXT_SIZE_SMALL   ? "small"
            : prefs->text_size == MESH_UI_TEXT_SIZE_LARGE ? "large"
                                                          : "standard");
    fprintf(file, "language=%s\n", prefs->language);
    struct inkstand_recent devices;
    devices_list(prefs, &devices);
    fprintf(file, "known_devices=");
    (void)inkstand_recent_write(&devices, file, ',', write_device, NULL);
    fputc('\n', file);
    struct inkstand_recent radios;
    radios_list(prefs, &radios);
    fprintf(file, "known_radios=");
    (void)inkstand_recent_write(&radios, file, ',', write_radio, NULL);
    fputc('\n', file);

    fclose(file);
    return 0;
}
