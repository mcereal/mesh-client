#pragma once

/*
 * One HTTPS GET, done by forking the device's own curl (or wget) and reading its stdout
 * through the event loop.
 *
 * There is no TLS in this process - the release build is a static musl binary whose only
 * dependency is libdbus - so every fetch this client makes is a child process. That shape was
 * written first for self-update (src/core/updater.c) and it is now shared, because the radio's
 * firmware needs the same thing: fetcher probing, the CA bundle the Brick has no system store
 * for, a per-step deadline, a cap on what a reply may grow to, and a reap that never blocks
 * the loop the UI draws on.
 *
 * What this module is not is a state machine. It knows nothing about what is being fetched or
 * what should happen next; it runs one request, and when the child is gone it says how that
 * went exactly once. Which state that lands in is the caller's - `mesh_update_state` for the
 * client's own binary, and the radio firmware's own for the other one.
 *
 * One child at a time, per fetcher. A caller that needs two documents runs them in sequence,
 * and may start the second from inside the first's completion: the fetcher is fully idle by
 * the time the callback runs, and the body it was handed outlives the call.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;

#define MESH_FETCH_PATH_MAX 256U
/* How many headers one request may carry. Both callers use two (Accept and User-Agent) and a
   range read adds a third; the array is fixed so a request is a value with no allocation. */
#define MESH_FETCH_HEADERS_MAX 4U
/* A JSON reply that is a few KB, with room for the ones that are a few tens of KB. Anything
   past a request's own cap is a reply we would not trust anyway. */
#define MESH_FETCH_RESPONSE_MAX 65536U

/* How a finished request ended. Every one of these is a sentence the caller has to be able to
   put on a screen, which is why the read failures are told apart rather than collapsed. */
enum mesh_fetch_outcome {
    MESH_FETCH_OK = 0,
    /* The fetcher ran and exited non-zero; `status` is what with. curl's 60 is the one worth
       reading - "peer certificate cannot be authenticated", i.e. no CA bundle. */
    MESH_FETCH_EXITED,
    MESH_FETCH_TOO_LARGE,   /* the reply passed the request's cap and was abandoned */
    MESH_FETCH_READ_FAILED, /* the pipe itself failed */
    MESH_FETCH_TIMED_OUT,   /* the deadline passed and the child was killed */
    MESH_FETCH_OUTCOME_COUNT,
};

struct mesh_fetch_result {
    enum mesh_fetch_outcome outcome;
    /* The child's exit status, or -1 when it was signalled. Only meaningful for EXITED. */
    int status;
    /*
     * Captured stdout, always NUL-terminated, or NULL when the request wrote to a file or the
     * child produced nothing. Valid for the duration of the callback and freed after it, so a
     * caller that wants to keep any of it copies it out.
     */
    const char *body;
    size_t len;
};

/* Called once per started request, from the event loop, when the child is gone. */
typedef void (*mesh_fetch_done_fn)(void *userdata, const struct mesh_fetch_result *result);

struct mesh_fetch_request {
    const char *url;
    /*
     * Whole header lines - "Accept: application/vnd.github+json" - passed to curl with -H and
     * to wget with --header=. Entries are read until the first NULL, so a request that sets
     * none simply leaves the array zeroed.
     */
    const char *headers[MESH_FETCH_HEADERS_MAX];
    /*
     * Where the body goes. NULL captures it in memory and hands it to the callback; a path
     * writes it straight to that file, which is what makes a download's progress a stat() on
     * a file this process named rather than a scrape of curl's terminal meter.
     */
    const char *output_path;
    /* When this passes, the child is killed and the outcome is TIMED_OUT. */
    uint32_t timeout_ms;
    /* Cap on a captured reply; 0 takes MESH_FETCH_RESPONSE_MAX. Ignored with output_path. */
    size_t response_max;
    mesh_fetch_done_fn on_done;
    void *userdata;
};

struct mesh_fetch {
    struct mesh_event_loop *loop;
    /* "curl", "wget", or NULL when the device has neither and fetching is unavailable. */
    const char *tool;
    /* Handed to the fetcher, or empty to leave it on its own defaults. The Brick has no system
       CA store at all, so without one every HTTPS fetch fails; see
       mesh_fetch_resolve_ca_bundle(). */
    char ca_bundle[MESH_FETCH_PATH_MAX];

    /* The running child, or -1. Only ever one. */
    pid_t child;
    int child_fd;
    uint64_t deadline_ms;

    /* Captured stdout so far, and the cap the request set. */
    char *response;
    size_t response_len;
    size_t response_max;
    /* Set by a drain that gave up, so the reap reports why rather than the exit status. */
    enum mesh_fetch_outcome failure;

    mesh_fetch_done_fn on_done;
    void *userdata;
};

/*
 * Probes PATH for curl, then wget. `loop` may be NULL, in which case the fetcher reports
 * itself unavailable - which is what a test that never means to spawn anything wants. Returns
 * 0, or -errno.
 */
int mesh_fetch_init(struct mesh_fetch *fetch, struct mesh_event_loop *loop);

/* Kills anything in flight without reporting it, and releases everything held. */
void mesh_fetch_shutdown(struct mesh_fetch *fetch);

/* True when a fetcher was found and there is a loop to read it through. */
bool mesh_fetch_available(const struct mesh_fetch *fetch);
/* True while a child is running. A second request is refused with -EBUSY. */
bool mesh_fetch_busy(const struct mesh_fetch *fetch);
/* "curl", "wget", or "fetcher" when there is none - always safe to print. */
const char *mesh_fetch_tool(const struct mesh_fetch *fetch);

/*
 * Pick the CA bundle every request will verify against.
 *
 * The Brick has no system CA store - no /etc/ssl at all - so curl rejects every HTTPS request
 * with exit 60 and nothing that needs the network could work on the one device this ships for.
 * `--insecure` is not the way out: what comes down these connections is a digest that a
 * download is later checked against, so trusting the metadata over an unauthenticated channel
 * would defeat the verification rather than work around a missing file.
 *
 * Order: an explicit environment override first (curl honours SSL_CERT_FILE and CURL_CA_BUNDLE
 * itself, so this only records what is already in effect), then `shipped` - the bundle inside
 * our own pak, which the caller finds because only the caller knows where the pak is - then the
 * usual system locations so a desktop build keeps using the distribution's certificates.
 * Finding nothing is not fatal, since curl may have a built-in default; it lets the caller say
 * something more useful than "exit 60" when it turns out there was none.
 */
void mesh_fetch_resolve_ca_bundle(struct mesh_fetch *fetch, const char *shipped);

/*
 * Starts `request`. Returns 0, or -errno: -ENOTSUP with no fetcher, -EBUSY with a child
 * already running, -EINVAL for a request with no URL and no callback. On any error nothing was
 * spawned and `on_done` will not be called.
 *
 * On 0 the callback is called exactly once, later, from the loop.
 */
int mesh_fetch_start(struct mesh_fetch *fetch, const struct mesh_fetch_request *request,
                     uint64_t now_ms);

/*
 * Enforces the deadline and reaps a finished child. Call every loop turn.
 *
 * Both halves matter and neither is redundant: the fd callback sees EOF, but a child that
 * exited without closing stdout, or one whose EOF the loop did not deliver, is only ever
 * finished here.
 */
void mesh_fetch_tick(struct mesh_fetch *fetch, uint64_t now_ms);

/*
 * Abandons anything in flight: the child is killed, the buffer dropped and the callback is
 * *not* called. For a caller that has decided the outcome itself - a cancelled step, a
 * shutdown - and does not want a completion arriving on top of the decision.
 */
void mesh_fetch_cancel(struct mesh_fetch *fetch);

#ifdef __cplusplus
}
#endif
