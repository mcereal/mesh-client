#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/preferences.h"

#include "mesh/utils/log.h"

#include <errno.h>
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
            mesh_log_warn("ui", "Failed to stat %s: %s", buffer, strerror(errno));
        }
        if (mkdir(buffer, 0700) < 0 && errno != EEXIST) {
            mesh_log_warn("ui", "Failed to create %s: %s", buffer, strerror(errno));
            return -errno;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        mesh_log_warn("ui", "%s exists but is not a directory", buffer);
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

bool mesh_ui_preferences_knows_radio(const struct mesh_ui_preferences *prefs, uint32_t node_num) {
    if (prefs == NULL || node_num == 0U) {
        return false;
    }
    for (uint8_t i = 0; i < prefs->known_radio_count && i < MESH_UI_MAX_KNOWN_RADIOS; ++i) {
        if (prefs->known_radios[i] == node_num) {
            return true;
        }
    }
    return false;
}

bool mesh_ui_preferences_note_radio(struct mesh_ui_preferences *prefs, uint32_t node_num) {
    if (prefs == NULL || node_num == 0U) {
        return false;
    }
    if (prefs->known_radio_count > MESH_UI_MAX_KNOWN_RADIOS) {
        prefs->known_radio_count = MESH_UI_MAX_KNOWN_RADIOS;
    }
    if (prefs->known_radio_count > 0U && prefs->known_radios[0] == node_num) {
        return false; /* already the most recent; the common case, every publish */
    }

    /* Slide everything ahead of the existing entry down by one and put this radio in front.
       A radio we have not seen before pushes the oldest one off the end. */
    uint8_t existing = prefs->known_radio_count;
    for (uint8_t i = 0; i < prefs->known_radio_count; ++i) {
        if (prefs->known_radios[i] == node_num) {
            existing = i;
            break;
        }
    }
    uint8_t shift_from = existing;
    if (existing == prefs->known_radio_count) {
        if (prefs->known_radio_count < MESH_UI_MAX_KNOWN_RADIOS) {
            prefs->known_radio_count++;
        }
        shift_from = (uint8_t)(prefs->known_radio_count - 1U);
    }
    for (uint8_t i = shift_from; i > 0U; --i) {
        prefs->known_radios[i] = prefs->known_radios[i - 1U];
    }
    prefs->known_radios[0] = node_num;
    return true;
}

/* One identity test for a device, shared by the three list operations below. A BLE address is
   case-insensitive (BlueZ writes them upper-case, a config file may not); a tty path is not. */
static bool device_matches(const struct mesh_ui_known_device *entry, const char *identifier,
                           uint8_t kind) {
    if (entry->kind != kind) {
        return false;
    }
    return kind == MESH_UI_PREFS_KIND_SERIAL ? strcmp(entry->identifier, identifier) == 0
                                             : strcasecmp(entry->identifier, identifier) == 0;
}

int mesh_ui_preferences_device_rank(const struct mesh_ui_preferences *prefs, const char *identifier,
                                    uint8_t kind) {
    if (prefs == NULL || identifier == NULL || identifier[0] == '\0') {
        return -1;
    }
    const uint8_t count = prefs->known_device_count < MESH_UI_MAX_KNOWN_DEVICES
                              ? prefs->known_device_count
                              : MESH_UI_MAX_KNOWN_DEVICES;
    for (uint8_t i = 0; i < count; ++i) {
        if (device_matches(&prefs->known_devices[i], identifier, kind)) {
            return (int)i;
        }
    }
    return -1;
}

bool mesh_ui_preferences_note_device(struct mesh_ui_preferences *prefs, const char *identifier,
                                     uint8_t kind) {
    if (prefs == NULL || identifier == NULL || identifier[0] == '\0') {
        return false;
    }
    if (prefs->known_device_count > MESH_UI_MAX_KNOWN_DEVICES) {
        prefs->known_device_count = MESH_UI_MAX_KNOWN_DEVICES;
    }

    /* By value before anything moves, for the same reason forget_device does it: the identifier
       may well point into the list this is about to shift, or at preferred_device itself, which
       would make the write below an overlapping copy. */
    char wanted[sizeof prefs->known_devices[0].identifier];
    snprintf(wanted, sizeof wanted, "%s", identifier);
    identifier = wanted;

    bool changed = false;
    if (strcmp(prefs->preferred_device, identifier) != 0 || prefs->preferred_device_kind != kind) {
        snprintf(prefs->preferred_device, sizeof prefs->preferred_device, "%s", identifier);
        prefs->preferred_device_kind = kind;
        changed = true;
    }

    if (prefs->known_device_count > 0U &&
        device_matches(&prefs->known_devices[0], identifier, kind)) {
        return changed; /* already the most recent; the common case, every publish */
    }

    /* Slide everything ahead of the existing entry down by one and put this device in front.
       A radio we have not connected to before pushes the oldest one off the end. */
    uint8_t existing = prefs->known_device_count;
    for (uint8_t i = 0; i < prefs->known_device_count; ++i) {
        if (device_matches(&prefs->known_devices[i], identifier, kind)) {
            existing = i;
            break;
        }
    }
    uint8_t shift_from = existing;
    if (existing == prefs->known_device_count) {
        if (prefs->known_device_count < MESH_UI_MAX_KNOWN_DEVICES) {
            prefs->known_device_count++;
        }
        shift_from = (uint8_t)(prefs->known_device_count - 1U);
    }
    for (uint8_t i = shift_from; i > 0U; --i) {
        prefs->known_devices[i] = prefs->known_devices[i - 1U];
    }
    snprintf(prefs->known_devices[0].identifier, sizeof prefs->known_devices[0].identifier, "%s",
             identifier);
    prefs->known_devices[0].kind = kind;
    return true;
}

bool mesh_ui_preferences_forget_device(struct mesh_ui_preferences *prefs, const char *identifier,
                                       uint8_t kind) {
    if (prefs == NULL || identifier == NULL || identifier[0] == '\0') {
        return false;
    }
    const int rank = mesh_ui_preferences_device_rank(prefs, identifier, kind);
    if (rank < 0) {
        return false;
    }

    /* By value before anything moves, as note_device does: the natural way to call this is with
       a pointer to the entry being dropped, and the shift below would leave that pointing at the
       radio that slid into its place - so the head test at the end would ask about the wrong
       one. */
    char wanted[sizeof prefs->known_devices[0].identifier];
    snprintf(wanted, sizeof wanted, "%s", identifier);
    identifier = wanted;

    for (uint8_t i = (uint8_t)rank; i + 1U < prefs->known_device_count; ++i) {
        prefs->known_devices[i] = prefs->known_devices[i + 1U];
    }
    if (prefs->known_device_count > 0U) {
        prefs->known_device_count--;
    }
    memset(&prefs->known_devices[prefs->known_device_count], 0,
           sizeof prefs->known_devices[prefs->known_device_count]);

    /* A radio whose pairing is gone is not the one to reconnect to. The next device in the
       list takes the head, which is the same answer auto-connect would reach anyway. */
    if (prefs->preferred_device[0] != '\0' && prefs->preferred_device_kind == kind &&
        (kind == MESH_UI_PREFS_KIND_SERIAL
             ? strcmp(prefs->preferred_device, identifier) == 0
             : strcasecmp(prefs->preferred_device, identifier) == 0)) {
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
        } else if (key_len == 8U && strncmp(line, "language", key_len) == 0) {
            snprintf(prefs->language, sizeof prefs->language, "%s", value);
        } else if (strncmp(line, "theme", key_len) == 0) {
            /* Taken as written. An id no build knows resolves to the default when it is looked
               up, so a file from a newer version that had more themes degrades rather than
               failing, and keeps the name in case that version comes back. */
            snprintf(prefs->theme, sizeof prefs->theme, "%s", value);
        } else if (strncmp(line, "update_allow_dev", key_len) == 0) {
            prefs->update_allow_dev = strcmp(value, "1") == 0;
        } else if (strncmp(line, "known_devices", key_len) == 0) {
            /* "kind:identifier" entries, comma separated, most recent first - the order is the
               value here, so it is read back in file order rather than through note_device().
               The kind comes first because a BLE address is full of colons and a tty path is
               full of slashes; only the first colon on an entry is the separator. */
            prefs->known_device_count = 0U;
            const char *cursor = value;
            while (*cursor != '\0' && prefs->known_device_count < MESH_UI_MAX_KNOWN_DEVICES) {
                const char *comma = strchr(cursor, ',');
                const size_t entry_len = comma != NULL ? (size_t)(comma - cursor) : strlen(cursor);
                char entry[80];
                const size_t copy_len =
                    entry_len < sizeof entry - 1U ? entry_len : sizeof entry - 1U;
                memcpy(entry, cursor, copy_len);
                entry[copy_len] = '\0';

                char *separator = strchr(entry, ':');
                if (separator != NULL && separator[1] != '\0') {
                    *separator = '\0';
                    const uint8_t kind =
                        (uint8_t)(strcmp(entry, "serial") == 0 ? MESH_UI_PREFS_KIND_SERIAL : 0U);
                    const char *identifier = separator + 1;
                    if (mesh_ui_preferences_device_rank(prefs, identifier, kind) < 0) {
                        struct mesh_ui_known_device *slot =
                            &prefs->known_devices[prefs->known_device_count++];
                        snprintf(slot->identifier, sizeof slot->identifier, "%s", identifier);
                        slot->kind = kind;
                    }
                }
                cursor = comma != NULL ? comma + 1 : cursor + entry_len;
            }
        } else if (strncmp(line, "known_radios", key_len) == 0) {
            /* One comma-separated line, most recent first - the order is the value here, so
               it is read back in file order rather than through note_radio(). */
            prefs->known_radio_count = 0U;
            const char *cursor = value;
            while (*cursor != '\0' && prefs->known_radio_count < MESH_UI_MAX_KNOWN_RADIOS) {
                char *end = NULL;
                const unsigned long parsed = strtoul(cursor, &end, 10);
                if (end == cursor) {
                    break;
                }
                if (parsed != 0UL && !mesh_ui_preferences_knows_radio(prefs, (uint32_t)parsed)) {
                    prefs->known_radios[prefs->known_radio_count++] = (uint32_t)parsed;
                }
                cursor = (*end == ',') ? end + 1 : end;
            }
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
            mesh_log_warn("ui", "%s exists but is not a directory", directory);
            return -ENOTDIR;
        }
        return 0;
    }

    if (errno != ENOENT) {
        mesh_log_warn("ui", "Failed to stat %s: %s", directory, strerror(errno));
    }

    if (mkdir(directory, 0700) < 0 && errno != EEXIST) {
        mesh_log_warn("ui", "Failed to create %s: %s", directory, strerror(errno));
        return -errno;
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
        mesh_log_warn("ui", "Failed to open %s for writing: %s", path, strerror(errno));
        return -errno;
    }

    fprintf(file, "preferred_device=%s\n", prefs->preferred_device);
    fprintf(file, "preferred_device_kind=%s\n",
            prefs->preferred_device_kind == 1U ? "serial" : "ble");
    fprintf(file, "preferred_channel=%s\n", prefs->preferred_channel);
    fprintf(file, "update_channel=%s\n",
            prefs->update_channel == 1U   ? "stable"
            : prefs->update_channel == 2U ? "prerelease"
                                          : "default");
    fprintf(file, "update_allow_dev=%s\n", prefs->update_allow_dev ? "1" : "0");
    fprintf(file, "theme=%s\n", prefs->theme);
    fprintf(file, "language=%s\n", prefs->language);
    fprintf(file, "known_devices=");
    for (uint8_t i = 0; i < prefs->known_device_count && i < MESH_UI_MAX_KNOWN_DEVICES; ++i) {
        fprintf(file, "%s%s:%s", i > 0U ? "," : "",
                prefs->known_devices[i].kind == MESH_UI_PREFS_KIND_SERIAL ? "serial" : "ble",
                prefs->known_devices[i].identifier);
    }
    fputc('\n', file);
    fprintf(file, "known_radios=");
    for (uint8_t i = 0; i < prefs->known_radio_count && i < MESH_UI_MAX_KNOWN_RADIOS; ++i) {
        fprintf(file, "%s%u", i > 0U ? "," : "", prefs->known_radios[i]);
    }
    fputc('\n', file);

    fclose(file);
    return 0;
}
