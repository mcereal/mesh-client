#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/backends/minui.h"

#include "mesh/event_loop.h"
#include "mesh/log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *k_minui_presenter = "minui-presenter";
static const char *k_minui_list = "minui-list";

static const char *get_env_or_default(const char *env_name, const char *fallback) {
    const char *value = getenv(env_name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    return value;
}

static bool command_in_path(const char *command) {
    if (command == NULL || command[0] == '\0') {
        return false;
    }

    if (strchr(command, '/') != NULL) {
        return access(command, X_OK) == 0;
    }

    const char *path = getenv("PATH");
    if (path == NULL || path[0] == '\0') {
        path = "/usr/bin:/bin";
    }

    char *mutable_path = strdup(path);
    if (mutable_path == NULL) {
        return false;
    }

    bool found = false;
    char *saveptr = NULL;
    for (char *token = strtok_r(mutable_path, ":", &saveptr); token != NULL; token = strtok_r(NULL, ":", &saveptr)) {
        size_t token_len = strlen(token);
        if (token_len == 0U) {
            continue;
        }
        char candidate[MESH_UI_MINUI_PATH_CAP];
        int written = snprintf(candidate, sizeof candidate, "%s/%s", token, command);
        if (written < 0) {
            continue;
        }
        if ((size_t)written >= sizeof candidate) {
            continue;
        }
        if (access(candidate, X_OK) == 0) {
            found = true;
            break;
        }
    }

    free(mutable_path);
    return found;
}

bool mesh_ui_backend_minui_is_available(void) {
    const char *list_cmd = get_env_or_default("MESHCLIENT_MINUI_LIST_CMD", k_minui_list);
    if (!command_in_path(list_cmd)) {
        return false;
    }
    return true;
}

struct minui_buffer {
    char *data;
    size_t capacity;
    size_t length;
};

static void minui_buffer_init(struct minui_buffer *buffer, char *storage, size_t storage_len) {
    buffer->data = storage;
    buffer->capacity = storage_len;
    buffer->length = 0U;
    if (storage_len > 0U) {
        buffer->data[0] = '\0';
    }
}

static bool minui_buffer_append_data(struct minui_buffer *buffer, const char *data, size_t len) {
    if (buffer->length + len >= buffer->capacity) {
        return false;
    }
    memcpy(buffer->data + buffer->length, data, len);
    buffer->length += len;
    buffer->data[buffer->length] = '\0';
    return true;
}

static bool minui_buffer_append_char(struct minui_buffer *buffer, char value) {
    return minui_buffer_append_data(buffer, &value, 1U);
}

static bool minui_buffer_append_literal(struct minui_buffer *buffer, const char *literal) {
    return minui_buffer_append_data(buffer, literal, strlen(literal));
}

static bool minui_buffer_append_format(struct minui_buffer *buffer, const char *fmt, ...) {
    if (buffer->capacity == 0U || buffer->length >= buffer->capacity - 1U) {
        return false;
    }

    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);

    int required = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (required < 0) {
        va_end(args);
        return false;
    }

    if (buffer->length + (size_t)required >= buffer->capacity) {
        va_end(args);
        return false;
    }

    int written = vsnprintf(buffer->data + buffer->length, buffer->capacity - buffer->length, fmt, args);
    va_end(args);
    if (written < 0) {
        return false;
    }
    buffer->length += (size_t)written;
    return true;
}

static bool minui_buffer_append_string(struct minui_buffer *buffer, const char *value) {
    if (value == NULL) {
        value = "";
    }
    if (!minui_buffer_append_char(buffer, '"')) {
        return false;
    }

    for (const unsigned char *ptr = (const unsigned char *)value; *ptr != '\0'; ++ptr) {
        unsigned char ch = *ptr;
        if (ch == '\"' || ch == '\\') {
            char escaped[3] = {'\\', (char)ch, '\0'};
            if (!minui_buffer_append_literal(buffer, escaped)) {
                return false;
            }
        } else if (ch == '\b') {
            if (!minui_buffer_append_literal(buffer, "\\b")) {
                return false;
            }
        } else if (ch == '\f') {
            if (!minui_buffer_append_literal(buffer, "\\f")) {
                return false;
            }
        } else if (ch == '\n') {
            if (!minui_buffer_append_literal(buffer, "\\n")) {
                return false;
            }
        } else if (ch == '\r') {
            if (!minui_buffer_append_literal(buffer, "\\r")) {
                return false;
            }
        } else if (ch == '\t') {
            if (!minui_buffer_append_literal(buffer, "\\t")) {
                return false;
            }
        } else if (ch < 0x20U) {
            char unicode[7];
            int written = snprintf(unicode, sizeof unicode, "\\u%04x", ch);
            if (written < 0 || (size_t)written >= sizeof unicode) {
                return false;
            }
            if (!minui_buffer_append_literal(buffer, unicode)) {
                return false;
            }
        } else {
            if (!minui_buffer_append_char(buffer, (char)ch)) {
                return false;
            }
        }
    }

    return minui_buffer_append_char(buffer, '"');
}

int mesh_ui_backend_minui_format_menu(const struct mesh_ui_snapshot *snapshot, char *buffer, size_t buffer_len) {
    if (snapshot == NULL || buffer == NULL || buffer_len == 0U) {
        return -EINVAL;
    }

    struct minui_buffer builder;
    minui_buffer_init(&builder, buffer, buffer_len);

    if (!minui_buffer_append_literal(&builder, "{\"settings\":[{\"name\":\"Devices\",\"options\":")) {
        return -ENOSPC;
    }
    if (!minui_buffer_append_literal(&builder, "[")) {
        return -ENOSPC;
    }

    int selected_index = -1;
    if (snapshot->device_count == 0U) {
        if (!minui_buffer_append_string(&builder, "No Meshtastic devices found")) {
            return -ENOSPC;
        }
        selected_index = 0;
    } else {
        for (size_t i = 0; i < snapshot->device_count; ++i) {
            if (i > 0U) {
                if (!minui_buffer_append_literal(&builder, ",")) {
                    return -ENOSPC;
                }
            }
            const struct mesh_ui_device *device = &snapshot->devices[i];
            const char *name = (device->name[0] != '\0') ? device->name : "<unknown>";
            const char *identifier = (device->identifier[0] != '\0') ? device->identifier : "<unknown>";
            char line[192];
            int written = snprintf(line, sizeof line, "%s%s [%s] (%d dBm)",
                                   device->connected ? "★ " : "", name, identifier, (int)device->rssi);
            if (written < 0 || (size_t)written >= sizeof line) {
                return -ENOSPC;
            }
            if (!minui_buffer_append_string(&builder, line)) {
                return -ENOSPC;
            }
            if (device->connected && selected_index < 0) {
                selected_index = (int)i;
            }
        }
        if (selected_index < 0) {
            selected_index = 0;
        }
    }

    if (!minui_buffer_append_literal(&builder, "],\"selected\":")) {
        return -ENOSPC;
    }
    if (!minui_buffer_append_format(&builder, "%d", selected_index)) {
        return -ENOSPC;
    }
    if (!minui_buffer_append_literal(&builder, "}")) {
        return -ENOSPC;
    }

    if (!minui_buffer_append_literal(&builder, ",{\"name\":\"Status\",\"options\":[")) {
        return -ENOSPC;
    }

    if (snapshot->handshake_valid) {
        char status_line[128];
        int written = snprintf(status_line, sizeof status_line, "Nodes: %" PRIu32, snapshot->handshake.node_count);
        if (written < 0 || (size_t)written >= sizeof status_line) {
            return -ENOSPC;
        }
        if (!minui_buffer_append_string(&builder, status_line)) {
            return -ENOSPC;
        }
        if (snapshot->handshake.config_complete) {
            if (!minui_buffer_append_literal(&builder, ",")) {
                return -ENOSPC;
            }
            if (!minui_buffer_append_string(&builder, "Config synchronised")) {
                return -ENOSPC;
            }
        } else {
            if (!minui_buffer_append_literal(&builder, ",")) {
                return -ENOSPC;
            }
            if (!minui_buffer_append_string(&builder, "Config pending")) {
                return -ENOSPC;
            }
        }
        if (snapshot->handshake.my_short_name[0] != '\0') {
            if (!minui_buffer_append_literal(&builder, ",")) {
                return -ENOSPC;
            }
            char name_line[128];
            written = snprintf(name_line, sizeof name_line, "Me: %s", snapshot->handshake.my_short_name);
            if (written < 0 || (size_t)written >= sizeof name_line) {
                return -ENOSPC;
            }
            if (!minui_buffer_append_string(&builder, name_line)) {
                return -ENOSPC;
            }
        }
    } else {
        if (!minui_buffer_append_string(&builder, "Waiting for mesh handshake")) {
            return -ENOSPC;
        }
    }

    if (!minui_buffer_append_literal(&builder, "],\"selected\":0,\"features\":{\"unselectable\":true}}")) {
        return -ENOSPC;
    }

    if (!minui_buffer_append_literal(&builder, "]}")) {
        return -ENOSPC;
    }

    return 0;
}

static bool minui_snapshots_equal(const struct mesh_ui_snapshot *a, const struct mesh_ui_snapshot *b) {
    if (a == NULL || b == NULL) {
        return false;
    }
    if (a->device_count != b->device_count) {
        return false;
    }
    if (memcmp(a->devices, b->devices, sizeof(a->devices)) != 0) {
        return false;
    }
    if (a->handshake_valid != b->handshake_valid) {
        return false;
    }
    if (a->handshake_valid && memcmp(&a->handshake, &b->handshake, sizeof(a->handshake)) != 0) {
        return false;
    }
    if (a->update_flags != b->update_flags) {
        return false;
    }
    return true;
}

static int minui_write_menu_file(const char *payload, size_t length, char *path_out, size_t path_len) {
    if (payload == NULL || path_out == NULL || path_len == 0U) {
        return -EINVAL;
    }

    char template_path[] = "/tmp/meshclient-minui-XXXXXX";
    int fd = mkstemp(template_path);
    if (fd < 0) {
        return -errno;
    }

    ssize_t written = write(fd, payload, length);
    if (written < 0 || (size_t)written != length) {
        int err = (written < 0) ? -errno : -EIO;
        close(fd);
        unlink(template_path);
        return err;
    }

    close(fd);
    int copy = snprintf(path_out, path_len, "%s", template_path);
    if (copy < 0 || (size_t)copy >= path_len) {
        unlink(template_path);
        return -ENAMETOOLONG;
    }
    return 0;
}

static int minui_parse_selected_index(const char *json) {
    if (json == NULL) {
        return -1;
    }
    const char *cursor = strstr(json, "\"settings\"");
    if (cursor == NULL) {
        return -1;
    }
    cursor = strstr(cursor, "\"selected\"");
    if (cursor == NULL) {
        return -1;
    }
    cursor = strchr(cursor, ':');
    if (cursor == NULL) {
        return -1;
    }
    ++cursor;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (*cursor == '\0') {
        return -1;
    }
    return atoi(cursor);
}

static void minui_close_stdout_fd(struct mesh_ui_backend_minui_context *context) {
    if (context->list_stdout_fd >= 0) {
        if (context->loop != NULL) {
            mesh_event_loop_remove_fd(context->loop, context->list_stdout_fd);
        }
        close(context->list_stdout_fd);
        context->list_stdout_fd = -1;
    }
}

static void minui_consume_selection(struct mesh_ui_backend_minui_context *context, int status_code) {
    if (context->list_json_path[0] != '\0') {
        unlink(context->list_json_path);
        context->list_json_path[0] = '\0';
    }

    if (!context->list_snapshot_valid) {
        return;
    }

    if (!WIFEXITED(status_code)) {
        return;
    }

    int exit_code = WEXITSTATUS(status_code);
    if (exit_code != 0) {
        return;
    }

    if (context->list_output_len >= sizeof(context->list_output)) {
        context->list_output[sizeof(context->list_output) - 1U] = '\0';
    } else {
        context->list_output[context->list_output_len] = '\0';
    }

    int selected = minui_parse_selected_index(context->list_output);
    if (selected < 0) {
        return;
    }

    if ((size_t)selected >= context->list_snapshot.device_count) {
        return;
    }

    if (context->on_device_selected != NULL) {
        const struct mesh_ui_device *device = &context->list_snapshot.devices[selected];
        if (device->identifier[0] != '\0') {
            context->on_device_selected(context->callback_userdata, device->identifier);
        }
    }
}

static void minui_complete_list(struct mesh_ui_backend_minui_context *context) {
    minui_close_stdout_fd(context);

    int status_code = 0;
    if (context->list_pid > 0) {
        if (waitpid(context->list_pid, &status_code, 0) < 0) {
            mesh_log_warn("ui", "waitpid for minui-list failed: %s", strerror(errno));
        }
    }

    context->list_pid = -1;

    minui_consume_selection(context, status_code);

    context->list_snapshot_valid = false;
    context->list_output_len = 0U;
    context->list_output[0] = '\0';
    context->list_running = false;
}

static void minui_cancel_list(struct mesh_ui_backend_minui_context *context) {
    if (!context->list_running) {
        return;
    }

    if (context->list_pid > 0) {
        kill(context->list_pid, SIGTERM);
    }
    minui_complete_list(context);

    if (context->list_json_path[0] != '\0') {
        unlink(context->list_json_path);
        context->list_json_path[0] = '\0';
    }
}

static int minui_list_fd_callback(int fd, uint32_t events, void *userdata) {
    struct mesh_ui_backend_minui_context *context = (struct mesh_ui_backend_minui_context *)userdata;
    if (context == NULL) {
        return 0;
    }

    if ((events & EPOLLIN) != 0U) {
        while (true) {
            char chunk[256];
            ssize_t read_len = read(fd, chunk, sizeof chunk);
            if (read_len < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                mesh_log_warn("ui", "minui-list read failed: %s", strerror(errno));
                break;
            }
            if (read_len == 0) {
                break;
            }
            size_t available = sizeof(context->list_output) - context->list_output_len;
            if (available > 0U) {
                size_t to_copy = (size_t)read_len;
                if (to_copy > available) {
                    to_copy = available;
                }
                memcpy(context->list_output + context->list_output_len, chunk, to_copy);
                context->list_output_len += to_copy;
            }
        }
    }

    if ((events & (EPOLLHUP | EPOLLERR)) != 0U) {
        minui_complete_list(context);
    }

    return 0;
}

static int minui_spawn_list(struct mesh_ui_backend_minui_context *context) {
    if (context->loop == NULL) {
        mesh_log_warn("ui", "MinUI backend missing event loop; cannot spawn list");
        return -EINVAL;
    }

    char menu_json[4096];
    int format_result = mesh_ui_backend_minui_format_menu(&context->last_snapshot, menu_json, sizeof menu_json);
    if (format_result < 0) {
        mesh_log_warn("ui", "Failed to format MinUI menu: %d", format_result);
        return format_result;
    }

    char json_path[MESH_UI_MINUI_PATH_CAP];
    int file_result = minui_write_menu_file(menu_json, strlen(menu_json), json_path, sizeof json_path);
    if (file_result < 0) {
        mesh_log_warn("ui", "Failed to write MinUI menu file: %d", file_result);
        return file_result;
    }

    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) {
        int err = -errno;
        mesh_log_warn("ui", "pipe creation failed: %s", strerror(errno));
        unlink(json_path);
        return err;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int err = -errno;
        mesh_log_warn("ui", "fork failed for minui-list: %s", strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        unlink(json_path);
        return err;
    }

    if (pid == 0) {
        const char *list_cmd = get_env_or_default("MESHCLIENT_MINUI_LIST_CMD", k_minui_list);
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipe_fds[1]);
        execlp(list_cmd, list_cmd, "--disable-auto-sleep", "--format", "json", "--title", "Mesh Client",
               "--confirm-text", "CONNECT", "--item-key", "settings", "--write-value", "state", "--file",
               json_path, (char *)NULL);
        _exit(127);
    }

    close(pipe_fds[1]);

    int flags = fcntl(pipe_fds[0], F_GETFL, 0);
    if (flags >= 0) {
        fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
    }

    context->list_pid = pid;
    context->list_stdout_fd = pipe_fds[0];
    context->list_running = true;
    context->list_snapshot = context->last_snapshot;
    context->list_snapshot_valid = true;
    context->list_output_len = 0U;
    context->list_output[0] = '\0';
    snprintf(context->list_json_path, sizeof context->list_json_path, "%s", json_path);

    int add_result = mesh_event_loop_add_fd(context->loop, pipe_fds[0], EPOLLIN | EPOLLHUP | EPOLLERR,
                                            minui_list_fd_callback, context);
    if (add_result < 0) {
        mesh_log_warn("ui", "Failed to watch MinUI output: %d", add_result);
        close(pipe_fds[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        unlink(json_path);
        context->list_pid = -1;
        context->list_stdout_fd = -1;
        context->list_running = false;
        context->list_snapshot_valid = false;
        return add_result;
    }

    return 0;
}

static void minui_present_toast(struct mesh_ui_backend_minui_context *context, const char *message) {
    if (message == NULL || message[0] == '\0') {
        return;
    }

    const char *presenter_cmd = get_env_or_default("MESHCLIENT_MINUI_PRESENTER_CMD", k_minui_presenter);
    if (!command_in_path(presenter_cmd)) {
        if (!context->warned_presenter_missing) {
            mesh_log_warn("ui", "minui-presenter helper not available");
            context->warned_presenter_missing = true;
        }
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        mesh_log_warn("ui", "fork failed for minui-presenter: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) {
            execlp(presenter_cmd, presenter_cmd, "--message", message, "--timeout", "2", (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }

    waitpid(pid, NULL, 0);
}

static void minui_handle_snapshot(struct mesh_ui_backend_minui_context *context) {
    if (context->list_running) {
        if (!context->list_snapshot_valid || !minui_snapshots_equal(&context->list_snapshot, &context->last_snapshot)) {
            minui_cancel_list(context);
        }
    }

    if (!context->list_running) {
        minui_spawn_list(context);
    }
}

static int mesh_ui_backend_minui_init(void **state, void *userdata) {
    struct mesh_ui_backend_minui_context *context = (struct mesh_ui_backend_minui_context *)userdata;
    if (context != NULL) {
        struct mesh_event_loop *loop = context->loop;
        void (*callback)(void *, const char *) = context->on_device_selected;
        void *callback_userdata = context->callback_userdata;
        memset(context, 0, sizeof *context);
        context->loop = loop;
        context->on_device_selected = callback;
        context->callback_userdata = callback_userdata;
        context->list_pid = -1;
        context->list_stdout_fd = -1;
    }
    if (state != NULL) {
        *state = context;
    }
    mesh_log_info("ui", "MinUI backend initialised");
    return 0;
}

static void mesh_ui_backend_minui_shutdown(void *state, void *userdata) {
    struct mesh_ui_backend_minui_context *context = (struct mesh_ui_backend_minui_context *)(state != NULL ? state : userdata);
    if (context == NULL) {
        return;
    }
    minui_cancel_list(context);
}

static void mesh_ui_backend_minui_present(void *state, const struct mesh_ui_snapshot *snapshot, void *userdata) {
    struct mesh_ui_backend_minui_context *context = (struct mesh_ui_backend_minui_context *)(state != NULL ? state : userdata);
    if (context == NULL || snapshot == NULL) {
        return;
    }

    struct mesh_ui_snapshot previous = context->last_snapshot;
    bool had_previous = context->has_snapshot;

    context->last_snapshot = *snapshot;
    context->has_snapshot = true;

    minui_handle_snapshot(context);

    if (had_previous) {
        bool handshake_now = snapshot->handshake_valid;
        bool handshake_prev = previous.handshake_valid;

        if (!handshake_prev && handshake_now) {
            char message[128];
            if (snapshot->handshake.my_short_name[0] != '\0') {
                snprintf(message, sizeof message, "Connected as %s (%" PRIu32 " nodes)", snapshot->handshake.my_short_name,
                         snapshot->handshake.node_count);
            } else {
                snprintf(message, sizeof message, "Mesh connected (%" PRIu32 " nodes)", snapshot->handshake.node_count);
            }
            minui_present_toast(context, message);
        } else if (handshake_prev && !handshake_now) {
            minui_present_toast(context, "Mesh disconnected");
        } else if (handshake_now && previous.handshake.config_complete != snapshot->handshake.config_complete &&
                   snapshot->handshake.config_complete) {
            minui_present_toast(context, "Mesh config synchronised");
        }
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
