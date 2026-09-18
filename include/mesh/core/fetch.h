#pragma once

/*
 * One HTTPS request, made in this process on the event loop.
 *
 * Everything this client downloads comes through here: the self-update check and the binary
 * (src/core/update/updater.c), and the radio's firmware index, hardware list and release zip. It
 * is the TLS session the MQTT proxy uses (tls_client.h) under the HTTP/1.1 codec in
 * mesh/proto/http.h - resolve, connect, handshake, one request, one reply, and a redirect is the
 * same again against the URL it named. Nothing is forked but the name lookup, which forks without
 * exec (resolve.h), so there is no program the device has to have and no CA bundle it has to
 * carry: the roots are compiled in (ca_roots.h).
 *
 * What this module is not is a state machine. It knows nothing about what is being fetched or
 * what should happen next; it runs one request, and when that is over it says how it went
 * exactly once. Which state that lands in is the caller's - `mesh_update_state` for the client's
 * own binary, and the radio firmware's own for the other one.
 *
 * One request at a time, per fetcher. A caller that needs two documents runs them in sequence,
 * and may start the second from inside the first's completion: the fetcher is fully idle by the
 * time the callback runs, and the body it was handed outlives the call.
 *
 * **https only, and verified.** A plain `http://` URL is refused at start, and so is a redirect
 * to one: what comes down these connections is a release's digest and then the file it is
 * checked against, and a digest that arrived in the clear proves nothing.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh/core/resolve.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_event_loop;
struct mesh_fetch_conn; /* the request in flight; defined in src/core/net/fetch.c */

#define MESH_FETCH_PATH_MAX 256U
/* How many headers one request may carry. The updater uses two (Accept and User-Agent) and a
   range read one; the array is fixed so a request is a value with no allocation. */
#define MESH_FETCH_HEADERS_MAX 4U
/* One header line, as the caller wrote it. */
#define MESH_FETCH_HEADER_MAX 256U
/* A JSON reply that is a few KB, with room for the ones that are a few tens of KB. Anything
   past a request's own cap is a reply we would not trust anyway. */
#define MESH_FETCH_RESPONSE_MAX 65536U
/* Hops a request may take before the chain is refused. A GitHub release asset is one: github.com
   answers 302 to its CDN. */
#define MESH_FETCH_REDIRECTS_MAX 5U

/* How a finished request ended. Every one of these is a sentence the caller has to be able to
   put on a screen, so they are told apart by what the reader would do next. */
enum mesh_fetch_outcome {
    MESH_FETCH_OK = 0,
    /* The server answered, and not with the document: a 404, a 500, a 3xx with nowhere to go.
       `status` is the code. */
    MESH_FETCH_HTTP_STATUS,
    MESH_FETCH_TOO_LARGE, /* the reply passed the request's cap and was abandoned */
    /* The server could not be reached, or the connection dropped before the reply was whole: a
       name that does not resolve, a refused connect, a body cut short. Worth retrying. */
    MESH_FETCH_NETWORK,
    /* The server's certificate did not verify, or the handshake failed. Not worth retrying. */
    MESH_FETCH_TLS,
    /* The reply was not HTTP this client will act on: malformed, a redirect off https or past
       MESH_FETCH_REDIRECTS_MAX, or a range request answered with the whole file. */
    MESH_FETCH_PROTOCOL,
    MESH_FETCH_FILE,      /* output_path could not be written */
    MESH_FETCH_TIMED_OUT, /* the request's deadline passed */
    MESH_FETCH_OUTCOME_COUNT,
};

struct mesh_fetch_result {
    enum mesh_fetch_outcome outcome;
    /* The final reply's HTTP status, or 0 when no reply arrived. */
    int status;
    /*
     * The body, always NUL-terminated, or NULL when the request wrote to a file or the reply had
     * none. For a HEAD, the final reply's status line and headers. Valid for the duration of the
     * callback and freed after it, so a caller that wants to keep any of it copies it out.
     */
    const char *body;
    size_t len;
    /* What went wrong, in words, for a log line - a TLS error, an errno, what was wrong with the
       reply. "" on success; never NULL. Valid for the duration of the callback. */
    const char *detail;
};

/*
 * What a request asks for. GET is the body; HEAD is the headers and nothing else.
 *
 * HEAD exists for one reason and it is not tidiness: a zip is read from the back, the CDN in
 * front of these files answers `501 Unsupported client range` to a suffix range, and so the only
 * way to ask for the last 64 KB of a file is to know how long it is first. The final reply's head
 * is captured as the body, which is what `mesh_fetch_content_length()` reads.
 */
enum mesh_fetch_method {
    MESH_FETCH_GET = 0,
    MESH_FETCH_HEAD,
};

/* Called once per started request, from the event loop, when it is over. */
typedef void (*mesh_fetch_done_fn)(void *userdata, const struct mesh_fetch_result *result);

struct mesh_fetch_request {
    const char *url;
    enum mesh_fetch_method method;
    /*
     * Whole header lines - "Accept: application/vnd.github+json" - sent on every hop. Entries are
     * read until the first NULL, so a request that sets none leaves the array zeroed. A
     * `User-Agent` is added when none is given, since GitHub's API refuses a request without one.
     *
     * A `Range:` line makes this a range request, and a range request must be answered `206`: a
     * server that ignores the range sends the whole file, and a caller that asked for 64 KB of a
     * 100 MB zip is owed a failure rather than the zip.
     */
    const char *headers[MESH_FETCH_HEADERS_MAX];
    /*
     * Where the body goes. NULL captures it in memory and hands it to the callback; a path
     * streams it into that file as it arrives, which is what makes a download's progress a
     * stat() on a file this process named. The file is created only once a 2xx has arrived, so a
     * 404's error page never lands where a download was expected.
     */
    const char *output_path;
    /* The whole request - every lookup, hop and byte - gets this long, or TIMED_OUT. 0 is 30 s. */
    uint32_t timeout_ms;
    /* Cap on a captured reply; 0 takes MESH_FETCH_RESPONSE_MAX. Ignored with output_path. */
    size_t response_max;
    mesh_fetch_done_fn on_done;
    void *userdata;
};

struct mesh_fetch {
    struct mesh_event_loop *loop;
    struct mesh_resolve resolve;
    /* The request in flight, or NULL. Heap-held and only while one runs: it carries a reply head
       and a read buffer, which is tens of KB this struct's three owners should not hold idle. */
    struct mesh_fetch_conn *conn;
    /* The loop's clock as of the last start() or tick(), for the lookups a redirect starts. */
    uint64_t now_ms;
    /* See mesh_fetch_connect_to(). Empty for the address a URL's host resolves to. */
    char connect_host[64];
    uint16_t connect_port;
};

/*
 * `loop` may be NULL, in which case the fetcher reports itself unavailable - which is what a test
 * that never means to connect anywhere wants. Returns 0, or -errno.
 */
int mesh_fetch_init(struct mesh_fetch *fetch, struct mesh_event_loop *loop);

/* Abandons anything in flight without reporting it, and releases everything held. */
void mesh_fetch_shutdown(struct mesh_fetch *fetch);

/* True when this build has TLS and there is a loop to run a request on. */
bool mesh_fetch_available(const struct mesh_fetch *fetch);
/* True while a request is running. A second is refused with -EBUSY. */
bool mesh_fetch_busy(const struct mesh_fetch *fetch);

/*
 * Every connection goes to `host`:`port` - a numeric address - instead of wherever a URL's host
 * resolves, the way curl's `--connect-to` does. The URL's host is still what is sent as SNI and
 * `Host`, and what the certificate is checked against, so nothing about verification changes.
 * For tests, which stand a server on loopback and point real URLs at it. NULL puts it back.
 */
void mesh_fetch_connect_to(struct mesh_fetch *fetch, const char *host, uint16_t port);

/*
 * Starts `request`. Returns 0, or -errno: -ENOTSUP when unavailable, -EBUSY with a request
 * already running, -EINVAL for a request with no callback or a URL that is not https, -ENOMEM.
 * On any error nothing was started and `on_done` will not be called.
 *
 * On 0 the callback is called exactly once, later, from the loop - never before this returns.
 */
int mesh_fetch_start(struct mesh_fetch *fetch, const struct mesh_fetch_request *request,
                     uint64_t now_ms);

/*
 * Enforces the deadline, reaps a finished lookup, and resumes a read that gave the loop back
 * early. Call every loop turn.
 */
void mesh_fetch_tick(struct mesh_fetch *fetch, uint64_t now_ms);

/* Reads the `Content-Length` out of a captured HEAD reply. False when there is none. */
bool mesh_fetch_content_length(const char *headers, size_t len, uint64_t *out);

/*
 * Abandons anything in flight: the connection is dropped and the callback is *not* called. For
 * a caller that has decided the outcome itself - a cancelled step, a shutdown - and does not want
 * a completion arriving on top of the decision. A partly written output_path is left for the
 * caller, who named it, to remove.
 */
void mesh_fetch_cancel(struct mesh_fetch *fetch);

/* A fixed English name for an outcome, for a log line. Never NULL. */
const char *mesh_fetch_outcome_name(enum mesh_fetch_outcome outcome);

#ifdef __cplusplus
}
#endif
