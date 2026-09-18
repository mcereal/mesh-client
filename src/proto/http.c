#include "mesh/proto/http.h"

#include <stdio.h>
#include <string.h>

/*
 * Where the parser is. The body phases are told apart by framing rather than folded into one,
 * because each ends differently and a phase that had to ask the framing on every byte would be
 * that question asked 64 KB at a time.
 */
enum http_phase {
    HTTP_PHASE_HEAD = 0,
    HTTP_PHASE_LENGTH,
    HTTP_PHASE_CHUNK_SIZE,
    HTTP_PHASE_CHUNK_DATA,
    HTTP_PHASE_CHUNK_END,    /* the CRLF after a chunk's data: expecting CR or LF */
    HTTP_PHASE_CHUNK_END_LF, /* ... having seen the CR */
    HTTP_PHASE_TRAILER,
    HTTP_PHASE_UNTIL_CLOSE,
    HTTP_PHASE_DONE,
    HTTP_PHASE_FAILED,
};

/* ------------------------------------------------------------------ small pieces */

static bool http_digit(char c) { return c >= '0' && c <= '9'; }

static char http_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static bool http_equal_nocase(const char *a, size_t a_len, const char *b) {
    const size_t b_len = strlen(b);
    if (a_len != b_len) {
        return false;
    }
    for (size_t i = 0U; i < a_len; ++i) {
        if (http_lower(a[i]) != http_lower(b[i])) {
            return false;
        }
    }
    return true;
}

/* Stops at the first mismatch, so a `text` shorter than `prefix` is never read past its NUL. */
static bool http_prefix_nocase(const char *text, const char *prefix) {
    for (; *prefix != '\0'; ++text, ++prefix) {
        if (http_lower(*text) != http_lower(*prefix)) {
            return false;
        }
    }
    return true;
}

/* RFC 9110 `tchar`: what a header name is made of. */
static bool http_token_char(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int http_hex(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = http_lower(c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static bool http_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* RFC 3986 `scheme`: a letter, then letters, digits, '+', '-' and '.'. */
static bool http_scheme_char(char c, bool first) {
    if (http_alpha(c)) {
        return true;
    }
    return !first && (http_digit(c) || c == '+' || c == '-' || c == '.');
}

/* A byte that may not appear anywhere in a URL this sends: space, controls, DEL. */
static bool http_url_byte_ok(unsigned char c) { return c > 0x20U && c != 0x7FU; }

/* ------------------------------------------------------------------ URLs */

static bool http_url_bytes_ok(const char *text, size_t len) {
    for (size_t i = 0U; i < len; ++i) {
        if (!http_url_byte_ok((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

/* The target from what follows the authority: path and query, never the fragment. */
static bool http_url_set_target(struct mesh_http_url *out, const char *rest) {
    size_t len = strcspn(rest, "#");
    if (!http_url_bytes_ok(rest, len)) {
        return false;
    }
    const bool slash = len > 0U && rest[0] == '/';
    const size_t need = len + (slash ? 0U : 1U) + 1U;
    if (need > sizeof out->target) {
        return false;
    }
    size_t at = 0U;
    if (!slash) {
        out->target[at++] = '/';
    }
    memcpy(out->target + at, rest, len);
    out->target[at + len] = '\0';
    return true;
}

bool mesh_http_url_parse(const char *url, struct mesh_http_url *out) {
    if (url == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);

    const char *rest = NULL;
    if (http_prefix_nocase(url, "https://")) {
        out->tls = true;
        out->port = 443U;
        rest = url + 8;
    } else if (http_prefix_nocase(url, "http://")) {
        out->port = 80U;
        rest = url + 7;
    } else {
        return false;
    }

    const size_t authority_len = strcspn(rest, "/?#");
    const char *authority = rest;
    if (authority_len == 0U || !http_url_bytes_ok(authority, authority_len) ||
        memchr(authority, '@', authority_len) != NULL) {
        return false;
    }

    const char *host = authority;
    size_t host_len = 0U;
    const char *port_text = NULL;
    size_t port_len = 0U;
    if (authority[0] == '[') {
        const char *close = memchr(authority, ']', authority_len);
        if (close == NULL) {
            return false;
        }
        host = authority + 1;
        host_len = (size_t)(close - host);
        const size_t after = (size_t)(close - authority) + 1U;
        if (after < authority_len) {
            if (authority[after] != ':') {
                return false;
            }
            port_text = authority + after + 1U;
            port_len = authority_len - after - 1U;
        }
        /* A v6 literal is hex, colons and maybe a dotted v4 tail - nothing that would need
           quoting on the way back out. */
        for (size_t i = 0U; i < host_len; ++i) {
            if (http_hex(host[i]) < 0 && host[i] != ':' && host[i] != '.') {
                return false;
            }
        }
    } else {
        const char *colon = memchr(authority, ':', authority_len);
        host_len = colon != NULL ? (size_t)(colon - authority) : authority_len;
        if (colon != NULL) {
            port_text = colon + 1;
            port_len = authority_len - host_len - 1U;
        }
    }
    if (host_len == 0U || host_len >= sizeof out->host) {
        return false;
    }
    memcpy(out->host, host, host_len);
    out->host[host_len] = '\0';

    if (port_text != NULL) {
        /* "host:" with nothing after is the default port, per RFC 3986. */
        if (port_len > 0U) {
            if (port_len > 5U) {
                return false;
            }
            unsigned long port = 0UL;
            for (size_t i = 0U; i < port_len; ++i) {
                if (!http_digit(port_text[i])) {
                    return false;
                }
                port = port * 10UL + (unsigned long)(port_text[i] - '0');
            }
            if (port == 0UL || port > 65535UL) {
                return false;
            }
            out->port = (uint16_t)port;
        }
    }

    return http_url_set_target(out, rest + authority_len);
}

bool mesh_http_url_resolve(const struct mesh_http_url *base, const char *location,
                           struct mesh_http_url *out) {
    if (base == NULL || location == NULL || out == NULL || location[0] == '\0') {
        return false;
    }

    /* A scheme is ALPHA *(ALPHA / DIGIT / "+" / "-" / ".") ":" - and a reference that has one is
       absolute, whichever scheme it names. mesh_http_url_parse() refuses the ones that are not
       HTTP. */
    size_t scheme = 0U;
    while (http_scheme_char(location[scheme], scheme == 0U)) {
        scheme++;
    }
    if (scheme > 0U && location[scheme] == ':') {
        return mesh_http_url_parse(location, out);
    }

    if (location[0] == '/' && location[1] == '/') {
        char absolute[MESH_HTTP_URL_MAX + 8U];
        const int written =
            snprintf(absolute, sizeof absolute, "%s:%s", base->tls ? "https" : "http", location);
        if (written < 0 || (size_t)written >= sizeof absolute) {
            return false;
        }
        return mesh_http_url_parse(absolute, out);
    }

    *out = *base;
    if (location[0] == '/') {
        return http_url_set_target(out, location);
    }

    /* Relative to the base's path: everything up to its last '/' for a path, or the whole path
       for a query. The query of the base is never kept. */
    const size_t path_len = strcspn(base->target, "?");
    size_t keep = path_len;
    if (location[0] != '?') {
        while (keep > 0U && base->target[keep - 1U] != '/') {
            keep--;
        }
    }
    char merged[MESH_HTTP_URL_MAX];
    const int written =
        snprintf(merged, sizeof merged, "%.*s%s", (int)keep, base->target, location);
    if (written < 0 || (size_t)written >= sizeof merged) {
        return false;
    }
    return http_url_set_target(out, merged);
}

/* ------------------------------------------------------------------ the request */

int mesh_http_request_format(char *out, size_t cap, enum mesh_http_method method,
                             const struct mesh_http_url *url, const char *const *headers,
                             size_t header_count) {
    if (out == NULL || cap == 0U || url == NULL || url->host[0] == '\0' || url->target[0] != '/') {
        return -1;
    }
    const bool v6 = strchr(url->host, ':') != NULL;
    const bool default_port = url->port == (url->tls ? 443U : 80U);
    char port[8] = "";
    if (!default_port) {
        (void)snprintf(port, sizeof port, ":%u", (unsigned)url->port);
    }

    int written = snprintf(out, cap,
                           "%s %s HTTP/1.1\r\n"
                           "Host: %s%s%s%s\r\n"
                           "Connection: close\r\n",
                           method == MESH_HTTP_HEAD ? "HEAD" : "GET", url->target, v6 ? "[" : "",
                           url->host, v6 ? "]" : "", port);
    if (written < 0 || (size_t)written >= cap) {
        return -1;
    }
    size_t at = (size_t)written;

    for (size_t i = 0U; headers != NULL && i < header_count && headers[i] != NULL; ++i) {
        if (strpbrk(headers[i], "\r\n") != NULL) {
            return -1;
        }
        written = snprintf(out + at, cap - at, "%s\r\n", headers[i]);
        if (written < 0 || (size_t)written >= cap - at) {
            return -1;
        }
        at += (size_t)written;
    }
    if (cap - at < 3U) {
        return -1;
    }
    memcpy(out + at, "\r\n", 3U);
    return (int)(at + 2U);
}

/* ------------------------------------------------------------------ the response */

static void http_fail(struct mesh_http_response *response, enum mesh_http_error error) {
    response->error = error;
    response->phase = HTTP_PHASE_FAILED;
}

/* The line starting at `head + at`, without its LF or a CR before it. Returns where the next
   line starts. */
static size_t http_next_line(const char *head, size_t head_len, size_t at, const char **line,
                             size_t *line_len) {
    const char *start = head + at;
    const char *lf = memchr(start, '\n', head_len - at);
    size_t len = lf != NULL ? (size_t)(lf - start) : head_len - at;
    const size_t next = at + len + (lf != NULL ? 1U : 0U);
    if (len > 0U && start[len - 1U] == '\r') {
        len--;
    }
    *line = start;
    *line_len = len;
    return next;
}

static void http_trim(const char **value, size_t *len) {
    while (*len > 0U && ((*value)[0] == ' ' || (*value)[0] == '\t')) {
        (*value)++;
        (*len)--;
    }
    while (*len > 0U && ((*value)[*len - 1U] == ' ' || (*value)[*len - 1U] == '\t')) {
        (*len)--;
    }
}

/* `name: value`, checked and split. False for a line that is not one. */
static bool http_split_header(const char *line, size_t len, const char **name, size_t *name_len,
                              const char **value, size_t *value_len) {
    size_t colon = 0U;
    while (colon < len && http_token_char(line[colon])) {
        colon++;
    }
    /* No name, or something other than a colon straight after it - including whitespace, which
       RFC 9112 says to refuse because two readers split such a line differently. */
    if (colon == 0U || colon >= len || line[colon] != ':') {
        return false;
    }
    *name = line;
    *name_len = colon;
    *value = line + colon + 1U;
    *value_len = len - colon - 1U;
    for (size_t i = 0U; i < *value_len; ++i) {
        const unsigned char c = (unsigned char)(*value)[i];
        if (c == '\r' || c == '\0') {
            return false;
        }
    }
    http_trim(value, value_len);
    return true;
}

/*
 * The head is complete in `head`: read it. Returns false having failed the response, or true
 * with either a final response set up or - for an interim 1xx - the buffer emptied for the one
 * that follows it.
 */
static bool http_parse_head(struct mesh_http_response *response) {
    const char *line = NULL;
    size_t len = 0U;
    size_t at = http_next_line(response->head, response->head_len, 0U, &line, &len);

    /* HTTP/1.x SP 3DIGIT [SP reason] */
    if (len < 12U || memcmp(line, "HTTP/1.", 7U) != 0 || !http_digit(line[7]) || line[8] != ' ' ||
        !http_digit(line[9]) || !http_digit(line[10]) || !http_digit(line[11]) ||
        (len > 12U && line[12] != ' ')) {
        http_fail(response, MESH_HTTP_ERR_STATUS_LINE);
        return false;
    }
    const int status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    if (status < 100) {
        http_fail(response, MESH_HTTP_ERR_STATUS_LINE);
        return false;
    }
    if (status == 101) {
        http_fail(response, MESH_HTTP_ERR_UPGRADE);
        return false;
    }

    bool have_length = false;
    uint64_t length = 0U;
    bool chunked = false;
    while (at < response->head_len) {
        at = http_next_line(response->head, response->head_len, at, &line, &len);
        if (len == 0U) {
            break; /* the empty line that ended the head */
        }
        const char *name = NULL;
        const char *value = NULL;
        size_t name_len = 0U;
        size_t value_len = 0U;
        if (!http_split_header(line, len, &name, &name_len, &value, &value_len)) {
            /* A line starting with whitespace lands here too: obsolete line folding, which a
               recipient may refuse and this one does. */
            http_fail(response, MESH_HTTP_ERR_HEADER);
            return false;
        }

        if (http_equal_nocase(name, name_len, "content-length")) {
            if (value_len == 0U) {
                http_fail(response, MESH_HTTP_ERR_LENGTH);
                return false;
            }
            uint64_t parsed = 0U;
            for (size_t i = 0U; i < value_len; ++i) {
                if (!http_digit(value[i]) || parsed > (UINT64_MAX - 9U) / 10U) {
                    http_fail(response, MESH_HTTP_ERR_LENGTH);
                    return false;
                }
                parsed = parsed * 10U + (uint64_t)(value[i] - '0');
            }
            /* The same length twice is a proxy being redundant; two different ones is a reply
               whose end depends on which one a reader believes. */
            if (have_length && parsed != length) {
                http_fail(response, MESH_HTTP_ERR_LENGTH);
                return false;
            }
            have_length = true;
            length = parsed;
        } else if (http_equal_nocase(name, name_len, "transfer-encoding")) {
            /* Nothing but a single `chunked`. A compression coding would be a body this client
               did not ask for, and a coding listed after `chunked` leaves no way to find the end
               but the close. */
            if (chunked || !http_equal_nocase(value, value_len, "chunked")) {
                http_fail(response, MESH_HTTP_ERR_ENCODING);
                return false;
            }
            chunked = true;
        }
    }

    /* An interim answer - 100 Continue, 103 Early Hints. The real one follows; start over. */
    if (status < 200) {
        response->head_len = 0U;
        response->head[0] = '\0';
        return true;
    }

    response->status = status;
    if (response->head_request || status == 204 || status == 304) {
        response->framing = MESH_HTTP_FRAMING_NONE;
        response->phase = HTTP_PHASE_DONE;
    } else if (chunked) {
        /* Transfer-Encoding wins over a Content-Length beside it (RFC 9112 6.3). */
        response->framing = MESH_HTTP_FRAMING_CHUNKED;
        response->phase = HTTP_PHASE_CHUNK_SIZE;
        response->chunk_line_len = 0U;
    } else if (have_length) {
        response->framing = MESH_HTTP_FRAMING_LENGTH;
        response->content_length = length;
        response->remaining = length;
        response->phase = length == 0U ? HTTP_PHASE_DONE : HTTP_PHASE_LENGTH;
    } else {
        response->framing = MESH_HTTP_FRAMING_UNTIL_CLOSE;
        response->phase = HTTP_PHASE_UNTIL_CLOSE;
    }
    return true;
}

/* One byte of head. Returns true when the head has just completed (or failed). */
static bool http_head_byte(struct mesh_http_response *response, char c) {
    if (response->head_len + 1U >= sizeof response->head) {
        http_fail(response, MESH_HTTP_ERR_HEAD_TOO_LARGE);
        return true;
    }
    response->head[response->head_len++] = c;
    response->head[response->head_len] = '\0';
    if (c != '\n') {
        response->line_len++;
        return false;
    }
    const bool blank =
        response->line_len == 0U ||
        (response->line_len == 1U && response->head[response->head_len - 2U] == '\r');
    response->line_len = 0U;
    if (!blank) {
        return false;
    }
    /* An empty line before any status line is noise, not an empty head. */
    if (response->head_len <= 2U) {
        response->head_len = 0U;
        response->head[0] = '\0';
        return false;
    }
    if (!http_parse_head(response)) {
        return true;
    }
    return response->status != 0;
}

/* One byte of a chunk-size line. Returns false having failed the response. */
static bool http_chunk_size_byte(struct mesh_http_response *response, char c) {
    if (c != '\n') {
        if (response->chunk_line_len + 1U >= sizeof response->chunk_line) {
            http_fail(response, MESH_HTTP_ERR_CHUNK);
            return false;
        }
        response->chunk_line[response->chunk_line_len++] = c;
        return true;
    }
    size_t len = response->chunk_line_len;
    response->chunk_line_len = 0U;
    if (len > 0U && response->chunk_line[len - 1U] == '\r') {
        len--;
    }

    uint64_t size = 0U;
    size_t at = 0U;
    while (at < len && http_hex(response->chunk_line[at]) >= 0) {
        if (size > (UINT64_MAX >> 4U)) {
            http_fail(response, MESH_HTTP_ERR_CHUNK);
            return false;
        }
        size = (size << 4U) | (uint64_t)http_hex(response->chunk_line[at]);
        at++;
    }
    if (at == 0U) {
        http_fail(response, MESH_HTTP_ERR_CHUNK);
        return false;
    }
    /* What may follow the size: whitespace, then extensions after a ';', which are ignored. */
    while (at < len && (response->chunk_line[at] == ' ' || response->chunk_line[at] == '\t')) {
        at++;
    }
    if (at < len && response->chunk_line[at] != ';') {
        http_fail(response, MESH_HTTP_ERR_CHUNK);
        return false;
    }
    for (size_t i = at; i < len; ++i) {
        if (response->chunk_line[i] == '\r' || response->chunk_line[i] == '\0') {
            http_fail(response, MESH_HTTP_ERR_CHUNK);
            return false;
        }
    }

    if (size == 0U) {
        response->phase = HTTP_PHASE_TRAILER;
        response->line_len = 0U;
        response->trailer_len = 0U;
    } else {
        response->phase = HTTP_PHASE_CHUNK_DATA;
        response->remaining = size;
    }
    return true;
}

void mesh_http_response_init(struct mesh_http_response *response, bool head_request) {
    if (response == NULL) {
        return;
    }
    memset(response, 0, sizeof *response);
    response->phase = HTTP_PHASE_HEAD;
    response->head_request = head_request;
}

size_t mesh_http_response_feed(struct mesh_http_response *response, const uint8_t *in, size_t len,
                               const uint8_t **body, size_t *body_len) {
    if (body != NULL) {
        *body = NULL;
    }
    if (body_len != NULL) {
        *body_len = 0U;
    }
    if (response == NULL || in == NULL || body == NULL || body_len == NULL) {
        return 0U;
    }

    size_t at = 0U;
    while (at < len) {
        const char c = (char)in[at];
        switch ((enum http_phase)response->phase) {
        case HTTP_PHASE_HEAD:
            at++;
            if (http_head_byte(response, c)) {
                return at;
            }
            break;

        case HTTP_PHASE_LENGTH:
        case HTTP_PHASE_CHUNK_DATA: {
            const size_t avail = len - at;
            const size_t take =
                response->remaining < (uint64_t)avail ? (size_t)response->remaining : avail;
            *body = in + at;
            *body_len = take;
            response->remaining -= take;
            response->body_received += take;
            if (response->remaining == 0U) {
                response->phase =
                    response->phase == HTTP_PHASE_LENGTH ? HTTP_PHASE_DONE : HTTP_PHASE_CHUNK_END;
            }
            return at + take;
        }

        case HTTP_PHASE_UNTIL_CLOSE:
            *body = in + at;
            *body_len = len - at;
            response->body_received += len - at;
            return len;

        case HTTP_PHASE_CHUNK_SIZE:
            at++;
            if (!http_chunk_size_byte(response, c)) {
                return at;
            }
            break;

        case HTTP_PHASE_CHUNK_END:
            at++;
            if (c == '\r') {
                response->phase = HTTP_PHASE_CHUNK_END_LF;
            } else if (c == '\n') {
                response->phase = HTTP_PHASE_CHUNK_SIZE;
            } else {
                http_fail(response, MESH_HTTP_ERR_CHUNK);
                return at;
            }
            break;

        case HTTP_PHASE_CHUNK_END_LF:
            at++;
            if (c != '\n') {
                http_fail(response, MESH_HTTP_ERR_CHUNK);
                return at;
            }
            response->phase = HTTP_PHASE_CHUNK_SIZE;
            break;

        case HTTP_PHASE_TRAILER: {
            /* Trailer fields are read past, not kept: nothing here needs one, and a field that
               arrives after the body cannot change how the body was framed. */
            at++;
            if (++response->trailer_len >= MESH_HTTP_HEAD_MAX) {
                http_fail(response, MESH_HTTP_ERR_HEAD_TOO_LARGE);
                return at;
            }
            if (c != '\n') {
                response->line_len++;
                response->line_cr = c == '\r';
                break;
            }
            const bool blank =
                response->line_len == 0U || (response->line_len == 1U && response->line_cr);
            response->line_len = 0U;
            if (blank) {
                response->phase = HTTP_PHASE_DONE;
                return at;
            }
            break;
        }

        case HTTP_PHASE_DONE:
        case HTTP_PHASE_FAILED:
        default:
            return at;
        }
    }
    return at;
}

bool mesh_http_response_finish(struct mesh_http_response *response) {
    if (response == NULL) {
        return false;
    }
    if (response->phase == HTTP_PHASE_DONE) {
        return true;
    }
    if (response->phase == HTTP_PHASE_UNTIL_CLOSE) {
        response->phase = HTTP_PHASE_DONE;
        return true;
    }
    if (response->phase != HTTP_PHASE_FAILED) {
        http_fail(response, MESH_HTTP_ERR_TRUNCATED);
    }
    return false;
}

bool mesh_http_response_head_done(const struct mesh_http_response *response) {
    return response != NULL && response->status != 0;
}

bool mesh_http_response_done(const struct mesh_http_response *response) {
    return response != NULL && response->phase == HTTP_PHASE_DONE;
}

bool mesh_http_response_failed(const struct mesh_http_response *response) {
    return response != NULL && response->phase == HTTP_PHASE_FAILED;
}

bool mesh_http_response_header(const struct mesh_http_response *response, const char *name,
                               const char **value, size_t *value_len) {
    if (!mesh_http_response_head_done(response) || name == NULL || value == NULL ||
        value_len == NULL) {
        return false;
    }
    const char *line = NULL;
    size_t len = 0U;
    size_t at = http_next_line(response->head, response->head_len, 0U, &line, &len);
    while (at < response->head_len) {
        at = http_next_line(response->head, response->head_len, at, &line, &len);
        if (len == 0U) {
            break;
        }
        const char *field = NULL;
        const char *text = NULL;
        size_t field_len = 0U;
        size_t text_len = 0U;
        if (http_split_header(line, len, &field, &field_len, &text, &text_len) &&
            http_equal_nocase(field, field_len, name)) {
            *value = text;
            *value_len = text_len;
            return true;
        }
    }
    return false;
}

const char *mesh_http_error_name(enum mesh_http_error error) {
    static const char *const k_names[MESH_HTTP_ERROR_COUNT] = {
        [MESH_HTTP_OK] = "ok",
        [MESH_HTTP_ERR_STATUS_LINE] = "status line",
        [MESH_HTTP_ERR_HEADER] = "header",
        [MESH_HTTP_ERR_HEAD_TOO_LARGE] = "head too large",
        [MESH_HTTP_ERR_LENGTH] = "content length",
        [MESH_HTTP_ERR_ENCODING] = "transfer encoding",
        [MESH_HTTP_ERR_CHUNK] = "chunk",
        [MESH_HTTP_ERR_UPGRADE] = "upgrade",
        [MESH_HTTP_ERR_TRUNCATED] = "truncated",
    };
    if ((unsigned)error >= (unsigned)MESH_HTTP_ERROR_COUNT || k_names[error] == NULL) {
        return "unknown";
    }
    return k_names[error];
}
