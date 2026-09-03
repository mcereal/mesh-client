#pragma once

#include "mesh/ui/backend.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;

enum { MESH_UI_MINUI_PATH_CAP = 256 };

struct mesh_ui_backend_minui_context {
    bool warned_list_missing;
    bool warned_presenter_missing;
    bool list_running;
    pid_t list_pid;
    int list_stdout_fd;
    char list_json_path[MESH_UI_MINUI_PATH_CAP];
    char list_output[2048];
    size_t list_output_len;
    struct mesh_ui_snapshot last_snapshot;
    struct mesh_ui_snapshot list_snapshot;
    bool has_snapshot;
    bool list_snapshot_valid;
    struct mesh_event_loop *loop;
    void (*on_device_selected)(void *userdata, const char *identifier);
    void *callback_userdata;
};

const struct mesh_ui_backend *mesh_ui_backend_minui(void);
bool mesh_ui_backend_minui_is_available(void);
int mesh_ui_backend_minui_format_menu(const struct mesh_ui_snapshot *snapshot, char *buffer,
                                      size_t buffer_len);

#ifdef __cplusplus
}
#endif
