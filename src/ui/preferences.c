#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/preferences.h"

#include "mesh/log.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

int mesh_ui_preferences_load(struct mesh_ui_preferences *prefs, const char *path) {
    if (prefs == NULL || path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    mesh_ui_preferences_init(prefs);

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -errno;
    }

    char line[256];
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
        prefs->preferred_device_kind = 1U;
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
    fprintf(file, "known_radios=");
    for (uint8_t i = 0; i < prefs->known_radio_count && i < MESH_UI_MAX_KNOWN_RADIOS; ++i) {
        fprintf(file, "%s%u", i > 0U ? "," : "", prefs->known_radios[i]);
    }
    fputc('\n', file);

    fclose(file);
    return 0;
}
