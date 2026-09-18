#define _POSIX_C_SOURCE 200809L

#include "mesh/core/resolve.h"

#include "mesh/core/event_loop.h"
#include "mesh/utils/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * What the child writes back: one fixed-size record, one write, no framing.
 *
 * Both ends of this pipe are the same binary, so the struct needs no wire format - and being
 * smaller than PIPE_BUF it goes down in a single atomic write, so there is no interleaving to
 * defend against. The parent still accumulates rather than assuming one read, because a short
 * read is a thing a signal can cause and a truncated answer must be told from a complete one.
 */
struct resolve_record {
    int32_t error; /* the getaddrinfo() return, 0 on success */
    uint32_t address_len;
    struct sockaddr_storage address;
};

_Static_assert(sizeof(struct resolve_record) <= sizeof(((struct mesh_resolve *)0)->record),
               "the record buffer in struct mesh_resolve must hold a whole resolve_record");

/* ------------------------------------------------------------------ literals */

bool mesh_resolve_literal(const char *host, uint16_t port, struct sockaddr_storage *out,
                          socklen_t *out_len) {
    if (host == NULL || host[0] == '\0' || out == NULL || out_len == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);

    struct in_addr v4;
    if (inet_pton(AF_INET, host, &v4) == 1) {
        struct sockaddr_in *addr = (struct sockaddr_in *)out;
        addr->sin_family = AF_INET;
        addr->sin_port = htons(port);
        addr->sin_addr = v4;
        *out_len = (socklen_t)sizeof *addr;
        return true;
    }

    struct in6_addr v6;
    if (inet_pton(AF_INET6, host, &v6) == 1) {
        struct sockaddr_in6 *addr = (struct sockaddr_in6 *)out;
        addr->sin6_family = AF_INET6;
        addr->sin6_port = htons(port);
        addr->sin6_addr = v6;
        *out_len = (socklen_t)sizeof *addr;
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------ the child */

/* Writes the whole record or dies trying; a short write here is the parent's truncation case. */
static void resolve_write_record(int fd, const struct resolve_record *record) {
    const uint8_t *bytes = (const uint8_t *)record;
    size_t left = sizeof *record;
    while (left > 0U) {
        const ssize_t wrote = write(fd, bytes, left);
        if (wrote > 0) {
            bytes += (size_t)wrote;
            left -= (size_t)wrote;
            continue;
        }
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        return; /* the parent gave up on us; it has already decided the outcome */
    }
}

/*
 * The whole of the child.
 *
 * This forks without exec'ing, so the child inherits everything the client has open - the epoll
 * descriptor, the D-Bus connection to BlueZ, the framebuffer mapping. **It must touch none of
 * it.** Resolve, write, `_exit` - never `exit()`, which would run atexit handlers and flush
 * stdio belonging to a process that is still running.
 */
static void resolve_child(const char *host, uint16_t port, int fd) {
    char service[8];
    snprintf(service, sizeof service, "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    /*
     * AI_ADDRCONFIG, so a family this device has no address in is not offered at all. The Brick
     * on WiFi usually has no routable IPv6, and taking a AAAA record there would hand the caller
     * an address whose connect can only ever time out.
     */
    hints.ai_flags = AI_ADDRCONFIG;

    struct resolve_record record;
    memset(&record, 0, sizeof record);

    struct addrinfo *results = NULL;
    record.error = getaddrinfo(host, service, &hints, &results);
    if (record.error == 0 && results != NULL && results->ai_addr != NULL &&
        results->ai_addrlen > 0U && (size_t)results->ai_addrlen <= sizeof record.address) {
        /* The first answer, with the port already in it - getaddrinfo() filled that from
           `service`, which is why nothing here pokes at sin_port. */
        record.address_len = (uint32_t)results->ai_addrlen;
        memcpy(&record.address, results->ai_addr, (size_t)results->ai_addrlen);
    }
    if (results != NULL) {
        freeaddrinfo(results);
    }

    resolve_write_record(fd, &record);
    _exit(0);
}

/* ------------------------------------------------------------------ the parent */

/*
 * Drops the pipe but keeps the pid, for the reason fetch_release_fd() gives: epoll reports
 * EOF on every wait, so an fd left registered after the child closed its end would stop
 * mesh_event_loop_run() ever returning on a timeout - and the deadline in tick() would never
 * be enforced.
 */
static void resolve_release_fd(struct mesh_resolve *resolve) {
    if (resolve->child_fd < 0) {
        return;
    }
    if (resolve->loop != NULL) {
        (void)mesh_event_loop_remove_fd(resolve->loop, resolve->child_fd);
    }
    close(resolve->child_fd);
    resolve->child_fd = -1;
}

/* Everything about the lookup in flight, gone. Does not call the callback. */
static void resolve_discard(struct mesh_resolve *resolve) {
    resolve_release_fd(resolve);
    if (resolve->child > 0) {
        int status = 0;
        if (waitpid(resolve->child, &status, WNOHANG) == 0) {
            kill(resolve->child, SIGKILL);
            (void)waitpid(resolve->child, &status, 0);
        }
        resolve->child = -1;
    }
    resolve->record_len = 0U;
    resolve->on_done = NULL;
    resolve->userdata = NULL;
    resolve->failure = MESH_RESOLVE_OK;
}

/*
 * Turns the record the child wrote into an outcome.
 *
 * The distinction this makes is the one the caller shows: a name that does not exist is the
 * user's to fix, and a resolver that did not work is not. EAI_NONAME and EAI_NODATA are the
 * two ways the first is said; everything else - EAI_AGAIN, EAI_FAIL, EAI_SYSTEM - is the
 * second, and says nothing about whether the name is good.
 */
static enum mesh_resolve_outcome resolve_outcome_of(const struct resolve_record *record) {
    if (record->error == 0) {
        return record->address_len > 0U ? MESH_RESOLVE_OK : MESH_RESOLVE_NOT_FOUND;
    }
    if (record->error == EAI_NONAME) {
        return MESH_RESOLVE_NOT_FOUND;
    }
#ifdef EAI_NODATA
    if (record->error == EAI_NODATA) {
        return MESH_RESOLVE_NOT_FOUND;
    }
#endif
    return MESH_RESOLVE_FAILED;
}

/*
 * Hands the outcome over, exactly once.
 *
 * Everything is lifted off the resolver and the resolver left idle *before* the callback runs,
 * the same re-entrancy rule fetch.c's completion keeps: a caller that starts its next lookup from
 * inside this one's completion is starting it against a clean resolver.
 */
static void resolve_complete(struct mesh_resolve *resolve, enum mesh_resolve_outcome outcome) {
    struct mesh_resolve_result result;
    memset(&result, 0, sizeof result);
    result.outcome = outcome;

    if (resolve->record_len >= sizeof(struct resolve_record)) {
        struct resolve_record record;
        memcpy(&record, resolve->record, sizeof record);
        result.error = (int)record.error;
        if (outcome == MESH_RESOLVE_OK) {
            result.address = record.address;
            result.address_len = (socklen_t)record.address_len;
        }
    }

    const mesh_resolve_done_fn done = resolve->on_done;
    void *const userdata = resolve->userdata;

    resolve_release_fd(resolve);
    resolve->child = -1;
    resolve->record_len = 0U;
    resolve->on_done = NULL;
    resolve->userdata = NULL;
    resolve->failure = MESH_RESOLVE_OK;

    if (done != NULL) {
        done(userdata, &result);
    }
}

/*
 * Reads whatever is buffered without blocking. Returns false when there will never be more -
 * EOF, or a failure already recorded in `failure` for the reap to report.
 */
static bool resolve_drain(struct mesh_resolve *resolve) {
    if (resolve->child_fd < 0) {
        return false;
    }
    for (;;) {
        if (resolve->record_len >= sizeof resolve->record) {
            /* A full record and the child still talking. It has nothing more to say that we
               would believe, so stop reading and let the reap report what we have. */
            return false;
        }
        const ssize_t got = read(resolve->child_fd, resolve->record + resolve->record_len,
                                 sizeof resolve->record - resolve->record_len);
        if (got > 0) {
            resolve->record_len += (size_t)got;
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
        resolve->failure = MESH_RESOLVE_FAILED;
        return false;
    }
}

/*
 * Finishes the lookup if the child has actually exited. Never blocks in waitpid, for the reason
 * fetch_try_finish() does not: this runs on the loop the UI draws on.
 *
 * Drains before reaping, because the exit and the EOF are separate events and either can be
 * seen first - reaping without draining would throw away an answer that had already arrived.
 */
static void resolve_try_finish(struct mesh_resolve *resolve) {
    if (resolve->child <= 0) {
        return;
    }
    if (!resolve_drain(resolve)) {
        resolve_release_fd(resolve);
    }

    int status = 0;
    if (waitpid(resolve->child, &status, WNOHANG) != resolve->child) {
        if (resolve->failure != MESH_RESOLVE_OK) {
            const enum mesh_resolve_outcome why = resolve->failure;
            kill(resolve->child, SIGKILL);
            (void)waitpid(resolve->child, &status, 0);
            resolve->child = -1;
            resolve_complete(resolve, why);
        }
        return;
    }
    resolve->child = -1;

    if (resolve->failure != MESH_RESOLVE_OK) {
        resolve_complete(resolve, resolve->failure);
        return;
    }
    if (resolve->record_len < sizeof(struct resolve_record)) {
        /* The child died before it finished writing - killed, or out of memory. Not an answer
           about the name either way. */
        resolve_complete(resolve, MESH_RESOLVE_FAILED);
        return;
    }

    struct resolve_record record;
    memcpy(&record, resolve->record, sizeof record);
    resolve_complete(resolve, resolve_outcome_of(&record));
}

static int resolve_on_child_output(int fd, uint32_t events, void *userdata) {
    (void)fd;
    struct mesh_resolve *resolve = (struct mesh_resolve *)userdata;
    if (resolve == NULL) {
        return 0;
    }
    if (resolve_drain(resolve) && (events & (EPOLLHUP | EPOLLERR)) == 0U) {
        return 0;
    }
    resolve_try_finish(resolve);
    return 0;
}

/* ------------------------------------------------------------------ api */

int mesh_resolve_init(struct mesh_resolve *resolve, struct mesh_event_loop *loop) {
    if (resolve == NULL) {
        return -EINVAL;
    }
    memset(resolve, 0, sizeof *resolve);
    resolve->child = -1;
    resolve->child_fd = -1;
    resolve->loop = loop;
    return 0;
}

void mesh_resolve_shutdown(struct mesh_resolve *resolve) {
    if (resolve == NULL) {
        return;
    }
    resolve_discard(resolve);
}

bool mesh_resolve_available(const struct mesh_resolve *resolve) {
    return resolve != NULL && resolve->loop != NULL;
}

bool mesh_resolve_busy(const struct mesh_resolve *resolve) {
    return resolve != NULL && resolve->child > 0;
}

int mesh_resolve_start(struct mesh_resolve *resolve, const char *host, uint16_t port,
                       mesh_resolve_done_fn on_done, void *userdata, uint64_t now_ms) {
    if (resolve == NULL || host == NULL || host[0] == '\0' || on_done == NULL) {
        return -EINVAL;
    }
    if (!mesh_resolve_available(resolve)) {
        return -ENOTSUP;
    }
    if (resolve->child > 0) {
        return -EBUSY;
    }
    resolve_discard(resolve);

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
        close(fds[0]);
        resolve_child(host, port, fds[1]);
        _exit(0); /* not reached; resolve_child() does not return */
    }

    close(fds[1]);
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) {
        const int err = -errno;
        close(fds[0]);
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return err;
    }

    resolve->child = pid;
    resolve->child_fd = fds[0];
    resolve->record_len = 0U;
    resolve->failure = MESH_RESOLVE_OK;
    resolve->deadline_ms = now_ms + MESH_RESOLVE_TIMEOUT_MS;
    resolve->on_done = on_done;
    resolve->userdata = userdata;

    const int added =
        mesh_event_loop_add_fd(resolve->loop, fds[0], EPOLLIN, resolve_on_child_output, resolve);
    if (added != 0) {
        resolve_discard(resolve);
        return added;
    }
    mesh_log_debug("resolve", "Looking up %s:%u", host, (unsigned)port);
    return 0;
}

void mesh_resolve_tick(struct mesh_resolve *resolve, uint64_t now_ms) {
    if (resolve == NULL || resolve->child <= 0) {
        return;
    }
    resolve_try_finish(resolve);
    if (resolve->child <= 0) {
        return;
    }
    if (now_ms >= resolve->deadline_ms) {
        resolve_release_fd(resolve);
        kill(resolve->child, SIGKILL);
        (void)waitpid(resolve->child, NULL, 0);
        resolve->child = -1;
        resolve_complete(resolve, MESH_RESOLVE_TIMED_OUT);
    }
}

void mesh_resolve_cancel(struct mesh_resolve *resolve) {
    if (resolve == NULL) {
        return;
    }
    resolve_discard(resolve);
}
