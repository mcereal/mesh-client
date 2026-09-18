#pragma once

/*
 * One hostname turned into a socket address, done by forking a child that is allowed to block.
 *
 * `getaddrinfo()` is the one POSIX call this client cannot make on its own thread: it blocks for
 * as long as the network takes to answer, there is no non-blocking form, and `getaddrinfo_a()`
 * starts threads - so a name typed on the Devices tab would be seconds of frozen UI. That is why
 * the TCP link took a numeric address and nothing else for as long as it did.
 *
 * The way out: fork, let the child block, read the answer back through the event loop.
 * The child does not exec. There is nothing to exec - `getent` is not on the Brick and busybox's
 * `nslookup` prints a different thing every version - and the resolver we want is the one this
 * binary is already linked against.
 *
 * **A literal is not a lookup.** mesh_resolve_literal() answers `192.168.1.50` and `fd00::1` with
 * inet_pton and no child at all, which is both faster and what keeps the behaviour a caller
 * already had for an address exactly as it was. A caller checks that first and only starts a
 * lookup for what is left; start() therefore always forks and always reports later, and never
 * calls back before it returns.
 *
 * One lookup at a time. A second is refused with -EBUSY, which is all either caller needs: a link
 * resolves one host because it is about to connect to one host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;

/* How long a lookup is given before the child is killed and the outcome is TIMED_OUT. A DNS
   server that is there answers in milliseconds; one that is not is what this is for. */
#define MESH_RESOLVE_TIMEOUT_MS 5000U

/* How a finished lookup ended. Each of these is a different sentence on a screen, which is why
   "there is no such name" is not folded in with "the resolver did not work". */
enum mesh_resolve_outcome {
    MESH_RESOLVE_OK = 0,
    /* The name does not resolve: NXDOMAIN, or a name with no address record. The user typed
       something wrong, or the host is not on this network. */
    MESH_RESOLVE_NOT_FOUND,
    /* The lookup itself did not work - no resolver configured, no route to the DNS server, the
       child could not be read. Says nothing about whether the name exists. */
    MESH_RESOLVE_FAILED,
    MESH_RESOLVE_TIMED_OUT,
    MESH_RESOLVE_OUTCOME_COUNT,
};

struct mesh_resolve_result {
    enum mesh_resolve_outcome outcome;
    /*
     * The `EAI_*` code the child got, or 0. Worth logging through gai_strerror() and not worth
     * showing: "Temporary failure in name resolution" is not a sentence that helps somebody
     * holding a handheld, which is what the outcome above is for.
     */
    int error;
    /*
     * The address, with the port already set, valid only when the outcome is OK.
     *
     * **The first one `getaddrinfo()` returned**, not a list. A caller here is connecting to a
     * radio on the local network or to a broker, and neither is a case where walking a second
     * A record is what fixes a failed connect - a retry goes back through the whole attempt,
     * name included, which is also how it picks up a DHCP lease that moved.
     */
    struct sockaddr_storage address;
    socklen_t address_len;
};

/* Called once per started lookup, from the event loop, when the child is gone. */
typedef void (*mesh_resolve_done_fn)(void *userdata, const struct mesh_resolve_result *result);

struct mesh_resolve {
    struct mesh_event_loop *loop;

    /* The running child, or -1. Only ever one. */
    pid_t child;
    int child_fd;
    uint64_t deadline_ms;

    /* The child writes one fixed-size record; this is how much of it has arrived. */
    uint8_t record[sizeof(struct sockaddr_storage) + 8U];
    size_t record_len;
    /* Set by a read that gave up, so the reap reports why rather than the exit status. */
    enum mesh_resolve_outcome failure;

    mesh_resolve_done_fn on_done;
    void *userdata;
};

/*
 * Fills `out` from a numeric host - v4 or v6, no brackets - and sets the port.
 *
 * True when `host` was a literal and nothing needs looking up. False means it is a name, which
 * is the caller's cue to start a lookup; it is not an error.
 */
bool mesh_resolve_literal(const char *host, uint16_t port, struct sockaddr_storage *out,
                          socklen_t *out_len);

/* `loop` may be NULL, in which case the resolver reports itself unavailable - which is what a
   test that never means to fork anything wants. Returns 0, or -errno. */
int mesh_resolve_init(struct mesh_resolve *resolve, struct mesh_event_loop *loop);

/* Kills anything in flight without reporting it, and releases everything held. */
void mesh_resolve_shutdown(struct mesh_resolve *resolve);

/* True when there is a loop to read a child through. */
bool mesh_resolve_available(const struct mesh_resolve *resolve);
/* True while a lookup is running. A second is refused with -EBUSY. */
bool mesh_resolve_busy(const struct mesh_resolve *resolve);

/*
 * Starts looking `host` up, with `port` written into whatever comes back.
 *
 * Returns 0, or -errno: -ENOTSUP with no loop, -EBUSY with a lookup already running, -EINVAL for
 * an empty name or no callback. On any error nothing was forked and `on_done` will not be called.
 *
 * On 0 the callback is called exactly once, later, from the loop. It is never called before this
 * returns, including for a name that turns out to be a literal - see mesh_resolve_literal().
 */
int mesh_resolve_start(struct mesh_resolve *resolve, const char *host, uint16_t port,
                       mesh_resolve_done_fn on_done, void *userdata, uint64_t now_ms);

/*
 * Enforces the deadline and reaps a finished child. Call every loop turn.
 *
 * Both halves matter: the fd callback sees EOF, but a
 * child that wrote its answer and has not yet been reaped is only ever finished here.
 */
void mesh_resolve_tick(struct mesh_resolve *resolve, uint64_t now_ms);

/*
 * Abandons anything in flight: the child is killed and the callback is *not* called. For a
 * caller that has decided the outcome itself - a link torn down while a name was being looked
 * up - and does not want a completion arriving on top of the decision.
 */
void mesh_resolve_cancel(struct mesh_resolve *resolve);

#ifdef __cplusplus
}
#endif
