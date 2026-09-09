#define _POSIX_C_SOURCE 200809L

#include "mesh/core/fetch.h"

#include "mesh/core/event_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * How long the fetcher is allowed to take, against how long we are.
 *
 * Both numbers exist and they must not be equal: when the transfer is what went wrong we want
 * curl to say so and exit, because "curl exited 28" names the step that failed, while our own
 * deadline firing means we killed a child that might have been about to succeed. So the tool
 * is given nine tenths of the request's timeout and our deadline is the backstop behind it.
 *
 * The two tools do not mean quite the same thing by their number - curl's --max-time is the
 * whole transfer, wget's -T is a per-read timeout - and that difference is older than this
 * file. Neither is a promise; the backstop is.
 */
#define MESH_FETCH_TOOL_SHARE_NUM 9U
#define MESH_FETCH_TOOL_SHARE_DEN 10U

/* Enough for the fixed flags, an optional CA bundle, the headers, an output path and the URL. */
#define MESH_FETCH_ARGV_MAX (12U + (MESH_FETCH_HEADERS_MAX * 2U))

static bool have_executable(const char *name) {
    const char *path = getenv("PATH");
    if (path == NULL || path[0] == '\0') {
        path = "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin";
    }
    while (*path != '\0') {
        const char *colon = strchr(path, ':');
        const size_t len = colon != NULL ? (size_t)(colon - path) : strlen(path);
        if (len > 0U && len < 200U) {
            char candidate[256];
            snprintf(candidate, sizeof candidate, "%.*s/%s", (int)len, path, name);
            if (access(candidate, X_OK) == 0) {
                return true;
            }
        }
        if (colon == NULL) {
            break;
        }
        path = colon + 1;
    }
    return false;
}

/*
 * Drops the pipe but keeps the pid. Once the child has closed stdout there is nothing more to
 * read, and leaving the fd registered would be actively harmful: epoll reports EOF/HUP on
 * every wait, so mesh_event_loop_run() would never see a zero-event timeout, never return, and
 * never let mesh_fetch_tick() enforce the deadline - a child that closed stdout without
 * exiting would spin the loop and freeze the UI. The response buffer is left alone; the reap
 * still has to hand it over.
 */
static void fetch_release_fd(struct mesh_fetch *fetch) {
    if (fetch->child_fd < 0) {
        return;
    }
    if (fetch->loop != NULL) {
        (void)mesh_event_loop_remove_fd(fetch->loop, fetch->child_fd);
    }
    close(fetch->child_fd);
    fetch->child_fd = -1;
}

/* Everything about the request in flight, gone. Does not call the callback. */
static void fetch_discard(struct mesh_fetch *fetch) {
    fetch_release_fd(fetch);
    if (fetch->child > 0) {
        int status = 0;
        if (waitpid(fetch->child, &status, WNOHANG) == 0) {
            kill(fetch->child, SIGKILL);
            (void)waitpid(fetch->child, &status, 0);
        }
        fetch->child = -1;
    }
    free(fetch->response);
    fetch->response = NULL;
    fetch->response_len = 0U;
    fetch->on_done = NULL;
    fetch->userdata = NULL;
    fetch->failure = MESH_FETCH_OK;
}

/*
 * Hands the outcome to whoever asked for it, exactly once.
 *
 * The order here is the re-entrancy rule: everything is lifted off the fetcher and the fetcher
 * is left idle *before* the callback runs, so a caller that starts its next request from
 * inside the completion is starting one against a clean fetcher. The body is freed after the
 * call, off a local pointer, so that second request allocating its own buffer cannot be
 * confused with this one's.
 */
static void fetch_complete(struct mesh_fetch *fetch, enum mesh_fetch_outcome outcome, int status) {
    char *const body = fetch->response;
    const size_t len = fetch->response_len;
    const mesh_fetch_done_fn done = fetch->on_done;
    void *const userdata = fetch->userdata;

    fetch_release_fd(fetch);
    fetch->child = -1;
    fetch->response = NULL;
    fetch->response_len = 0U;
    fetch->on_done = NULL;
    fetch->userdata = NULL;
    fetch->failure = MESH_FETCH_OK;

    if (done != NULL) {
        const struct mesh_fetch_result result = {
            .outcome = outcome,
            .status = status,
            .body = body,
            .len = len,
        };
        done(userdata, &result);
    }
    free(body);
}

/* Appends whatever the child has written, capped so a runaway reply cannot grow without
   bound. Returns false when the cap is hit. */
static bool fetch_absorb(struct mesh_fetch *fetch, const char *bytes, size_t len) {
    if (fetch->response_len + len + 1U > fetch->response_max) {
        return false;
    }
    char *grown = realloc(fetch->response, fetch->response_len + len + 1U);
    if (grown == NULL) {
        return false;
    }
    memcpy(grown + fetch->response_len, bytes, len);
    fetch->response_len += len;
    grown[fetch->response_len] = '\0';
    fetch->response = grown;
    return true;
}

/*
 * Reads whatever is buffered without blocking. Returns false when there will never be more -
 * EOF, or a failure that has already recorded itself in `failure` for the reap to report.
 */
static bool fetch_drain(struct mesh_fetch *fetch) {
    if (fetch->child_fd < 0) {
        return false;
    }
    for (;;) {
        char buffer[4096];
        const ssize_t got = read(fetch->child_fd, buffer, sizeof buffer);
        if (got > 0) {
            if (!fetch_absorb(fetch, buffer, (size_t)got)) {
                fetch->failure = MESH_FETCH_TOO_LARGE;
                return false;
            }
            continue;
        }
        if (got == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        fetch->failure = MESH_FETCH_READ_FAILED;
        return false;
    }
}

/*
 * Finishes the request if the child has actually exited. Deliberately never blocks in waitpid:
 * this runs from the event loop, which is the same thread the UI draws on, and a child that
 * closed stdout without exiting would otherwise stall the whole client past the point where
 * the deadline in tick() could rescue it.
 *
 * Everything buffered is drained before dispatching, because the exit and the EOF are separate
 * events and either can be seen first: reaping without draining would hand a caller a
 * truncated reply and let it decide the document was broken.
 */
static void fetch_try_finish(struct mesh_fetch *fetch) {
    if (fetch->child <= 0) {
        return;
    }
    if (!fetch_drain(fetch)) {
        /* EOF, or a failure that ends the read either way. The pipe is finished with. */
        fetch_release_fd(fetch);
    }

    int status = 0;
    if (waitpid(fetch->child, &status, WNOHANG) != fetch->child) {
        /*
         * A drain that gave up is not a reason to wait for the exit: the child is writing into
         * a pipe nobody is reading and the caller's answer is already known.
         */
        if (fetch->failure != MESH_FETCH_OK) {
            const enum mesh_fetch_outcome why = fetch->failure;
            kill(fetch->child, SIGKILL);
            (void)waitpid(fetch->child, &status, 0);
            fetch->child = -1;
            fetch_complete(fetch, why, -1);
        }
        return;
    }
    fetch->child = -1;

    const int exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (fetch->failure != MESH_FETCH_OK) {
        fetch_complete(fetch, fetch->failure, exit_status);
    } else if (exit_status != 0) {
        fetch_complete(fetch, MESH_FETCH_EXITED, exit_status);
    } else {
        fetch_complete(fetch, MESH_FETCH_OK, 0);
    }
}

static int fetch_on_child_output(int fd, uint32_t events, void *userdata) {
    (void)fd;
    struct mesh_fetch *fetch = (struct mesh_fetch *)userdata;
    if (fetch == NULL) {
        return 0;
    }
    /* EOF or a hangup means the child has closed stdout; either way, drain and see whether it
       has exited. try_finish() is a no-op until it has, so nothing here can block. */
    if (fetch_drain(fetch) && (events & (EPOLLHUP | EPOLLERR)) == 0U) {
        return 0;
    }
    fetch_try_finish(fetch);
    return 0;
}

/* Forks `argv` with its stdout on a pipe registered with the event loop. */
static int fetch_spawn(struct mesh_fetch *fetch, char *const argv[], uint64_t now_ms,
                       uint32_t timeout_ms) {
    int fds[2];
    if (pipe(fds) < 0) {
        return -errno;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        const int err = -errno;
        close(fds[0]);
        close(fds[1]);
        return err;
    }
    if (pid == 0) {
        /* Child: stdout to the pipe, stderr to the log's fate (inherited), stdin closed. */
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(fds[1]);
        const int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    close(fds[1]);
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) {
        const int err = -errno;
        close(fds[0]);
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return err;
    }

    fetch->child = pid;
    fetch->child_fd = fds[0];
    fetch->deadline_ms = now_ms + timeout_ms;
    if (fetch->loop != NULL) {
        const int added =
            mesh_event_loop_add_fd(fetch->loop, fds[0], EPOLLIN, fetch_on_child_output, fetch);
        if (added != 0) {
            fetch_discard(fetch);
            return added;
        }
    }
    return 0;
}

int mesh_fetch_init(struct mesh_fetch *fetch, struct mesh_event_loop *loop) {
    if (fetch == NULL) {
        return -EINVAL;
    }
    memset(fetch, 0, sizeof *fetch);
    fetch->child = -1;
    fetch->child_fd = -1;
    fetch->loop = loop;

    if (have_executable("curl")) {
        fetch->tool = "curl";
    } else if (have_executable("wget")) {
        fetch->tool = "wget";
    } else {
        fetch->tool = NULL;
    }
    return 0;
}

void mesh_fetch_shutdown(struct mesh_fetch *fetch) {
    if (fetch == NULL) {
        return;
    }
    fetch_discard(fetch);
}

bool mesh_fetch_available(const struct mesh_fetch *fetch) {
    return fetch != NULL && fetch->tool != NULL && fetch->loop != NULL;
}

bool mesh_fetch_busy(const struct mesh_fetch *fetch) { return fetch != NULL && fetch->child > 0; }

const char *mesh_fetch_tool(const struct mesh_fetch *fetch) {
    if (fetch == NULL || fetch->tool == NULL) {
        return "fetcher";
    }
    return fetch->tool;
}

void mesh_fetch_resolve_ca_bundle(struct mesh_fetch *fetch, const char *shipped) {
    if (fetch == NULL) {
        return;
    }
    fetch->ca_bundle[0] = '\0';

    static const char *const k_env[] = {"SSL_CERT_FILE", "CURL_CA_BUNDLE"};
    for (size_t i = 0; i < sizeof k_env / sizeof k_env[0]; ++i) {
        const char *const value = getenv(k_env[i]);
        if (value != NULL && value[0] != '\0' && access(value, R_OK) == 0) {
            snprintf(fetch->ca_bundle, sizeof fetch->ca_bundle, "%s", value);
            return;
        }
    }

    if (shipped != NULL && shipped[0] != '\0' && access(shipped, R_OK) == 0) {
        snprintf(fetch->ca_bundle, sizeof fetch->ca_bundle, "%s", shipped);
        return;
    }

    static const char *const k_system[] = {
        "/etc/ssl/certs/ca-certificates.crt", /* Debian, Ubuntu, Alpine, Arch */
        "/etc/pki/tls/certs/ca-bundle.crt",   /* Fedora, RHEL */
        "/etc/ssl/cert.pem",                  /* BSD, and Alpine's compatibility link */
        "/etc/ssl/certs/ca-bundle.crt",
    };
    for (size_t i = 0; i < sizeof k_system / sizeof k_system[0]; ++i) {
        if (access(k_system[i], R_OK) == 0) {
            snprintf(fetch->ca_bundle, sizeof fetch->ca_bundle, "%s", k_system[i]);
            return;
        }
    }
}

int mesh_fetch_start(struct mesh_fetch *fetch, const struct mesh_fetch_request *request,
                     uint64_t now_ms) {
    if (fetch == NULL || request == NULL || request->url == NULL || request->url[0] == '\0' ||
        request->on_done == NULL) {
        return -EINVAL;
    }
    if (!mesh_fetch_available(fetch)) {
        return -ENOTSUP;
    }
    if (fetch->child > 0) {
        return -EBUSY;
    }
    /* A previous request's buffer, if a caller cancelled without going through us. */
    fetch_discard(fetch);

    const uint32_t timeout_ms = request->timeout_ms > 0U ? request->timeout_ms : 30000U;
    unsigned long tool_seconds =
        (unsigned long)timeout_ms * MESH_FETCH_TOOL_SHARE_NUM / (MESH_FETCH_TOOL_SHARE_DEN * 1000U);
    if (tool_seconds < 1UL) {
        tool_seconds = 1UL;
    }
    char seconds[16];
    snprintf(seconds, sizeof seconds, "%lu", tool_seconds);

    /* Long-option forms for wget, which takes its bundle and its headers glued to the flag. */
    char ca_option[MESH_FETCH_PATH_MAX + 24U];
    snprintf(ca_option, sizeof ca_option, "--ca-certificate=%s", fetch->ca_bundle);
    char header_options[MESH_FETCH_HEADERS_MAX][256];

    char *argv[MESH_FETCH_ARGV_MAX];
    size_t argc = 0U;
    const bool curl = strcmp(fetch->tool, "curl") == 0;
    if (curl) {
        argv[argc++] = (char *)"curl";
        argv[argc++] = (char *)"-fsSL";
        argv[argc++] = (char *)"--max-time";
        argv[argc++] = seconds;
        if (fetch->ca_bundle[0] != '\0') {
            argv[argc++] = (char *)"--cacert";
            argv[argc++] = fetch->ca_bundle;
        }
        for (size_t i = 0; i < MESH_FETCH_HEADERS_MAX && request->headers[i] != NULL; ++i) {
            argv[argc++] = (char *)"-H";
            argv[argc++] = (char *)request->headers[i];
        }
        if (request->output_path != NULL) {
            argv[argc++] = (char *)"-o";
            argv[argc++] = (char *)request->output_path;
        }
    } else {
        argv[argc++] = (char *)"wget";
        argv[argc++] = (char *)"-q";
        argv[argc++] = (char *)"-T";
        argv[argc++] = seconds;
        if (fetch->ca_bundle[0] != '\0') {
            argv[argc++] = ca_option;
        }
        for (size_t i = 0; i < MESH_FETCH_HEADERS_MAX && request->headers[i] != NULL; ++i) {
            snprintf(header_options[i], sizeof header_options[i], "--header=%s",
                     request->headers[i]);
            argv[argc++] = header_options[i];
        }
        argv[argc++] = (char *)"-O";
        /* wget has no "write to stdout" default: capturing means asking for `-` by name. */
        argv[argc++] = request->output_path != NULL ? (char *)request->output_path : (char *)"-";
    }
    argv[argc++] = (char *)request->url;
    argv[argc] = NULL;

    fetch->response_max =
        request->response_max > 0U ? request->response_max : MESH_FETCH_RESPONSE_MAX;
    fetch->on_done = request->on_done;
    fetch->userdata = request->userdata;
    fetch->failure = MESH_FETCH_OK;

    const int result = fetch_spawn(fetch, argv, now_ms, timeout_ms);
    if (result != 0) {
        /* Nothing was spawned, so nothing may be reported: leave the fetcher idle and let the
           caller turn the errno into whatever its own screen says. */
        fetch->on_done = NULL;
        fetch->userdata = NULL;
        return result;
    }
    return 0;
}

void mesh_fetch_tick(struct mesh_fetch *fetch, uint64_t now_ms) {
    if (fetch == NULL || fetch->child <= 0) {
        return;
    }
    /* A child that exited without closing stdout, or whose EOF the loop did not deliver, is
       finished here. Drains first, exactly as the fd callback does. */
    fetch_try_finish(fetch);
    if (fetch->child <= 0) {
        return;
    }
    if (now_ms >= fetch->deadline_ms) {
        /* Deliberately silent: the caller knows which step this was and says so itself. */
        fetch_release_fd(fetch);
        kill(fetch->child, SIGKILL);
        (void)waitpid(fetch->child, NULL, 0);
        fetch->child = -1;
        fetch_complete(fetch, MESH_FETCH_TIMED_OUT, -1);
    }
}

void mesh_fetch_cancel(struct mesh_fetch *fetch) {
    if (fetch == NULL) {
        return;
    }
    fetch_discard(fetch);
}
