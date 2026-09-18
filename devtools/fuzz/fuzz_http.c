/*
 * The HTTP/1.1 codec, fed whatever a server sends.
 *
 * This is the reader in front of the self-updater: what it parses comes from a server on the
 * internet, through a TLS session that proves who sent it and nothing about whether it is well
 * formed. Memory safety is the first oracle, and three more hold over every input:
 *
 *   1. **The split does not matter.** The response is parsed once in a single feed and once in
 *      reads of a size the input chooses, and the two must agree on status, framing, error and
 *      every body byte. On a real connection where the reads split is up to the network; a parser
 *      that is only right when a line arrives whole is only right on a fast link.
 *   2. **Every feed is honest about what it consumed.** A body slice lies inside what was fed and
 *      inside what was consumed, a feed with bytes to give consumes at least one until the
 *      response is done or failed, and a Content-Length body never hands out more than it said.
 *   3. **A Location cannot write the next request.** Whatever redirect the server named, if it
 *      resolves, the request formatted from it is a request line and headers - a CR or LF that
 *      made it through would be a header chosen by the server.
 *
 * The first input byte picks HEAD or GET and the second the read size; the rest is the reply.
 *
 * Build with scripts/fuzz.sh; see docs/testing.md.
 */

#include "mesh/proto/http.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_BODY_MAX 65536U

static void fuzz_broke(const char *what) {
    fprintf(stderr, "http codec contract broken: %s\n", what);
    fflush(stderr);
    abort();
}

struct fuzz_run {
    struct mesh_http_response response;
    uint8_t body[FUZZ_BODY_MAX];
    size_t body_len;
    size_t consumed;
    bool finished;
};

static void fuzz_parse(struct fuzz_run *run, const uint8_t *data, size_t size, bool head,
                       size_t step) {
    mesh_http_response_init(&run->response, head);
    run->body_len = 0U;
    run->consumed = 0U;

    size_t at = 0U;
    bool stopped = false;
    while (at < size && !stopped) {
        const size_t window = step < size - at ? step : size - at;
        size_t used = 0U;
        while (used < window) {
            const uint8_t *body = NULL;
            size_t body_len = 0U;
            const uint8_t *const in = data + at + used;
            const size_t avail = window - used;
            const size_t consumed =
                mesh_http_response_feed(&run->response, in, avail, &body, &body_len);

            if (consumed > avail) {
                fuzz_broke("consumed more than it was given");
            }
            if (body_len > 0U) {
                if (body < in || body + body_len > in + consumed) {
                    fuzz_broke("a body slice outside what was consumed");
                }
                const size_t room = FUZZ_BODY_MAX - run->body_len;
                const size_t keep = body_len < room ? body_len : room;
                memcpy(run->body + run->body_len, body, keep);
                run->body_len += keep;
            }
            if (consumed == 0U) {
                if (!mesh_http_response_done(&run->response) &&
                    !mesh_http_response_failed(&run->response)) {
                    fuzz_broke("a live parser consumed nothing");
                }
                stopped = true;
                break;
            }
            used += consumed;
        }
        at += used;
    }
    run->consumed = at;
    run->finished = mesh_http_response_finish(&run->response);

    const struct mesh_http_response *response = &run->response;
    if (response->head_len >= MESH_HTTP_HEAD_MAX || response->head[response->head_len] != '\0') {
        fuzz_broke("a head that is not bounded and terminated");
    }
    if (response->framing == MESH_HTTP_FRAMING_LENGTH &&
        response->body_received > response->content_length) {
        fuzz_broke("more body than Content-Length");
    }
    if (mesh_http_response_done(response) && response->framing == MESH_HTTP_FRAMING_LENGTH &&
        response->body_received != response->content_length) {
        fuzz_broke("done with a Content-Length body short");
    }
    if (response->framing == MESH_HTTP_FRAMING_NONE && response->body_received != 0U) {
        fuzz_broke("a body where there is none");
    }
    if (mesh_http_response_done(response) == mesh_http_response_failed(response)) {
        fuzz_broke("after finish, a response is exactly one of done and failed");
    }
}

/* Oracle 3: the next request, from whatever Location this reply named. */
static void fuzz_redirect(const struct mesh_http_response *response) {
    const char *value = NULL;
    size_t len = 0U;
    if (!mesh_http_response_header(response, "location", &value, &len)) {
        return;
    }
    if (value < response->head || value + len > response->head + response->head_len) {
        fuzz_broke("a header value outside the head");
    }
    static char location[MESH_HTTP_HEAD_MAX];
    memcpy(location, value, len);
    location[len] = '\0';

    struct mesh_http_url base;
    if (!mesh_http_url_parse("https://github.com/o/r/releases/download/v1/a.bin?x=1", &base)) {
        fuzz_broke("the base URL did not parse");
    }
    static struct mesh_http_url next;
    if (!mesh_http_url_resolve(&base, location, &next)) {
        return;
    }
    static char request[MESH_HTTP_URL_MAX + 512U];
    const int written =
        mesh_http_request_format(request, sizeof request, MESH_HTTP_GET, &next, NULL, 0U);
    if (written < 0) {
        return;
    }
    /* Exactly three lines and the blank one: request line, Host, Connection. */
    unsigned lines = 0U;
    for (int i = 0; i < written; ++i) {
        const char c = request[i];
        if (c == '\n') {
            if (i == 0 || request[i - 1] != '\r') {
                fuzz_broke("a bare LF in a request");
            }
            lines++;
        } else if (c == '\r' && (i + 1 >= written || request[i + 1] != '\n')) {
            fuzz_broke("a bare CR in a request");
        }
    }
    if (lines != 4U) {
        fuzz_broke("a Location added a line to the request");
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2U) {
        return 0;
    }
    const bool head = (data[0] & 1U) != 0U;
    const size_t step = (size_t)data[1] % 64U + 1U;
    data += 2;
    size -= 2U;

    static struct fuzz_run whole;
    static struct fuzz_run split;
    fuzz_parse(&whole, data, size, head, size > 0U ? size : 1U);
    fuzz_parse(&split, data, size, head, step);

    if (whole.response.status != split.response.status ||
        whole.response.framing != split.response.framing ||
        whole.response.error != split.response.error || whole.finished != split.finished ||
        whole.consumed != split.consumed || whole.body_len != split.body_len ||
        memcmp(whole.body, split.body, whole.body_len) != 0 ||
        whole.response.head_len != split.response.head_len ||
        memcmp(whole.response.head, split.response.head, whole.response.head_len) != 0) {
        fuzz_broke("one feed and split feeds disagree");
    }

    fuzz_redirect(&whole.response);
    return 0;
}
