/*
 * The HTTP/1.1 codec, against bytes.
 *
 * Every response here is written out by hand, the way a server would send it, and every one is
 * parsed twice: in one feed, and a byte at a time. The two must agree on everything - status,
 * framing, error and every body byte - because on a real connection where the reads split is up
 * to the network, and a parser that is right only when a line arrives whole is right only on a
 * fast link.
 */

#include "framework/mesh_test.h"

#include "mesh/proto/http.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ harness */

struct parsed {
    struct mesh_http_response response;
    char body[512];
    size_t body_len;
    /* The input left over after the response was done, which is never part of it. */
    size_t unconsumed;
    bool finished; /* what finish() said, when the harness called it */
};

/*
 * Feeds `text` `step` bytes at a time (0: all at once), collecting the body. `close` calls
 * finish() at the end, as a connection closing would.
 */
static void feed(struct parsed *out, const char *text, size_t len, bool head_request, size_t step,
                 bool close) {
    memset(out, 0, sizeof *out);
    mesh_http_response_init(&out->response, head_request);
    const uint8_t *in = (const uint8_t *)text;
    size_t at = 0U;
    while (at < len) {
        size_t window = step == 0U ? len - at : step;
        if (window > len - at) {
            window = len - at;
        }
        size_t used = 0U;
        while (used < window) {
            const uint8_t *body = NULL;
            size_t body_len = 0U;
            const size_t consumed = mesh_http_response_feed(&out->response, in + at + used,
                                                            window - used, &body, &body_len);
            if (body_len > 0U && out->body_len + body_len < sizeof out->body) {
                memcpy(out->body + out->body_len, body, body_len);
            }
            out->body_len += body_len;
            if (consumed == 0U) {
                break;
            }
            used += consumed;
        }
        at += used;
        if (used < window) {
            out->unconsumed = len - at;
            break;
        }
    }
    if (close) {
        out->finished = mesh_http_response_finish(&out->response);
    }
}

/*
 * Parses both ways into `whole` and asserts they agree. False, with a failure recorded, when
 * they do not - which is itself the bug.
 */
static bool parse(const char *test_name, struct parsed *whole, const char *text, bool head_request,
                  bool close) {
    static struct parsed bytewise;
    const size_t len = strlen(text);
    feed(whole, text, len, head_request, 0U, close);
    feed(&bytewise, text, len, head_request, 1U, close);
    if (whole->response.status != bytewise.response.status ||
        whole->response.error != bytewise.response.error ||
        whole->response.framing != bytewise.response.framing ||
        whole->body_len != bytewise.body_len ||
        memcmp(whole->body, bytewise.body,
               whole->body_len < sizeof whole->body ? whole->body_len : sizeof whole->body) != 0 ||
        whole->unconsumed != bytewise.unconsumed || whole->finished != bytewise.finished ||
        mesh_http_response_done(&whole->response) != mesh_http_response_done(&bytewise.response)) {
        record_failure(test_name, "one feed and a byte at a time should parse the same");
        return false;
    }
    return true;
}

static bool body_is(const struct parsed *parsed, const char *want) {
    return parsed->body_len == strlen(want) && memcmp(parsed->body, want, parsed->body_len) == 0;
}

static bool header_is(const struct mesh_http_response *response, const char *name,
                      const char *want) {
    const char *value = NULL;
    size_t len = 0U;
    return mesh_http_response_header(response, name, &value, &len) && len == strlen(want) &&
           memcmp(value, want, len) == 0;
}

/* ------------------------------------------------------------------ URLs */

MESH_TEST_CASE(http_url_parses_the_parts, unit) {
    struct mesh_http_url url;
    MESH_TEST_FAIL_IF(
        !mesh_http_url_parse("https://api.github.com/repos/a/b/releases?x=1#frag", &url),
        "an ordinary https URL should parse");
    MESH_TEST_FAIL_IF(!url.tls || url.port != 443U || strcmp(url.host, "api.github.com") != 0,
                      "https should default to 443 and keep the host");
    MESH_TEST_FAIL_IF(strcmp(url.target, "/repos/a/b/releases?x=1") != 0,
                      "the target is path and query, never the fragment");

    MESH_TEST_FAIL_IF(!mesh_http_url_parse("HTTP://example.org:8080", &url),
                      "a scheme is case-insensitive and a path is optional");
    MESH_TEST_FAIL_IF(url.tls || url.port != 8080U || strcmp(url.target, "/") != 0,
                      "an explicit port is kept and an empty path is '/'");

    MESH_TEST_FAIL_IF(!mesh_http_url_parse("https://example.org?q", &url) ||
                          strcmp(url.target, "/?q") != 0,
                      "a query with no path still gets a '/'");
    MESH_TEST_FAIL_IF(!mesh_http_url_parse("https://[fd00::1]:8443/x", &url) ||
                          strcmp(url.host, "fd00::1") != 0 || url.port != 8443U,
                      "a v6 literal loses its brackets and keeps its port");
    MESH_TEST_FAIL_IF(!mesh_http_url_parse("https://example.org:/x", &url) || url.port != 443U,
                      "an empty port is the default one");
    record_success(test_name);
}

MESH_TEST_CASE(http_url_refuses_what_it_cannot_send, unit) {
    static const char *const k_bad[] = {
        "ftp://example.org/",          /* not HTTP */
        "example.org/path",            /* not absolute */
        "https://user:pw@example.org", /* userinfo */
        "https:///path",               /* no host */
        "https://example.org:0/",      /* no port 0 */
        "https://example.org:65536/",  /* nor past 65535 */
        "https://example.org:80a/",
        "https://example.org/a b",            /* a space */
        "https://example.org/a\r\nX-Evil: 1", /* a header smuggled in on the request line */
        "https://[fd00::1/",                  /* unclosed bracket */
        "https://[fd00::1]x/",
        "https://[not-v6]/",
    };
    for (size_t i = 0U; i < sizeof k_bad / sizeof k_bad[0]; ++i) {
        struct mesh_http_url url;
        if (mesh_http_url_parse(k_bad[i], &url)) {
            char why[160];
            snprintf(why, sizeof why, "should refuse %s", k_bad[i]);
            record_failure(test_name, why);
            return;
        }
    }

    char longest[MESH_HTTP_URL_MAX + 32U];
    memcpy(longest, "https://example.org/", 20U);
    memset(longest + 20, 'a', sizeof longest - 21U);
    longest[sizeof longest - 1U] = '\0';
    struct mesh_http_url url;
    MESH_TEST_FAIL_IF(mesh_http_url_parse(longest, &url), "a URL that does not fit is refused");
    record_success(test_name);
}

/*
 * The redirect the self-updater actually follows: a GitHub release download answers 302 with an
 * absolute URL on another host, carrying a long signed query.
 */
MESH_TEST_CASE(http_url_resolves_a_location, unit) {
    struct mesh_http_url base;
    MESH_TEST_FAIL_IF(
        !mesh_http_url_parse("https://github.com/o/r/releases/download/v1/a.bin?x=1", &base),
        "the base should parse");

    struct mesh_http_url out;
    MESH_TEST_FAIL_IF(
        !mesh_http_url_resolve(
            &base,
            "https://release-assets.githubusercontent.com/github-production/1?sp=r&sig=abc%2B",
            &out) ||
            strcmp(out.host, "release-assets.githubusercontent.com") != 0 ||
            strcmp(out.target, "/github-production/1?sp=r&sig=abc%2B") != 0,
        "an absolute Location replaces everything");

    MESH_TEST_FAIL_IF(!mesh_http_url_resolve(&base, "//cdn.example/x", &out) || !out.tls ||
                          strcmp(out.host, "cdn.example") != 0 || strcmp(out.target, "/x") != 0,
                      "a scheme-relative Location keeps the scheme");
    MESH_TEST_FAIL_IF(!mesh_http_url_resolve(&base, "/other", &out) ||
                          strcmp(out.host, "github.com") != 0 || strcmp(out.target, "/other") != 0,
                      "an absolute path keeps the host");
    MESH_TEST_FAIL_IF(!mesh_http_url_resolve(&base, "b.bin", &out) ||
                          strcmp(out.target, "/o/r/releases/download/v1/b.bin") != 0,
                      "a relative path replaces the last segment and drops the query");
    MESH_TEST_FAIL_IF(!mesh_http_url_resolve(&base, "?y=2", &out) ||
                          strcmp(out.target, "/o/r/releases/download/v1/a.bin?y=2") != 0,
                      "a query-only reference keeps the whole path");

    MESH_TEST_FAIL_IF(mesh_http_url_resolve(&base, "ftp://example.org/x", &out),
                      "a redirect to another scheme is refused");
    MESH_TEST_FAIL_IF(mesh_http_url_resolve(&base, "/a\r\nX-Evil: 1", &out),
                      "a Location with a line break in it is refused");
    MESH_TEST_FAIL_IF(mesh_http_url_resolve(&base, "", &out), "an empty Location is refused");
    record_success(test_name);
}

/* ------------------------------------------------------------------ the request */

MESH_TEST_CASE(http_request_is_the_1_1_shape, unit) {
    struct mesh_http_url url;
    MESH_TEST_FAIL_IF(!mesh_http_url_parse("https://api.github.com/repos/a/b", &url), "parse");
    const char *const headers[] = {"Accept: application/vnd.github+json", "User-Agent: t/1", NULL};
    char out[512];
    int len = mesh_http_request_format(out, sizeof out, MESH_HTTP_GET, &url, headers, 4U);
    static const char k_want[] = "GET /repos/a/b HTTP/1.1\r\n"
                                 "Host: api.github.com\r\n"
                                 "Connection: close\r\n"
                                 "Accept: application/vnd.github+json\r\n"
                                 "User-Agent: t/1\r\n"
                                 "\r\n";
    MESH_TEST_FAIL_IF(len != (int)strlen(k_want) || strcmp(out, k_want) != 0,
                      "a GET should be exactly the request line, Host, Connection and headers");

    MESH_TEST_FAIL_IF(!mesh_http_url_parse("http://[fd00::1]:8080/x", &url), "parse v6");
    len = mesh_http_request_format(out, sizeof out, MESH_HTTP_HEAD, &url, NULL, 0U);
    MESH_TEST_FAIL_IF(
        len < 0 || strncmp(out, "HEAD /x HTTP/1.1\r\nHost: [fd00::1]:8080\r\n", 40U) != 0,
        "a HEAD to a v6 literal on its own port brackets the host and names the port");
    record_success(test_name);
}

MESH_TEST_CASE(http_request_refuses_a_header_that_is_two, unit) {
    struct mesh_http_url url;
    MESH_TEST_FAIL_IF(!mesh_http_url_parse("https://example.org/", &url), "parse");
    const char *const smuggled[] = {"Range: bytes=0-1\r\nX-Evil: 1"};
    char out[512];
    MESH_TEST_FAIL_IF(
        mesh_http_request_format(out, sizeof out, MESH_HTTP_GET, &url, smuggled, 1U) != -1,
        "a header carrying a line break is refused");
    MESH_TEST_FAIL_IF(mesh_http_request_format(out, 20U, MESH_HTTP_GET, &url, NULL, 0U) != -1,
                      "a request that does not fit is refused, not truncated");
    record_success(test_name);
}

/* ------------------------------------------------------------------ framing */

MESH_TEST_CASE(http_response_reads_a_content_length_body, unit) {
    static struct parsed parsed;
    if (!parse(test_name, &parsed,
               "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello"
               "LEFTOVER",
               false, false)) {
        return;
    }
    MESH_TEST_FAIL_IF(parsed.response.status != 200, "the status should be read");
    MESH_TEST_FAIL_IF(parsed.response.framing != MESH_HTTP_FRAMING_LENGTH ||
                          parsed.response.content_length != 5U,
                      "Content-Length frames the body");
    MESH_TEST_FAIL_IF(!body_is(&parsed, "hello"), "the body is exactly Content-Length bytes");
    MESH_TEST_FAIL_IF(!mesh_http_response_done(&parsed.response), "and then it is done");
    MESH_TEST_FAIL_IF(parsed.unconsumed != 8U, "what follows the body is not consumed");
    record_success(test_name);
}

/* The one place a feed stops without a body slice: straight after the head, so a caller can see
   a redirect before it is handed the redirect's body. */
MESH_TEST_CASE(http_response_stops_after_the_head, unit) {
    static const char k_text[] =
        "HTTP/1.1 302 Found\r\nLocation: /x\r\nContent-Length: 3\r\n\r\nabc";
    static struct mesh_http_response response;
    mesh_http_response_init(&response, false);
    const uint8_t *body = NULL;
    size_t body_len = 0U;
    const size_t consumed = mesh_http_response_feed(&response, (const uint8_t *)k_text,
                                                    strlen(k_text), &body, &body_len);
    MESH_TEST_FAIL_IF(consumed != strlen(k_text) - 3U || body_len != 0U,
                      "the first feed should end exactly at the end of the head");
    MESH_TEST_FAIL_IF(!mesh_http_response_head_done(&response) || response.status != 302,
                      "and the head should be readable then");
    MESH_TEST_FAIL_IF(!header_is(&response, "location", "/x"), "Location should be findable");
    record_success(test_name);
}

MESH_TEST_CASE(http_response_reads_a_chunked_body, unit) {
    static struct parsed parsed;
    if (!parse(test_name, &parsed,
               "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
               "5\r\nhello\r\n"
               "7;name=value\r\n, world\r\n"
               "A \r\n0123456789\r\n"
               "0\r\nX-Trailer: yes\r\n\r\n",
               false, false)) {
        return;
    }
    MESH_TEST_FAIL_IF(parsed.response.framing != MESH_HTTP_FRAMING_CHUNKED, "chunked framing");
    MESH_TEST_FAIL_IF(!body_is(&parsed, "hello, world0123456789"),
                      "the body is the chunks' data, without sizes, extensions or CRLFs");
    MESH_TEST_FAIL_IF(!mesh_http_response_done(&parsed.response),
                      "a zero chunk and the end of the trailers finish it");
    MESH_TEST_FAIL_IF(parsed.response.body_received != 22U, "body_received counts decoded bytes");
    record_success(test_name);
}

MESH_TEST_CASE(http_response_takes_bare_line_feeds, unit) {
    static struct parsed parsed;
    if (!parse(test_name, &parsed, "HTTP/1.1 200 OK\nTransfer-Encoding: chunked\n\n3\nabc\n0\n\n",
               false, false)) {
        return;
    }
    MESH_TEST_FAIL_IF(!body_is(&parsed, "abc") || !mesh_http_response_done(&parsed.response),
                      "LF alone is a line ending a recipient may accept");
    record_success(test_name);
}

MESH_TEST_CASE(http_response_reads_until_close, unit) {
    static struct parsed parsed;
    if (!parse(test_name, &parsed, "HTTP/1.0 200 OK\r\n\r\nall of it", false, true)) {
        return;
    }
    MESH_TEST_FAIL_IF(parsed.response.framing != MESH_HTTP_FRAMING_UNTIL_CLOSE,
                      "no length and no chunking is a body that ends with the connection");
    MESH_TEST_FAIL_IF(!parsed.finished || !mesh_http_response_done(&parsed.response),
                      "for which the close is the end");
    MESH_TEST_FAIL_IF(!body_is(&parsed, "all of it"), "and everything before it is body");
    record_success(test_name);
}

MESH_TEST_CASE(http_response_has_no_body_where_none_is_sent, unit) {
    static struct parsed parsed;
    /* A HEAD's reply describes a body it does not send, and only the request says so. */
    if (!parse(test_name, &parsed, "HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\n\r\n", true,
               true)) {
        return;
    }
    MESH_TEST_FAIL_IF(parsed.response.framing != MESH_HTTP_FRAMING_NONE || !parsed.finished,
                      "a HEAD reply is done at the end of its head");
    MESH_TEST_FAIL_IF(strstr(parsed.response.head, "Content-Length: 1048576") == NULL,
                      "and its head is kept whole - it is the whole answer");

    if (!parse(test_name, &parsed, "HTTP/1.1 204 No Content\r\n\r\n", false, true)) {
        return;
    }
    MESH_TEST_FAIL_IF(parsed.response.framing != MESH_HTTP_FRAMING_NONE || !parsed.finished,
                      "a 204 has no body");
    if (!parse(test_name, &parsed, "HTTP/1.1 304 Not Modified\r\nContent-Length: 9\r\n\r\n", false,
               true)) {
        return;
    }
    MESH_TEST_FAIL_IF(parsed.response.framing != MESH_HTTP_FRAMING_NONE || !parsed.finished,
                      "nor does a 304, whatever length it names");
    record_success(test_name);
}

MESH_TEST_CASE(http_response_skips_an_interim_answer, unit) {
    static struct parsed parsed;
    if (!parse(test_name, &parsed,
               "\r\nHTTP/1.1 100 Continue\r\n\r\n"
               "HTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\n"
               "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
               false, false)) {
        return;
    }
    MESH_TEST_FAIL_IF(parsed.response.status != 200 || !body_is(&parsed, "ok"),
                      "1xx answers are passed over for the one that follows");
    MESH_TEST_FAIL_IF(strstr(parsed.response.head, "Early Hints") != NULL,
                      "and the head kept is only the final one");
    record_success(test_name);
}

MESH_TEST_CASE(http_response_transfer_encoding_beats_length, unit) {
    static struct parsed parsed;
    if (!parse(test_name, &parsed,
               "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nTransfer-Encoding: Chunked\r\n\r\n"
               "2\r\nhi\r\n0\r\n\r\n",
               false, false)) {
        return;
    }
    MESH_TEST_FAIL_IF(parsed.response.framing != MESH_HTTP_FRAMING_CHUNKED ||
                          !body_is(&parsed, "hi") || !mesh_http_response_done(&parsed.response),
                      "with both, Transfer-Encoding frames the body (RFC 9112 6.3)");
    record_success(test_name);
}

MESH_TEST_CASE(http_response_headers_are_found_without_case, unit) {
    static struct parsed parsed;
    if (!parse(test_name, &parsed,
               "HTTP/1.1 200 OK\r\nX-Empty:\r\nETag:   \"abc\"  \r\nContent-Length: 0\r\n\r\n",
               false, false)) {
        return;
    }
    MESH_TEST_FAIL_IF(!header_is(&parsed.response, "etag", "\"abc\""),
                      "a value is found by any case of its name, trimmed");
    MESH_TEST_FAIL_IF(!header_is(&parsed.response, "x-empty", ""), "an empty value is a value");
    const char *value = NULL;
    size_t len = 0U;
    MESH_TEST_FAIL_IF(mesh_http_response_header(&parsed.response, "location", &value, &len),
                      "a header that is not there is not found");
    MESH_TEST_FAIL_IF(!mesh_http_response_done(&parsed.response),
                      "a zero length is done at the end of the head");
    record_success(test_name);
}

/* ------------------------------------------------------------------ refusals */

struct refusal {
    const char *text;
    enum mesh_http_error want;
    const char *why;
};

MESH_TEST_CASE(http_response_refuses_ambiguous_framing, unit) {
    static const struct refusal k_cases[] = {
        {"HTTP/2 200\r\n\r\n", MESH_HTTP_ERR_STATUS_LINE, "HTTP/2 is not spoken here"},
        {"ICY 200 OK\r\n\r\n", MESH_HTTP_ERR_STATUS_LINE, "not an HTTP status line"},
        {"HTTP/1.1 20\r\n\r\n", MESH_HTTP_ERR_STATUS_LINE, "a two-digit status"},
        {"HTTP/1.1 200OK\r\n\r\n", MESH_HTTP_ERR_STATUS_LINE, "no space after the status"},
        {"HTTP/1.1 101 Switching Protocols\r\nUpgrade: h2c\r\n\r\n", MESH_HTTP_ERR_UPGRADE,
         "a switch that was never asked for"},
        {"HTTP/1.1 200 OK\r\nX-A: 1\r\n folded\r\n\r\n", MESH_HTTP_ERR_HEADER, "a folded line"},
        {"HTTP/1.1 200 OK\r\nContent-Length : 5\r\n\r\nhello", MESH_HTTP_ERR_HEADER,
         "whitespace before the colon"},
        {"HTTP/1.1 200 OK\r\nno colon here\r\n\r\n", MESH_HTTP_ERR_HEADER, "a line with no colon"},
        {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!",
         MESH_HTTP_ERR_LENGTH, "two lengths that disagree"},
        {"HTTP/1.1 200 OK\r\nContent-Length: 5, 5\r\n\r\nhello", MESH_HTTP_ERR_LENGTH,
         "a length that is a list"},
        {"HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", MESH_HTTP_ERR_LENGTH, "a signed length"},
        {"HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n", MESH_HTTP_ERR_LENGTH,
         "a length past 64 bits"},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", MESH_HTTP_ERR_ENCODING,
         "a coding this never asked for"},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
         MESH_HTTP_ERR_ENCODING, "chunked twice"},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", MESH_HTTP_ERR_CHUNK,
         "a size that is not hex"},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3x\r\n", MESH_HTTP_ERR_CHUNK,
         "junk after a size"},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n11111111111111111\r\n",
         MESH_HTTP_ERR_CHUNK, "a size past 64 bits"},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcX\r\n", MESH_HTTP_ERR_CHUNK,
         "a chunk longer than its size"},
    };
    for (size_t i = 0U; i < sizeof k_cases / sizeof k_cases[0]; ++i) {
        static struct parsed parsed;
        if (!parse(test_name, &parsed, k_cases[i].text, false, false)) {
            return;
        }
        if (!mesh_http_response_failed(&parsed.response) ||
            parsed.response.error != k_cases[i].want) {
            char why[200];
            snprintf(why, sizeof why, "should refuse %s as \"%s\", got \"%s\"", k_cases[i].why,
                     mesh_http_error_name(k_cases[i].want),
                     mesh_http_error_name(parsed.response.error));
            record_failure(test_name, why);
            return;
        }
    }
    record_success(test_name);
}

MESH_TEST_CASE(http_response_says_when_it_was_cut_short, unit) {
    static const char *const k_cut[] = {
        "HTTP/1.1 200 OK\r\nContent-Len",                                /* inside the head */
        "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhalf",             /* inside the body */
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel", /* inside a chunk */
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n", /* before the trailers end */
    };
    for (size_t i = 0U; i < sizeof k_cut / sizeof k_cut[0]; ++i) {
        static struct parsed parsed;
        if (!parse(test_name, &parsed, k_cut[i], false, true)) {
            return;
        }
        MESH_TEST_FAIL_IF(parsed.finished || parsed.response.error != MESH_HTTP_ERR_TRUNCATED,
                          "a close before the framing's end is a truncation, not an end");
    }
    record_success(test_name);
}

MESH_TEST_CASE(http_response_bounds_the_head, unit) {
    static char text[MESH_HTTP_HEAD_MAX + 64U];
    int at = snprintf(text, sizeof text, "HTTP/1.1 200 OK\r\nX-Big: ");
    memset(text + at, 'a', sizeof text - (size_t)at - 8U);
    memcpy(text + sizeof text - 8U, "\r\n\r\n", 5U);
    static struct parsed parsed;
    if (!parse(test_name, &parsed, text, false, false)) {
        return;
    }
    MESH_TEST_FAIL_IF(parsed.response.error != MESH_HTTP_ERR_HEAD_TOO_LARGE,
                      "a head that does not fit is refused, not truncated");
    record_success(test_name);
}
