#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/backends/minui.h"

#include "mesh/log.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *k_minui_presenter = "minui-presenter";
static const char *k_minui_list = "minui-list";
static const char *k_minui_keyboard = "minui-keyboard";

static const char *get_env_or_default(const char *env_name, const char *fallback) {
    const char *value = getenv(env_name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    return value;
}

static bool command_seems_available(const char *command) {
    if (command == NULL || command[0] == '\0') {
        return false;
    }
    if (strchr(command, ' ') != NULL) {
        return true; // treat as shell fragment
    }
    if (strchr(command, '/') != NULL) {
        return access(command, X_OK) == 0;
    }
    // No path separator; assume located via PATH.
    return true;
}

bool mesh_ui_backend_minui_is_available(void) {
    const char *presenter = get_env_or_default("MESHCLIENT_MINUI_PRESENTER_CMD", k_minui_presenter);
    const char *list = get_env_or_default("MESHCLIENT_MINUI_LIST_CMD", k_minui_list);
    if (command_seems_available(presenter) || command_seems_available(list)) {
        return true;
    }
    return access(k_minui_keyboard, X_OK) == 0;
}

static int run_command_with_input(const char *command, const char *input) {
    if (command == NULL || command[0] == '\0') {
        return -EINVAL;
    }

    int pipe_fd[2];
    if (pipe(pipe_fd) < 0) {
        return -errno;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int err = errno;
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return -err;
    }

    if (pid == 0) {
        // child
        dup2(pipe_fd[0], STDIN_FILENO);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(pipe_fd[0]);

    if (input != NULL && input[0] != '\0') {
        size_t len = strlen(input);
        ssize_t written = write(pipe_fd[1], input, len);
        (void)written;
    }
    close(pipe_fd[1]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -errno;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }

    return -EIO;
}

static void minui_present_devices(struct mesh_ui_backend_minui_context *context,
                                  const struct mesh_ui_snapshot *snapshot) {
    const char *command = get_env_or_default("MESHCLIENT_MINUI_LIST_CMD", k_minui_list);
    if (!command_seems_available(command)) {
        if (!context->warned_list_missing) {
            mesh_log_warn("ui", "minui-list helper not available; skipping device list update");
            context->warned_list_missing = true;
        }
        return;
    }

    char buffer[1024];
    size_t offset = 0U;

    for (size_t i = 0; i < snapshot->device_count; ++i) {
        const struct mesh_ui_device *device = &snapshot->devices[i];
        int written = snprintf(buffer + offset, sizeof buffer - offset, "%s%s (%s) RSSI=%d\n",
                               device->connected ? "* " : "", device->name[0] != '\0' ? device->name : "<unknown>",
                               device->identifier[0] != '\0' ? device->identifier : "<unknown>",
                               (int)device->rssi);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= sizeof buffer - offset) {
            offset = sizeof buffer - 1U;
            buffer[offset] = '\0';
            break;
        }
        offset += (size_t)written;
    }

    if (offset == 0U) {
        snprintf(buffer, sizeof buffer, "No Meshtastic devices found\n");
    }

    int result = run_command_with_input(command, buffer);
    if (result < 0 && !context->warned_list_missing) {
        mesh_log_warn("ui", "minui-list command failed (cmd=%s, err=%d)", command, result);
        context->warned_list_missing = true;
    }
}

static void minui_present_handshake(struct mesh_ui_backend_minui_context *context,
                                    const struct mesh_ui_snapshot *snapshot) {
    const char *command = get_env_or_default("MESHCLIENT_MINUI_PRESENTER_CMD", k_minui_presenter);
    if (!command_seems_available(command)) {
        if (!context->warned_presenter_missing) {
            mesh_log_warn("ui", "minui-presenter helper not available; skipping status toast");
            context->warned_presenter_missing = true;
        }
        return;
    }

    char buffer[256];
    if (!snapshot->handshake_valid) {
        snprintf(buffer, sizeof buffer, "Mesh handshake pending...\n");
    } else {
        snprintf(buffer, sizeof buffer, "NodeDB=%" PRIu32 " %s%s\n",
                 snapshot->handshake.node_count,
                 snapshot->handshake.config_complete ? "config ok" : "config pending",
                 snapshot->handshake.request_in_flight ? " (request in flight)" : "");
    }

    int result = run_command_with_input(command, buffer);
    if (result < 0 && !context->warned_presenter_missing) {
        mesh_log_warn("ui", "minui-presenter command failed (cmd=%s, err=%d)", command, result);
        context->warned_presenter_missing = true;
    }
}

static int mesh_ui_backend_minui_init(void **state, void *userdata) {
    struct mesh_ui_backend_minui_context *context = (struct mesh_ui_backend_minui_context *)userdata;
    if (context != NULL) {
        memset(context, 0, sizeof *context);
    }
    if (state != NULL) {
        *state = context;
    }
    mesh_log_info("ui", "MinUI backend initialised");
    return 0;
}

static void mesh_ui_backend_minui_shutdown(void *state, void *userdata) {
    (void)state;
    (void)userdata;
}

static void mesh_ui_backend_minui_present(void *state, const struct mesh_ui_snapshot *snapshot, void *userdata) {
    if (snapshot == NULL) {
        return;
    }
    struct mesh_ui_backend_minui_context *context = NULL;
    if (state != NULL) {
        context = (struct mesh_ui_backend_minui_context *)state;
    } else if (userdata != NULL) {
        context = (struct mesh_ui_backend_minui_context *)userdata;
    }
    if (context == NULL) {
        return;
    }

    if ((snapshot->update_flags & MESH_UI_UPDATE_DISCOVERY) != 0U) {
        minui_present_devices(context, snapshot);
    }
    if ((snapshot->update_flags & MESH_UI_UPDATE_HANDSHAKE) != 0U) {
        minui_present_handshake(context, snapshot);
    }
}

const struct mesh_ui_backend *mesh_ui_backend_minui(void) {
    static const struct mesh_ui_backend k_backend = {
        .name = "minui",
        .init = mesh_ui_backend_minui_init,
        .shutdown = mesh_ui_backend_minui_shutdown,
        .present = mesh_ui_backend_minui_present,
    };
    return &k_backend;
}
