#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HTTP/1.1 on the wire, for a client that makes one request per connection.
 *
 * A codec, not a client, in the same way mqtt_packet.h is: it turns a URL and some header lines
 * into request bytes, and response bytes into a status, some headers and a body. It holds no
 * socket and no opinion about redirects, timeouts or where a body goes - that is
 * src/core/net/fetch.c. Keeping the two apart is what makes the fiddly half testable against byte
 * arrays and fuzzable, which matters more here than for most of this directory: what this reads
 * is the reply from a server on the internet, and it sits in front of the self-updater.
 *
 * **Only what a download needs.** `Connection: close` on every request, so there is no keep-alive
 * and no pipelining and a reply ends where the connection does or earlier. No `Accept-Encoding`
 * is sent, so a body arrives as the bytes it is - a digest is checked against it later, and a
 * gzip layer in between would be one more thing to trust. No HTTP/2: this is spoken after a TLS
 * handshake that offers no ALPN, so a server has nothing else to answer in.
 *
 * **Strict where being lenient is how HTTP goes wrong.** Two different `Content-Length`s, a
 * `Transfer-Encoding` other than `chunked`, a header folded across lines, a chunk size that
 * overflows - each of these is refused rather than guessed at, because every one of them is a
 * reply whose length two readers could disagree about. Bare `\n` line endings are accepted, as
 * RFC 9112 allows a recipient to.
 */

/* The status line and headers of one response, kept whole. GitHub's are 3-5 KB, most of it
   security policy; a redirect to a signed asset URL adds a `Location` over a kilobyte long. */
#define MESH_HTTP_HEAD_MAX 16384U
/* A URL, and so a `Location` - the signed release-asset redirects are the long ones. */
#define MESH_HTTP_URL_MAX 4096U
#define MESH_HTTP_HOST_MAX 256U

/* ------------------------------------------------------------------ URLs */

struct mesh_http_url {
    bool tls; /* https */
    /* No brackets, even for a v6 literal; mesh_http_request_format() puts them back. */
    char host[MESH_HTTP_HOST_MAX];
    uint16_t port;
    /* The request target: path and query, always starting with '/'. No fragment. */
    char target[MESH_HTTP_URL_MAX];
};

/*
 * Parses an absolute `http://` or `https://` URL. False for anything else: another scheme, a
 * userinfo (`user@host`), an empty host, a bad port, or a URL that does not fit.
 *
 * **A space or control byte anywhere is a refusal**, not something to escape. A `Location` goes
 * back out in the next request's first line, and a CR/LF in it would be a header the server
 * chose; a URL with a space in it was never valid to begin with.
 */
bool mesh_http_url_parse(const char *url, struct mesh_http_url *out);

/*
 * Resolves a `Location` against the URL that answered with it. Absolute (`https://...`),
 * scheme-relative (`//host/...`), absolute-path (`/...`), relative (`name`), query-only (`?q`)
 * and fragment-only (`#f`) references are all understood; dot segments are passed through for the
 * server to resolve rather than normalised here. False on the same grounds as
 * mesh_http_url_parse().
 */
bool mesh_http_url_resolve(const struct mesh_http_url *base, const char *location,
                           struct mesh_http_url *out);

/* ------------------------------------------------------------------ the request */

enum mesh_http_method {
    MESH_HTTP_GET = 0,
    MESH_HTTP_HEAD,
};

/*
 * Writes a whole request - request line, `Host`, `Connection: close`, then `headers` (whole
 * lines without their CRLF, read up to the first NULL or `header_count`) - into `out`.
 *
 * Returns the length, or -1 when it does not fit or a header line carries a CR or LF: a caller's
 * header is one line, and one that is not would be a second header nobody wrote.
 */
int mesh_http_request_format(char *out, size_t cap, enum mesh_http_method method,
                             const struct mesh_http_url *url, const char *const *headers,
                             size_t header_count);

/* ------------------------------------------------------------------ the response */

/* How the end of the body is found, once the head is in. */
enum mesh_http_framing {
    MESH_HTTP_FRAMING_NONE = 0, /* no body: a HEAD, a 204, a 304 */
    MESH_HTTP_FRAMING_LENGTH,   /* Content-Length */
    MESH_HTTP_FRAMING_CHUNKED,  /* Transfer-Encoding: chunked */
    /*
     * Neither: the body is everything until the connection closes. Legal, and the one framing in
     * which a cut connection looks exactly like a finished body - so whoever holds the connection
     * decides whether to believe the close (under TLS, whether it was a close_notify).
     */
    MESH_HTTP_FRAMING_UNTIL_CLOSE,
};

/* Why a response was refused. Each names what was wrong with the bytes, not what to do. */
enum mesh_http_error {
    MESH_HTTP_OK = 0,
    MESH_HTTP_ERR_STATUS_LINE, /* not `HTTP/1.x NNN ...` */
    MESH_HTTP_ERR_HEADER,      /* a header line that is not `name: value`, or is folded */
    MESH_HTTP_ERR_HEAD_TOO_LARGE,
    MESH_HTTP_ERR_LENGTH,    /* a Content-Length that is not a number, or two that disagree */
    MESH_HTTP_ERR_ENCODING,  /* a Transfer-Encoding other than `chunked` */
    MESH_HTTP_ERR_CHUNK,     /* a chunk size line or chunk terminator that is not one */
    MESH_HTTP_ERR_UPGRADE,   /* a 101: this never asks to switch protocols */
    MESH_HTTP_ERR_TRUNCATED, /* the connection ended before the body did */
    MESH_HTTP_ERROR_COUNT,
};

struct mesh_http_response {
    /* ---- the answer, valid once mesh_http_response_head_done() */
    int status;
    enum mesh_http_framing framing;
    /* The Content-Length, when framing is LENGTH. */
    uint64_t content_length;
    /* Body bytes handed out so far - decoded, so a chunked body's sizes and CRLFs are not in it. */
    uint64_t body_received;
    enum mesh_http_error error;

    /* The status line and header lines of the final response, as received and terminated by
       the empty line; NUL-terminated. A HEAD's whole answer, and what header() reads. */
    char head[MESH_HTTP_HEAD_MAX];
    size_t head_len;

    /* ---- the parser's own */
    uint8_t phase;
    bool head_request;
    size_t line_len;      /* bytes in the current line so far, not counting its LF */
    bool line_cr;         /* ... and the last of them was a CR */
    uint64_t remaining;   /* of the Content-Length, or of the current chunk */
    char chunk_line[128]; /* a chunk-size line, extensions and all */
    size_t chunk_line_len;
    size_t trailer_len; /* bytes of trailer seen, bounded like a head */
};

/*
 * Ready for one response. `head_request` because a reply to HEAD carries the headers of a body
 * it does not send, and nothing in the reply itself says so.
 */
void mesh_http_response_init(struct mesh_http_response *response, bool head_request);

/*
 * Feeds what arrived. Returns how many bytes of `in` were consumed, and points `*body` at up to
 * that many bytes of decoded body - a slice of `in`, valid as long as `in` is - or sets
 * `*body_len` to 0.
 *
 * Call it in a loop until everything is consumed. It stops early, deliberately, at two points:
 * when the head completes, so the caller can look at the status before the first body byte
 * (a redirect's body is not the document), and after every body slice, since a chunked body's
 * bytes are not contiguous in `in`. **Every call with bytes to give consumes at least one** until
 * the response is done or has failed; after that it consumes none, and anything left over is not
 * part of this response.
 */
size_t mesh_http_response_feed(struct mesh_http_response *response, const uint8_t *in, size_t len,
                               const uint8_t **body, size_t *body_len);

/*
 * The connection has closed. True when that is a legitimate end - the response was complete, or
 * its body was framed as UNTIL_CLOSE - and false, with error set to TRUNCATED if nothing else was
 * wrong, when it cut the response short.
 */
bool mesh_http_response_finish(struct mesh_http_response *response);

bool mesh_http_response_head_done(const struct mesh_http_response *response);
/* The whole response is in: head and all of the body its framing promised. */
bool mesh_http_response_done(const struct mesh_http_response *response);
bool mesh_http_response_failed(const struct mesh_http_response *response);

/*
 * The value of the first header named `name`, compared without case, with the whitespace around
 * it trimmed. Not NUL-terminated - it points into `head`. False when there is none, or before the
 * head is done.
 */
bool mesh_http_response_header(const struct mesh_http_response *response, const char *name,
                               const char **value, size_t *value_len);

/* A fixed English name for a log line - "chunk", "truncated". Never NULL. */
const char *mesh_http_error_name(enum mesh_http_error error);

#ifdef __cplusplus
}
#endif
