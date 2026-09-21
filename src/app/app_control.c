#define _POSIX_C_SOURCE 200809L

/*
 * The control socket - see include/mesh/app/control.h for the commands.
 *
 * Both ends are here. The listening end runs on the client's loop like any other source: it
 * reads a line, does what it says, and answers - except for the two commands that are about
 * time passing, which park the connection on a timer and pick the next line up when it fires.
 * The sending end is a plain blocking program, because it is one: `meshclient --ui-send` exits
 * the moment the last answer is in.
 */

#include "mesh/app/control.h"

#include "inkcell/ui/fb_capture.h"
#include "inkcell/ui/fb_draw.h"
#include "inkcell/ui/key.h"
#include "inkwell/base/fd.h"
#include "inkwell/base/log.h"
#include "inkwell/base/time.h"
#include "inkwell/runtime/timer.h"

#include "mesh/ui/controller.h"
#include "mesh/ui/route.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* How long the sending end waits for an answer, on top of any `wait` it asked for. A `shot`
   answers within MESH_APP_CONTROL_SETTLE_MS plus the write, so this is slack for a slow card. */
#define MESH_APP_CONTROL_ANSWER_MS 10000

static void mesh_app_control_drop_client(struct mesh_app_control *control);
static void mesh_app_control_run(struct mesh_app_control *control);

/* ---- the listening end -------------------------------------------------------------------- */

static void mesh_app_control_reply(struct mesh_app_control *control, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void mesh_app_control_reply(struct mesh_app_control *control, const char *format, ...) {
    if (control->client_fd < 0) {
        return;
    }
    char line[MESH_APP_CONTROL_LINE_MAX + 32U];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(line, sizeof line - 1U, format, args);
    va_end(args);
    if (len < 0) {
        return;
    }
    if ((size_t)len > sizeof line - 2U) {
        len = (int)(sizeof line - 2U);
    }
    line[len++] = '\n';
    /* An answer is a line, and a socket buffer holds thousands of them: a short write here is a
       peer that stopped reading, which is a peer to let go of. */
    if (send(control->client_fd, line, (size_t)len, MSG_NOSIGNAL) != (ssize_t)len) {
        mesh_app_control_drop_client(control);
    }
}

static void mesh_app_control_arm(struct mesh_app_control *control, uint32_t after_ms) {
    if (control->timer_fd >= 0) {
        /* A zero delay disarms, and a wait of 0 still means "on the next turn". */
        (void)inkwell_timer_arm_once(control->timer_fd, after_ms > 0U ? after_ms : 1U);
    }
}

static void mesh_app_control_shoot(struct mesh_app_control *control, bool settled) {
    control->waiting = MESH_APP_CONTROL_READY;
    struct inkcell_surface frame;
    if (!mesh_ui_controller_frame(control->controller, &frame)) {
        mesh_app_control_reply(control, "error no frame (the backend draws no pixels, or has "
                                        "not drawn yet)");
        return;
    }
    const int written = inkcell_surface_write_ppm(&frame, control->shot_path);
    if (written < 0) {
        mesh_app_control_reply(control, "error %s: %s", control->shot_path, strerror(-written));
        return;
    }
    /* Said, rather than hidden: a picture of a screen that was still moving is still a picture,
       but whoever asked should know it is not of the screen at rest. */
    mesh_app_control_reply(control, settled ? "ok %s" : "ok %s (still moving)", control->shot_path);
}

/* The next word of `*cursor`, NUL-terminated in place; NULL when there is none. */
static char *mesh_app_control_word(char **cursor) {
    char *p = *cursor;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p == '\0') {
        *cursor = p;
        return NULL;
    }
    char *const word = p;
    while (*p != '\0' && !isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p != '\0') {
        *p++ = '\0';
    }
    *cursor = p;
    return word;
}

/* One command. Leaves `waiting` set when the answer comes later. */
static void mesh_app_control_command(struct mesh_app_control *control, char *line) {
    char *cursor = line;
    const char *const verb = mesh_app_control_word(&cursor);
    if (verb == NULL) {
        return;
    }

    if (strcmp(verb, "ping") == 0) {
        mesh_app_control_reply(control, "ok");
    } else if (strcmp(verb, "key") == 0) {
        /* Every name checked before any is pressed, so a typo in the third does not leave the
           first two done and the reader somewhere nobody meant. */
        char *names[32];
        size_t count = 0U;
        for (char *name = mesh_app_control_word(&cursor); name != NULL;
             name = mesh_app_control_word(&cursor)) {
            if (inkcell_key_from_name(name) == INKCELL_KEY_NONE) {
                mesh_app_control_reply(control, "error unknown key '%s'", name);
                return;
            }
            if (count == sizeof names / sizeof names[0]) {
                mesh_app_control_reply(control, "error more than %zu keys in one command", count);
                return;
            }
            names[count++] = name;
        }
        if (count == 0U) {
            mesh_app_control_reply(control, "error key needs a name");
            return;
        }
        for (size_t i = 0U; i < count; ++i) {
            mesh_ui_controller_handle_key(control->controller, inkcell_key_from_name(names[i]));
        }
        mesh_app_control_reply(control, "ok");
    } else if (strcmp(verb, "wait") == 0) {
        const char *const ms = mesh_app_control_word(&cursor);
        char *end = NULL;
        const unsigned long value = ms != NULL ? strtoul(ms, &end, 10) : 0UL;
        if (ms == NULL || end == ms || *end != '\0' || value > 600000UL) {
            mesh_app_control_reply(control, "error wait needs milliseconds (0-600000)");
            return;
        }
        control->waiting = MESH_APP_CONTROL_SLEEPING;
        mesh_app_control_arm(control, (uint32_t)value);
    } else if (strcmp(verb, "shot") == 0) {
        const char *const path = mesh_app_control_word(&cursor);
        if (path == NULL) {
            mesh_app_control_reply(control, "error shot needs a path");
            return;
        }
        snprintf(control->shot_path, sizeof control->shot_path, "%s", path);
        /* Not now: a key a moment ago has published a snapshot the loop has not drawn yet, and
           what it draws may slide in over several frames. Looked at again shortly, and taken
           once nothing is moving. */
        control->waiting = MESH_APP_CONTROL_SETTLING;
        control->deadline_ms = inkwell_time_monotonic_ms() + MESH_APP_CONTROL_SETTLE_MS;
        mesh_app_control_arm(control, MESH_APP_CONTROL_POLL_MS);
    } else if (strcmp(verb, "screen") == 0) {
        const struct mesh_ui_store *const store =
            control->controller != NULL ? control->controller->store : NULL;
        if (store == NULL) {
            mesh_app_control_reply(control, "error no UI");
            return;
        }
        mesh_app_control_reply(control, "ok %s", mesh_ui_screen_id(store->nav.screen));
    } else if (strcmp(verb, "quit") == 0) {
        mesh_app_control_reply(control, "ok");
        inkwell_loop_request_stop(control->loop);
    } else {
        mesh_app_control_reply(control, "error unknown command '%s'", verb);
    }
}

/* Every whole line in the buffer, in order, until one has to wait. */
static void mesh_app_control_run(struct mesh_app_control *control) {
    while (control->client_fd >= 0 && control->waiting == MESH_APP_CONTROL_READY) {
        char *const newline = memchr(control->in, '\n', control->in_len);
        if (newline == NULL) {
            if (control->in_len == sizeof control->in) {
                mesh_app_control_reply(control, "error line longer than %zu bytes",
                                       sizeof control->in);
                mesh_app_control_drop_client(control);
            }
            return;
        }
        *newline = '\0';
        if (newline > control->in && newline[-1] == '\r') {
            newline[-1] = '\0';
        }
        char line[MESH_APP_CONTROL_LINE_MAX];
        memcpy(line, control->in, (size_t)(newline - control->in) + 1U);
        const size_t consumed = (size_t)(newline - control->in) + 1U;
        memmove(control->in, newline + 1, control->in_len - consumed);
        control->in_len -= consumed;
        mesh_app_control_command(control, line);
    }
}

static void mesh_app_control_drop_client(struct mesh_app_control *control) {
    if (control->client_fd >= 0) {
        inkwell_loop_remove_fd(control->loop, control->client_fd);
        close(control->client_fd);
        control->client_fd = -1;
    }
    control->in_len = 0U;
    control->waiting = MESH_APP_CONTROL_READY;
    if (control->timer_fd >= 0) {
        (void)inkwell_timer_arm_once(control->timer_fd, 0U);
    }
}

static int mesh_app_control_on_client(int fd, uint32_t events, void *userdata) {
    struct mesh_app_control *const control = (struct mesh_app_control *)userdata;
    if ((events & INKWELL_LOOP_IN) != 0U) {
        while (control->in_len < sizeof control->in) {
            const ssize_t got =
                read(fd, control->in + control->in_len, sizeof control->in - control->in_len);
            if (got > 0) {
                control->in_len += (size_t)got;
                continue;
            }
            if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                break;
            }
            /* The peer hung up. What it already sent still runs: `echo quit | nc -U` closes
               the moment it has written. */
            mesh_app_control_run(control);
            mesh_app_control_drop_client(control);
            return 0;
        }
        mesh_app_control_run(control);
        return 0;
    }
    if ((events & (INKWELL_LOOP_HUP | INKWELL_LOOP_ERR)) != 0U) {
        mesh_app_control_drop_client(control);
    }
    return 0;
}

static int mesh_app_control_on_accept(int fd, uint32_t events, void *userdata) {
    struct mesh_app_control *const control = (struct mesh_app_control *)userdata;
    if ((events & INKWELL_LOOP_IN) == 0U) {
        return 0;
    }
    const int client = accept(fd, NULL, NULL);
    if (client < 0) {
        return 0;
    }
    if (control->client_fd >= 0) {
        static const char busy[] = "error busy\n";
        (void)send(client, busy, sizeof busy - 1U, MSG_NOSIGNAL);
        close(client);
        return 0;
    }
    if (inkwell_fd_set_nonblocking_cloexec(client) < 0 ||
        inkwell_loop_add_fd(control->loop, client, INKWELL_LOOP_IN, mesh_app_control_on_client,
                            control) < 0) {
        close(client);
        return 0;
    }
    control->client_fd = client;
    control->in_len = 0U;
    control->waiting = MESH_APP_CONTROL_READY;
    return 0;
}

static int mesh_app_control_on_timer(int fd, uint32_t events, void *userdata) {
    struct mesh_app_control *const control = (struct mesh_app_control *)userdata;
    if ((events & INKWELL_LOOP_IN) == 0U) {
        return 0;
    }
    (void)inkwell_timer_read(fd);
    switch (control->waiting) {
    case MESH_APP_CONTROL_SLEEPING:
        control->waiting = MESH_APP_CONTROL_READY;
        mesh_app_control_reply(control, "ok");
        break;
    case MESH_APP_CONTROL_SETTLING:
        if (mesh_ui_controller_settled(control->controller)) {
            mesh_app_control_shoot(control, true);
        } else if (inkwell_time_monotonic_ms() >= control->deadline_ms) {
            mesh_app_control_shoot(control, false);
        } else {
            mesh_app_control_arm(control, MESH_APP_CONTROL_POLL_MS);
        }
        break;
    case MESH_APP_CONTROL_READY:
        break;
    }
    mesh_app_control_run(control);
    return 0;
}

/* Whether `path` is free to bind: nothing there, or a socket a previous run left behind. A
   regular file at the path is somebody's, and is not ours to delete. */
static int mesh_app_control_clear_path(const char *path) {
    struct stat info;
    if (lstat(path, &info) < 0) {
        return errno == ENOENT ? 0 : -errno;
    }
    if (!S_ISSOCK(info.st_mode)) {
        return -EEXIST;
    }
    return unlink(path) < 0 ? -errno : 0;
}

int mesh_app_control_open(struct mesh_app_control *control, struct inkwell_loop *loop,
                          struct mesh_ui_controller *controller, const char *path) {
    if (control == NULL || loop == NULL || controller == NULL || path == NULL || path[0] == '\0') {
        return -EINVAL;
    }
    memset(control, 0, sizeof *control);
    control->listen_fd = -1;
    control->client_fd = -1;
    control->timer_fd = -1;
    control->loop = loop;
    control->controller = controller;

    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof control->path || strlen(path) >= sizeof address.sun_path) {
        return -ENAMETOOLONG;
    }
    snprintf(control->path, sizeof control->path, "%s", path);
    memcpy(address.sun_path, path, strlen(path) + 1U);

    int result = mesh_app_control_clear_path(path);
    if (result < 0) {
        return result;
    }

    const int fd = inkwell_fd_socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return fd;
    }
    /* 0600 from the moment it exists, rather than chmod'ed after a bind that anyone could have
       connected to in between. The umask is the process's, and this loop has no other thread
       to be surprised by it. */
    const mode_t previous = umask(0077);
    result = bind(fd, (const struct sockaddr *)&address, sizeof address) < 0 ? -errno : 0;
    umask(previous);
    if (result == 0 && listen(fd, 2) < 0) {
        result = -errno;
    }
    if (result < 0) {
        close(fd);
        return result;
    }
    control->listen_fd = fd;

    control->timer_fd = inkwell_timer_open();
    if (control->timer_fd < 0 ||
        inkwell_loop_add_fd(loop, control->timer_fd, INKWELL_LOOP_IN, mesh_app_control_on_timer,
                            control) < 0 ||
        inkwell_loop_add_fd(loop, fd, INKWELL_LOOP_IN, mesh_app_control_on_accept, control) < 0) {
        result = control->timer_fd < 0 ? control->timer_fd : -EMFILE;
        mesh_app_control_close(control);
        return result;
    }
    inkwell_log_info("app", "UI control socket listening at %s", control->path);
    return 0;
}

void mesh_app_control_close(struct mesh_app_control *control) {
    if (control == NULL || control->loop == NULL) {
        return;
    }
    mesh_app_control_drop_client(control);
    if (control->timer_fd >= 0) {
        inkwell_loop_remove_fd(control->loop, control->timer_fd);
        close(control->timer_fd);
        control->timer_fd = -1;
    }
    if (control->listen_fd >= 0) {
        inkwell_loop_remove_fd(control->loop, control->listen_fd);
        close(control->listen_fd);
        control->listen_fd = -1;
        (void)mesh_app_control_clear_path(control->path);
    }
    control->loop = NULL;
}

/* ---- the sending end ---------------------------------------------------------------------- */

/* One line of answer into `line`, waiting up to `timeout_ms`. Returns its length or a negative
   errno. `pending`/`pending_len` hold whatever arrived after the newline, for the next call. */
static int mesh_app_control_read_line(int fd, char *line, size_t cap, char *pending,
                                      size_t *pending_len, int timeout_ms) {
    for (;;) {
        char *const newline = memchr(pending, '\n', *pending_len);
        if (newline != NULL) {
            const size_t len = (size_t)(newline - pending);
            const size_t copy = len < cap - 1U ? len : cap - 1U;
            memcpy(line, pending, copy);
            line[copy] = '\0';
            memmove(pending, newline + 1, *pending_len - len - 1U);
            *pending_len -= len + 1U;
            return (int)copy;
        }
        if (*pending_len == MESH_APP_CONTROL_LINE_MAX) {
            return -EPROTO;
        }
        struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
        const int ready = poll(&poll_fd, 1, timeout_ms);
        if (ready == 0) {
            return -ETIMEDOUT;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        const ssize_t got =
            read(fd, pending + *pending_len, MESH_APP_CONTROL_LINE_MAX - *pending_len);
        if (got == 0) {
            return -ECONNRESET;
        }
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -errno;
        }
        *pending_len += (size_t)got;
    }
}

int mesh_app_control_send(const char *path, const char *commands, FILE *out) {
    if (path == NULL || commands == NULL || out == NULL) {
        return -EINVAL;
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof address.sun_path) {
        return -ENAMETOOLONG;
    }
    memcpy(address.sun_path, path, strlen(path) + 1U);

    /* Blocking, and so not inkwell_fd_socket(): this end has no loop to stall. */
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -errno;
    }
    if (connect(fd, (const struct sockaddr *)&address, sizeof address) < 0) {
        const int error = -errno;
        close(fd);
        return error;
    }

    int status = 0;
    char pending[MESH_APP_CONTROL_LINE_MAX];
    size_t pending_len = 0U;
    const char *cursor = commands;
    while (status == 0 && *cursor != '\0') {
        const size_t span = strcspn(cursor, ";\n");
        const char *start = cursor;
        const char *end = cursor + span;
        cursor = *end != '\0' ? end + 1 : end;
        while (start < end && isspace((unsigned char)*start)) {
            ++start;
        }
        while (end > start && isspace((unsigned char)end[-1])) {
            --end;
        }
        const size_t len = (size_t)(end - start);
        if (len == 0U) {
            continue;
        }
        if (len >= MESH_APP_CONTROL_LINE_MAX - 1U) {
            status = -E2BIG;
            break;
        }

        char command[MESH_APP_CONTROL_LINE_MAX];
        memcpy(command, start, len);
        command[len] = '\n';
        if (send(fd, command, len + 1U, MSG_NOSIGNAL) != (ssize_t)(len + 1U)) {
            status = -errno;
            break;
        }

        /* A `wait` answers when it is done waiting, so it gets its own time on top. */
        int timeout = MESH_APP_CONTROL_ANSWER_MS;
        if (len > 5U && strncmp(start, "wait ", 5U) == 0) {
            timeout += atoi(start + 5);
        }
        char answer[MESH_APP_CONTROL_LINE_MAX];
        const int got =
            mesh_app_control_read_line(fd, answer, sizeof answer, pending, &pending_len, timeout);
        if (got < 0) {
            status = got;
            break;
        }
        fprintf(out, "%.*s: %s\n", (int)len, start, answer);
        if (strncmp(answer, "ok", 2U) != 0) {
            status = -EPROTO;
        }
    }
    fflush(out);
    close(fd);
    return status;
}
