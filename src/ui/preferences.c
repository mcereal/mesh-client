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
        } else if (strncmp(line, "preferred_channel", key_len) == 0) {
            snprintf(prefs->preferred_channel, sizeof prefs->preferred_channel, "%s", value);
        }
    }

    fclose(file);
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
    fprintf(file, "preferred_channel=%s\n", prefs->preferred_channel);

    fclose(file);
    return 0;
}
