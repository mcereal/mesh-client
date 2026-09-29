#define _POSIX_C_SOURCE 200809L

#include "mesh/core/crash_upload.h"

#include "mesh/core/version.h"

#include "inkwell/base/env.h"
#include "inkwell/base/file.h"
#include "inkwell/base/log.h"
#include "inkwell/base/text.h"
#include "inkwell/codec/sha256.h"
#include "inkwell/net/fetch.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest line a field is read from; see mesh_crash_report_parse(). */
#define CRASH_LINE_MAX 256U
/* The column inkwell pads every label to (INKWELL_CRASH_NOTE_COLUMN). */
#define CRASH_LABEL_COLUMN 13U
/* A send that has not finished in this long is not going to. */
#define CRASH_UPLOAD_TIMEOUT_MS 30000U
#define CRASH_UPLOAD_IDLE_MS 15000U
/* Sentry answers with a line of JSON naming the event; anything more is not worth keeping. */
#define CRASH_UPLOAD_RESPONSE_MAX 4096U

/* ---- reading a report ----------------------------------------------------------------------- */

/*
 * Which part of the file a line is in. Only the head and the two address sections are read; the
 * log is skipped whole, which is what keeps a log line that happens to start "version" from being
 * taken for the note of that name - and, more to the point, keeps anything the log says out of
 * everything below.
 */
enum crash_section {
    CRASH_SECTION_HEAD = 0,
    CRASH_SECTION_WHERE,
    CRASH_SECTION_LOG,
    CRASH_SECTION_STACK,
    CRASH_SECTION_END,
};

/* The value of `line` when it is `label` padded out to the column, else NULL. */
static const char *crash_field(const char *line, size_t len, const char *label) {
    const size_t label_len = strlen(label);
    if (len <= CRASH_LABEL_COLUMN || label_len >= CRASH_LABEL_COLUMN ||
        strncmp(line, label, label_len) != 0) {
        return NULL;
    }
    for (size_t i = label_len; i < CRASH_LABEL_COLUMN; ++i) {
        if (line[i] != ' ') {
            return NULL;
        }
    }
    return line + CRASH_LABEL_COLUMN;
}

/* Copies a value up to the end of its line, bounded. */
static void crash_copy(char *out, size_t out_len, const char *value, const char *end) {
    size_t len = (size_t)(end - value);
    if (len >= out_len) {
        len = out_len - 1U;
    }
    memcpy(out, value, len);
    out[len] = '\0';
}

static bool crash_hex_value(const char *text, uint64_t *out) {
    if (text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text + 2, &end, 16);
    if (errno != 0 || end == text + 2) {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static void crash_read_head(struct mesh_crash_report *out, const char *line, const char *end) {
    const size_t len = (size_t)(end - line);
    const char *value = NULL;
    char text[MESH_CRASH_FIELD_MAX];

    if ((value = crash_field(line, len, "signal")) != NULL) {
        out->signal = atoi(value);
        const char *open = memchr(value, '(', (size_t)(end - value));
        const char *close = open != NULL ? memchr(open, ')', (size_t)(end - open)) : NULL;
        if (open != NULL && close != NULL) {
            crash_copy(out->signal_name, sizeof out->signal_name, open + 1, close);
        }
    } else if ((value = crash_field(line, len, "code")) != NULL) {
        out->code = atoi(value);
        out->have_code = true;
    } else if ((value = crash_field(line, len, "fault addr")) != NULL) {
        out->have_fault_addr = crash_hex_value(value, &out->fault_addr);
    } else if ((value = crash_field(line, len, "uptime ms")) != NULL) {
        out->uptime_ms = (uint64_t)strtoull(value, NULL, 10);
    } else if ((value = crash_field(line, len, "load base")) != NULL) {
        out->have_load_base = crash_hex_value(value, &out->load_base);
    } else if ((value = crash_field(line, len, "image size")) != NULL) {
        (void)crash_hex_value(value, &out->image_size);
    } else if ((value = crash_field(line, len, "build id")) != NULL) {
        crash_copy(text, sizeof text, value, end);
        /* Hex and nothing else: this goes into JSON and a symbol server's lookup as it is. */
        size_t hex = 0U;
        while (text[hex] != '\0' &&
               ((text[hex] >= '0' && text[hex] <= '9') || (text[hex] >= 'a' && text[hex] <= 'f'))) {
            ++hex;
        }
        if (text[hex] == '\0' && hex >= 2U && hex % 2U == 0U) {
            inkwell_str_copy(out->build_id, sizeof out->build_id, text);
        }
    } else if ((value = crash_field(line, len, "version")) != NULL) {
        crash_copy(out->version, sizeof out->version, value, end);
    } else if ((value = crash_field(line, len, "route")) != NULL) {
        /* Screen and level, never the subject: see the top of the header. */
        const char *colon = memchr(value, ':', (size_t)(end - value));
        crash_copy(out->route, sizeof out->route, value, colon != NULL ? colon : end);
    } else if ((value = crash_field(line, len, "transport")) != NULL) {
        crash_copy(out->transport, sizeof out->transport, value, end);
    } else if ((value = crash_field(line, len, "backend")) != NULL) {
        crash_copy(out->backend, sizeof out->backend, value, end);
    } else if ((value = crash_field(line, len, "screen")) != NULL) {
        crash_copy(out->screen, sizeof out->screen, value, end);
    }
}

bool mesh_crash_report_parse(const char *text, size_t len, struct mesh_crash_report *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (text == NULL) {
        return false;
    }

    enum crash_section section = CRASH_SECTION_HEAD;
    bool have_signal = false;
    const char *const stop = text + len;
    for (const char *next = text; next < stop;) {
        const char *newline = memchr(next, '\n', (size_t)(stop - next));
        if (newline == NULL) {
            newline = stop;
        }
        /*
         * Each line is parsed from a terminated copy, never in place. `text` is a file read into
         * memory and is not promised a terminator, and atoi() and strtoull() read until they
         * find one - which on the last line of a file is past the end of the buffer. A line
         * longer than the copy is cut, which costs nothing: every field this reads is short, and
         * the one section with long lines is the log, which is never read.
         */
        char copy[CRASH_LINE_MAX];
        size_t line_len = (size_t)(newline - next);
        if (line_len >= sizeof copy) {
            line_len = sizeof copy - 1U;
        }
        memcpy(copy, next, line_len);
        copy[line_len] = '\0';
        const char *const line = copy;
        const char *const end = copy + line_len;
        next = newline + 1;

        if (line_len >= 4U && strncmp(line, "--- ", 4U) == 0) {
            if (strncmp(line, "--- where", 9U) == 0) {
                section = CRASH_SECTION_WHERE;
            } else if (strncmp(line, "--- log", 7U) == 0) {
                section = CRASH_SECTION_LOG;
            } else if (strncmp(line, "--- stack", 9U) == 0) {
                section = CRASH_SECTION_STACK;
            } else {
                section = CRASH_SECTION_END;
            }
        } else if (section == CRASH_SECTION_HEAD) {
            if (crash_field(line, line_len, "signal") != NULL) {
                have_signal = true;
            }
            crash_read_head(out, line, end);
        } else if (section == CRASH_SECTION_WHERE) {
            const char *value = crash_field(line, line_len, "pc");
            uint64_t pc = 0U;
            if (value != NULL && out->frame_count == 0U && crash_hex_value(value, &pc)) {
                out->frames[out->frame_count++] = pc;
            }
        } else if (section == CRASH_SECTION_STACK) {
            /* " #00 0x..." */
            const char *hash = memchr(line, '#', line_len);
            const char *space = hash != NULL ? memchr(hash, ' ', (size_t)(end - hash)) : NULL;
            uint64_t address = 0U;
            if (space != NULL && out->frame_count < MESH_CRASH_FRAMES_MAX &&
                crash_hex_value(space + 1, &address)) {
                out->frames[out->frame_count++] = address;
            }
        }
    }
    if (!have_signal) {
        memset(out, 0, sizeof *out);
        return false;
    }
    return true;
}

/* ---- the DSN -------------------------------------------------------------------------------- */

bool mesh_crash_dsn_parse(const char *dsn, struct mesh_crash_dsn *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    static const char k_scheme[] = "https://";
    if (dsn == NULL || strncmp(dsn, k_scheme, sizeof k_scheme - 1U) != 0 ||
        strlen(dsn) >= sizeof out->dsn) {
        return false;
    }
    const char *const key = dsn + sizeof k_scheme - 1U;
    const char *const at = strchr(key, '@');
    if (at == NULL || at == key) {
        return false;
    }
    /* A key with a secret after it (`key:secret@`) is the old form; the key is what is sent. */
    const char *key_end = memchr(key, ':', (size_t)(at - key));
    if (key_end == NULL) {
        key_end = at;
    }
    const char *const host = at + 1;
    const char *const slash = strrchr(host, '/');
    if (slash == NULL || slash == host || slash[1] == '\0') {
        return false;
    }
    for (const char *c = slash + 1; *c != '\0'; ++c) {
        if (*c < '0' || *c > '9') {
            return false;
        }
    }
    for (const char *c = key; c < key_end; ++c) {
        const bool ok =
            (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z');
        if (!ok) {
            return false;
        }
    }
    const size_t key_len = (size_t)(key_end - key);
    const size_t host_len = (size_t)(slash - host);
    if (key_len >= sizeof out->key || strlen(slash + 1) >= sizeof out->project) {
        return false;
    }
    memcpy(out->key, key, key_len);
    inkwell_str_copy(out->project, sizeof out->project, slash + 1);
    inkwell_str_copy(out->dsn, sizeof out->dsn, dsn);
    /* The host keeps any path before the project - a self-hosted Sentry under a prefix. */
    const int written =
        snprintf(out->envelope_url, sizeof out->envelope_url, "https://%.*s/api/%s/envelope/",
                 (int)host_len, host, out->project);
    if (written < 0 || (size_t)written >= sizeof out->envelope_url) {
        memset(out, 0, sizeof *out);
        return false;
    }
    return true;
}

/* ---- writing JSON --------------------------------------------------------------------------- */

struct crash_json {
    char *out;
    size_t cap;
    size_t len;
    bool overflow;
};

static void json_raw(struct crash_json *json, const char *text, size_t len) {
    if (json->overflow || json->len + len >= json->cap) {
        json->overflow = true;
        return;
    }
    memcpy(json->out + json->len, text, len);
    json->len += len;
    json->out[json->len] = '\0';
}

/* A literal, measured by the compiler rather than by whoever typed it. */
#define json_lit(json, text) json_raw((json), (text), sizeof(text) - 1U)

static void json_fmt(struct crash_json *json, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void json_fmt(struct crash_json *json, const char *format, ...) {
    if (json->overflow) {
        return;
    }
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(json->out + json->len, json->cap - json->len, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= json->cap - json->len) {
        json->overflow = true;
        return;
    }
    json->len += (size_t)written;
}

/* A JSON string, quoted and escaped. Every string here came out of a file, so none is trusted
   to be free of a quote or a control byte. */
static void json_str(struct crash_json *json, const char *text) {
    json_lit(json, "\"");
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; ++c) {
        if (*c == '"' || *c == '\\') {
            const char escaped[2] = {'\\', (char)*c};
            json_raw(json, escaped, 2U);
        } else if (*c < 0x20U) {
            json_fmt(json, "\\u%04x", (unsigned)*c);
        } else {
            json_raw(json, (const char *)c, 1U);
        }
    }
    json_lit(json, "\"");
}

/* `"key":"value"` with a comma before it unless it is the first; nothing for an empty value. */
static void json_tag(struct crash_json *json, bool *first, const char *key, const char *value) {
    if (value == NULL || value[0] == '\0') {
        return;
    }
    if (!*first) {
        json_lit(json, ",");
    }
    *first = false;
    json_str(json, key);
    json_lit(json, ":");
    json_str(json, value);
}

/* ---- the event ------------------------------------------------------------------------------ */

/* Wire values, not words: Sentry's names for a system and an architecture, and this binary's
   file name, which is what the uploaded debug files are matched against. */
void mesh_crash_context_init(struct mesh_crash_context *out, uint64_t now_unix) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->sender_version = mesh_version_string();
    out->environment = mesh_version_is_release() ? "production" : "development";
    out->binary = "meshclient";
#if defined(__APPLE__)
    out->os = "macos";
#elif defined(_WIN32)
    out->os = "windows";
#elif defined(__linux__)
    out->os = "linux";
#else
    out->os = "unknown";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    out->arch = "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    out->arch = "x86_64";
#elif defined(__arm__)
    out->arch = "arm";
#elif defined(__i386__) || defined(_M_IX86)
    out->arch = "x86";
#else
    out->arch = "unknown";
#endif
    out->now_unix = now_unix;
}

void mesh_crash_event_id(const char *text, size_t len, char out[33]) {
    static const char k_hex[] = "0123456789abcdef";
    uint8_t digest[INKWELL_SHA256_DIGEST_LEN];
    struct inkwell_sha256 sha;
    inkwell_sha256_init(&sha);
    inkwell_sha256_update(&sha, text, len);
    inkwell_sha256_final(&sha, digest);
    for (size_t i = 0U; i < 16U; ++i) {
        out[2U * i] = k_hex[digest[i] >> 4U];
        out[2U * i + 1U] = k_hex[digest[i] & 0x0fU];
    }
    out[32] = '\0';
}

static uint8_t crash_nibble(char c) { return (uint8_t)(c <= '9' ? c - '0' : c - 'a' + 10); }

/*
 * The debug id a symbol server files this binary under, from its build id.
 *
 * A Mach-O UUID is already one, byte for byte. An ELF build id is not a UUID at all - it is
 * usually a 20-byte SHA-1 - and the convention Sentry and Breakpad share is to take its first 16
 * bytes as a little-endian GUID: the first three fields byte-swapped, the rest as they are. Get
 * this wrong and the symbols upload fine and never match.
 */
static void crash_debug_id(const char *build_id, bool swap, char out[37]) {
    uint8_t bytes[16] = {0};
    const size_t digits = strlen(build_id);
    for (size_t i = 0U; i < 16U && 2U * i + 1U < digits; ++i) {
        bytes[i] =
            (uint8_t)(crash_nibble(build_id[2U * i]) << 4U | crash_nibble(build_id[2U * i + 1U]));
    }
    if (swap) {
        const uint8_t a[4] = {bytes[3], bytes[2], bytes[1], bytes[0]};
        memcpy(bytes, a, 4U);
        const uint8_t b = bytes[4];
        bytes[4] = bytes[5];
        bytes[5] = b;
        const uint8_t c = bytes[6];
        bytes[6] = bytes[7];
        bytes[7] = c;
    }
    (void)snprintf(out, 37U, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                   bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                   bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                   bytes[15]);
}

static void crash_write_event(struct crash_json *json, const struct mesh_crash_report *report,
                              const char *event_id, const struct mesh_crash_context *context) {
    const char *const version =
        report->version[0] != '\0' ? report->version : context->sender_version;
    const char *const signal_name = report->signal_name[0] != '\0' ? report->signal_name : "signal";

    json_fmt(json, "{\"event_id\":\"%s\",\"timestamp\":%llu,", event_id,
             (unsigned long long)context->now_unix);
    json_lit(json, "\"platform\":\"native\",\"level\":\"fatal\",\"logger\":\"crash\",");
    json_lit(json, "\"release\":");
    char release[MESH_CRASH_FIELD_MAX + 16U];
    (void)snprintf(release, sizeof release, "meshclient@%s", version != NULL ? version : "");
    json_str(json, release);
    json_lit(json, ",\"environment\":");
    json_str(json, context->environment != NULL ? context->environment : "");

    /* Tags are what the issue list filters on, so the form factor goes here. */
    json_lit(json, ",\"tags\":{");
    bool first = true;
    json_tag(json, &first, "route", report->route);
    json_tag(json, &first, "backend", report->backend);
    json_tag(json, &first, "screen", report->screen);
    json_tag(json, &first, "link", report->transport);
    json_tag(json, &first, "os", context->os);
    json_tag(json, &first, "arch", context->arch);
    json_lit(json, "}");

    json_lit(json, ",\"contexts\":{\"os\":{\"name\":");
    json_str(json, context->os != NULL ? context->os : "");
    json_lit(json, "},\"device\":{\"arch\":");
    json_str(json, context->arch != NULL ? context->arch : "");
    json_lit(json, "},\"app\":{\"app_version\":");
    json_str(json, version != NULL ? version : "");
    json_fmt(json, "}},\"extra\":{\"uptime_ms\":%llu", (unsigned long long)report->uptime_ms);
    if (report->have_fault_addr) {
        json_fmt(json, ",\"fault_addr\":\"0x%llx\"", (unsigned long long)report->fault_addr);
    }
    json_lit(json, "}");

    /* The fault. The frames go oldest first, which is the order Sentry reads them in; the
       report writes the pc first and walks outward, so they are turned round. */
    json_lit(json, ",\"exception\":{\"values\":[{\"type\":");
    json_str(json, signal_name);
    char value[96];
    if (report->have_fault_addr) {
        (void)snprintf(value, sizeof value, "Fatal signal %d at 0x%llx", report->signal,
                       (unsigned long long)report->fault_addr);
    } else {
        (void)snprintf(value, sizeof value, "Fatal signal %d", report->signal);
    }
    json_lit(json, ",\"value\":");
    json_str(json, value);
    json_fmt(json,
             ",\"mechanism\":{\"type\":\"signalhandler\",\"handled\":false,\"synthetic\":true,"
             "\"meta\":{\"signal\":{\"number\":%d,\"code\":%d,\"name\":",
             report->signal, report->have_code ? report->code : 0);
    json_str(json, signal_name);
    json_lit(json, "}}}");
    if (report->frame_count > 0U) {
        json_lit(json, ",\"stacktrace\":{\"frames\":[");
        for (size_t i = report->frame_count; i > 0U; --i) {
            json_fmt(json, "%s{\"instruction_addr\":\"0x%llx\"}",
                     i == report->frame_count ? "" : ",",
                     (unsigned long long)report->frames[i - 1U]);
        }
        json_lit(json, "]}");
    }
    json_lit(json, "}]}");

    /* The one image, when the report named it: what lets the addresses above become names. */
    if (report->build_id[0] != '\0' && report->have_load_base) {
        const bool macho = context->os != NULL && strcmp(context->os, "macos") == 0;
        char debug_id[37];
        crash_debug_id(report->build_id, !macho, debug_id);
        json_fmt(json,
                 ",\"debug_meta\":{\"images\":[{\"type\":\"%s\",\"code_id\":\"%s\","
                 "\"debug_id\":\"%s\",\"image_addr\":\"0x%llx\"",
                 macho ? "macho" : "elf", report->build_id, debug_id,
                 (unsigned long long)report->load_base);
        if (report->image_size > 0U) {
            json_fmt(json, ",\"image_size\":%llu", (unsigned long long)report->image_size);
        }
        json_lit(json, ",\"code_file\":");
        json_str(json, context->binary != NULL ? context->binary : "");
        json_lit(json, "}]}");
    }
    json_lit(json, "}");
}

int mesh_crash_envelope_build(const struct mesh_crash_report *report, const char *event_id,
                              const struct mesh_crash_dsn *dsn,
                              const struct mesh_crash_context *context, char *out, size_t cap) {
    if (report == NULL || event_id == NULL || dsn == NULL || context == NULL || out == NULL ||
        cap == 0U) {
        return -EINVAL;
    }
    out[0] = '\0';

    /* The event first, into the tail of the buffer the header will not reach, because the item
       header in front of it has to carry its length. */
    static char event[MESH_CRASH_ENVELOPE_MAX];
    struct crash_json body = {.out = event, .cap = sizeof event};
    crash_write_event(&body, report, event_id, context);
    if (body.overflow) {
        return -ENOSPC;
    }

    struct crash_json json = {.out = out, .cap = cap};
    json_fmt(&json, "{\"event_id\":\"%s\",\"dsn\":", event_id);
    json_str(&json, dsn->dsn);
    json_fmt(&json, "}\n{\"type\":\"event\",\"length\":%zu}\n", body.len);
    json_raw(&json, event, body.len);
    json_lit(&json, "\n");
    if (json.overflow) {
        out[0] = '\0';
        return -ENOSPC;
    }
    return (int)json.len;
}

/* ---- sending -------------------------------------------------------------------------------- */

const char *mesh_crash_upload_default_dsn(void) {
    const char *const env = inkwell_env_get("CRASH_DSN");
    if (env != NULL) {
        return env;
    }
#ifdef MESHCLIENT_CRASH_DSN
    return MESHCLIENT_CRASH_DSN;
#else
    return "";
#endif
}

int mesh_crash_upload_init(struct mesh_crash_upload *upload, struct inkwell_loop *loop,
                           const char *dsn, mesh_crash_upload_done_fn on_done, void *userdata) {
    if (upload == NULL) {
        return -EINVAL;
    }
    memset(upload, 0, sizeof *upload);
    upload->on_done = on_done;
    upload->userdata = userdata;
    const int ready = inkwell_fetch_init(&upload->fetch, loop);
    if (ready != 0) {
        return ready;
    }
    if (dsn != NULL && dsn[0] != '\0') {
        upload->configured = mesh_crash_dsn_parse(dsn, &upload->dsn);
        if (!upload->configured) {
            inkwell_log_warn("crash", "The crash DSN is not one this client can send to");
        }
    }
    return 0;
}

void mesh_crash_upload_shutdown(struct mesh_crash_upload *upload) {
    if (upload != NULL) {
        inkwell_fetch_shutdown(&upload->fetch);
    }
}

bool mesh_crash_upload_available(const struct mesh_crash_upload *upload) {
    return upload != NULL && upload->configured && inkwell_fetch_available(&upload->fetch);
}

bool mesh_crash_upload_busy(const struct mesh_crash_upload *upload) {
    return upload != NULL && inkwell_fetch_busy(&upload->fetch);
}

static void crash_upload_on_done(void *userdata, const struct inkwell_fetch_result *result) {
    struct mesh_crash_upload *const upload = (struct mesh_crash_upload *)userdata;
    upload->outcome = result->outcome;
    upload->status = result->status;
    upload->failure = result->failure;
    inkwell_str_copy(upload->host, sizeof upload->host, result->host);
    upload->state =
        result->outcome == INKWELL_FETCH_OK ? MESH_CRASH_UPLOAD_SENT : MESH_CRASH_UPLOAD_FAILED;
    if (upload->state == MESH_CRASH_UPLOAD_SENT) {
        inkwell_log_info("crash", "Crash report sent (HTTP %d)", result->status);
    } else {
        inkwell_log_warn("crash", "Crash report not sent: %s (HTTP %d) %s",
                         inkwell_fetch_outcome_name(result->outcome), result->status,
                         result->detail);
    }
    if (upload->on_done != NULL) {
        upload->on_done(upload->userdata, upload);
    }
}

int mesh_crash_upload_send(struct mesh_crash_upload *upload, const char *path,
                           const struct mesh_crash_context *context, uint64_t now_ms) {
    if (upload == NULL || path == NULL || context == NULL) {
        return -EINVAL;
    }
    if (!mesh_crash_upload_available(upload)) {
        return -ENOTSUP;
    }
    if (mesh_crash_upload_busy(upload)) {
        return -EBUSY;
    }

    size_t len = 0U;
    errno = 0;
    uint8_t *const text = inkwell_file_read(path, MESH_CRASH_REPORT_FILE_MAX, &len);
    if (text == NULL) {
        return errno != 0 ? -errno : -ENOENT;
    }
    struct mesh_crash_report report;
    const bool parsed = mesh_crash_report_parse((const char *)text, len, &report);
    char event_id[33];
    mesh_crash_event_id((const char *)text, len, event_id);
    free(text);
    if (!parsed) {
        return -EBADMSG;
    }

    static char envelope[MESH_CRASH_ENVELOPE_MAX];
    const int envelope_len = mesh_crash_envelope_build(&report, event_id, &upload->dsn, context,
                                                       envelope, sizeof envelope);
    if (envelope_len < 0) {
        return envelope_len;
    }

    char auth[INKWELL_FETCH_HEADER_MAX];
    (void)snprintf(auth, sizeof auth,
                   "X-Sentry-Auth: Sentry sentry_version=7, sentry_key=%s, sentry_client=%s/%s",
                   upload->dsn.key, context->binary != NULL ? context->binary : "meshclient",
                   context->sender_version != NULL ? context->sender_version : "0");
    const struct inkwell_fetch_request request = {
        .url = upload->dsn.envelope_url,
        .method = INKWELL_FETCH_POST,
        .headers = {"Content-Type: application/x-sentry-envelope", auth},
        .body = envelope,
        .body_len = (size_t)envelope_len,
        .timeout_ms = CRASH_UPLOAD_TIMEOUT_MS,
        .idle_timeout_ms = CRASH_UPLOAD_IDLE_MS,
        .response_max = CRASH_UPLOAD_RESPONSE_MAX,
        .on_done = crash_upload_on_done,
        .userdata = upload,
    };
    const int started = inkwell_fetch_start(&upload->fetch, &request, now_ms);
    if (started != 0) {
        upload->state = MESH_CRASH_UPLOAD_FAILED;
        return started;
    }
    upload->state = MESH_CRASH_UPLOAD_SENDING;
    inkwell_log_info("crash", "Sending crash report %s (%d bytes, %zu frames)", event_id,
                     envelope_len, report.frame_count);
    return 0;
}

void mesh_crash_upload_tick(struct mesh_crash_upload *upload, uint64_t now_ms) {
    if (upload != NULL && mesh_crash_upload_busy(upload)) {
        inkwell_fetch_tick(&upload->fetch, now_ms);
    }
}
