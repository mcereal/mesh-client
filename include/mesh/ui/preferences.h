#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_preferences {
    char preferred_device[64];
    char preferred_channel[64];
};

int mesh_ui_preferences_default_path(char *buffer, size_t buffer_len);
int mesh_ui_preferences_load(struct mesh_ui_preferences *prefs, const char *path);
int mesh_ui_preferences_save(const struct mesh_ui_preferences *prefs, const char *path);

#ifdef __cplusplus
}
#endif

