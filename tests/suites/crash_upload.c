#define _POSIX_C_SOURCE 200809L

/*
 * Sending a crash report, and - the half that matters more - what does not go with it.
 *
 * The parser is an allowlist, and the cases that hold it are the ones built around a report
 * whose log section says things: a line that looks like a note, a node's name, a message. None
 * of it may reach the envelope, and a route's subject is cut off before it does. Then the
 * envelope itself is taken apart again with inkwell's JSON reader, because a hand-written JSON
 * writer that is wrong is wrong silently, on a server, where nobody here is looking.
 */

#include "framework/mesh_test.h"

#include "inkwell/codec/json.h"
#include "inkwell/runtime/crash.h"
#include "inkwell/runtime/loop.h"
#include "mesh/core/crash_upload.h"
#include "mesh/utils/crash.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A report as inkwell writes one, with the log saying everything the envelope must not. */
static const char k_report[] =
    "MeshClient crash report\n"
    "=======================\n"
    "\n"
    "Issues: https://github.com/mcereal/mesh-client/issues\n"
    "\n"
    "signal       11 (SIGSEGV)\n"
    "code         1\n"
    "fault addr   0x0000000000000010\n"
    "uptime ms    123456\n"
    "wall clock   42 (seconds since 1970; a device with no RTC reads small here)\n"
    "load base    0x0000aaaab0000000\n"
    "build id     5ef9bb6fa38b470771e73e5a14f5709b60edc5e1\n"
    "image size   0x1b150\n"
    "version      1.2.3\n"
    "route        nodes/detail:0a1b2c3d\n"
    "transport    Connected\n"
    "backend      fb\n"
    "screen       1024x768@2\n"
    "\n--- where ---------------------------------------------------------------\n"
    "pc           0x0000aaaab0001234\n"
    "Resolve an address with:  addr2line -fpe meshclient <address - load base>\n"
    "\n--- log -----------------------------------------------------------------\n"
    "version      9.9.9-LEAKED\n"
    "route        secret/place\n"
    "2026-01-01T00:00:00Z [INFO] (session): Sent \"meet at the SECRET-SPOT\" to Alice\n"
    " #07 0x00000000deadbeef\n"
    "\n--- stack ---------------------------------------------------------------\n"
    " #00 0x0000aaaab0002000\n"
    " #01 0x0000aaaab0003000\n"
    "\n--- end -----------------------------------------------------------------\n";

static const struct mesh_crash_context k_context = {
    .sender_version = "1.2.4",
    .environment = "production",
    .binary = "meshclient",
    .os = "linux",
    .arch = "aarch64",
    .now_unix = 1800000000U,
};

static const char k_dsn[] = "https://abc123@example.invalid/42";

MESH_TEST_CASE(crash_upload_reads_only_what_it_was_told_to, unit) {
    struct mesh_crash_report report;
    MESH_TEST_FAIL_IF(!mesh_crash_report_parse(k_report, sizeof k_report - 1U, &report),
                      "a report inkwell wrote should parse");
    MESH_TEST_FAIL_IF(report.signal != 11 || strcmp(report.signal_name, "SIGSEGV") != 0,
                      "the signal, by number and by name");
    MESH_TEST_FAIL_IF(!report.have_code || report.code != 1, "the signal code");
    MESH_TEST_FAIL_IF(!report.have_fault_addr || report.fault_addr != 0x10U, "the fault address");
    MESH_TEST_FAIL_IF(report.uptime_ms != 123456U, "the uptime");
    MESH_TEST_FAIL_IF(!report.have_load_base || report.load_base != 0xaaaab0000000ULL,
                      "the load base");
    MESH_TEST_FAIL_IF(report.image_size != 0x1b150U, "the image size");
    MESH_TEST_FAIL_IF(strcmp(report.build_id, "5ef9bb6fa38b470771e73e5a14f5709b60edc5e1") != 0,
                      "the build id");
    MESH_TEST_FAIL_IF(strcmp(report.backend, "fb") != 0 ||
                          strcmp(report.screen, "1024x768@2") != 0 ||
                          strcmp(report.transport, "Connected") != 0,
                      "the form factor and the link");

    /* The log's look-alike lines are not the notes: the head's are. */
    MESH_TEST_FAIL_IF(strcmp(report.version, "1.2.3") != 0,
                      "the version must come from the head, never from a log line");
    /* A node number is somebody's radio. */
    MESH_TEST_FAIL_IF(strcmp(report.route, "nodes/detail") != 0,
                      "the route must be cut to its screen and level");

    /* The pc, then the stack - and not the frame-shaped line in the log. */
    MESH_TEST_FAIL_IF(report.frame_count != 3U || report.frames[0] != 0xaaaab0001234ULL ||
                          report.frames[1] != 0xaaaab0002000ULL ||
                          report.frames[2] != 0xaaaab0003000ULL,
                      "the frames are the pc and the stack section, innermost first");

    MESH_TEST_FAIL_IF(mesh_crash_report_parse("not a report\n", 13U, &report),
                      "text with no signal line is not a report");
    MESH_TEST_FAIL_IF(report.signal != 0 || report.version[0] != '\0',
                      "and a refused parse leaves nothing behind");
    record_success(test_name);
}

/*
 * The same parser over a report inkwell really wrote, through this client's own install. The
 * fixture above is a copy of the format; this is the format, so a change to inkwell's labels or
 * sections fails here instead of sending events with nothing in them.
 */
MESH_TEST_CASE(crash_upload_reads_the_report_inkwell_writes, unit) {
    char dir[] = "/tmp/mesh_crash_formatXXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "mkdtemp");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_crash_install(dir) != 0, (void)rmdir(dir), "install");
    inkwell_crash_note(MESH_CRASH_NOTE_VERSION, "7.7.7-format");
    inkwell_crash_note(MESH_CRASH_NOTE_ROUTE, "map/list:0000beef");
    inkwell_crash_note(MESH_CRASH_NOTE_BACKEND, "sdl");
    inkwell_crash_note(MESH_CRASH_NOTE_SCREEN, "1280x800@3");

    char path[256];
    snprintf(path, sizeof path, "%s/report.txt", dir);
    FILE *file = fopen(path, "we");
    MESH_TEST_FAIL_IF_CLEANUP(file == NULL, (void)rmdir(dir), "open");
    inkwell_crash_write_report(fileno(file), 6);
    (void)fclose(file);
    static char body[16384];
    FILE *back = fopen(path, "re");
    const size_t len = back != NULL ? fread(body, 1U, sizeof body - 1U, back) : 0U;
    body[len] = '\0';
    if (back != NULL) {
        (void)fclose(back);
    }
    (void)unlink(path);
    (void)rmdir(dir);

    struct mesh_crash_report report;
    MESH_TEST_FAIL_IF(!mesh_crash_report_parse(body, len, &report), "inkwell's report parses");
    MESH_TEST_FAIL_IF(report.signal != 6 || strcmp(report.signal_name, "SIGABRT") != 0,
                      "the signal line");
    MESH_TEST_FAIL_IF(
        strcmp(report.version, "7.7.7-format") != 0 || strcmp(report.route, "map/list") != 0 ||
            strcmp(report.backend, "sdl") != 0 || strcmp(report.screen, "1280x800@3") != 0,
        "the notes, under the labels this client gave them");
#if defined(__linux__) || defined(__APPLE__)
    MESH_TEST_FAIL_IF(!report.have_load_base, "the load base");
#endif
#if defined(__linux__)
    /* CMakeLists.txt links every Linux binary here with a build id. */
    MESH_TEST_FAIL_IF(strlen(report.build_id) != 40U || report.image_size == 0U,
                      "the build id and image size, which symbolication needs");
#endif
    record_success(test_name);
}

/*
 * A file read into memory has no terminator, and a report whose last line is a field ends on a
 * digit rather than a newline. Parsed in place, atoi() on that line reads past the buffer - which
 * only a sanitizer that checks every byte notices. This hands the parser exactly the bytes.
 */
MESH_TEST_CASE(crash_upload_reads_no_further_than_it_was_given, unit) {
    static const char k_head[] = "signal       11 (SIGSEGV)\nuptime ms    4242";
    char *const exact = malloc(sizeof k_head - 1U);
    MESH_TEST_FAIL_IF(exact == NULL, "malloc");
    memcpy(exact, k_head, sizeof k_head - 1U);
    struct mesh_crash_report report;
    const bool parsed = mesh_crash_report_parse(exact, sizeof k_head - 1U, &report);
    free(exact);
    MESH_TEST_FAIL_IF(!parsed || report.signal != 11 || report.uptime_ms != 4242U,
                      "an unterminated last line should still read, and read no further");
    record_success(test_name);
}

MESH_TEST_CASE(crash_upload_takes_a_dsn_apart, unit) {
    struct mesh_crash_dsn dsn;
    MESH_TEST_FAIL_IF(!mesh_crash_dsn_parse("https://abc123@o1.ingest.sentry.io/4507", &dsn),
                      "an ordinary DSN");
    MESH_TEST_FAIL_IF(
        strcmp(dsn.key, "abc123") != 0 || strcmp(dsn.project, "4507") != 0 ||
            strcmp(dsn.envelope_url, "https://o1.ingest.sentry.io/api/4507/envelope/") != 0,
        "its key, project and envelope URL");
    MESH_TEST_FAIL_IF(
        !mesh_crash_dsn_parse("https://key:secret@sentry.example:9000/prefix/7", &dsn) ||
            strcmp(dsn.key, "key") != 0 ||
            strcmp(dsn.envelope_url, "https://sentry.example:9000/prefix/api/7/envelope/") != 0,
        "the old key:secret form, a port and a path prefix");

    static const char *const k_refused[] = {
        "",
        "http://abc@example.invalid/1",
        "https://example.invalid/1",
        "https://@example.invalid/1",
        "https://abc@example.invalid/",
        "https://abc@example.invalid/notanumber",
        "https://a\"b@example.invalid/1",
    };
    for (size_t i = 0U; i < sizeof k_refused / sizeof k_refused[0]; ++i) {
        MESH_TEST_FAIL_IF(mesh_crash_dsn_parse(k_refused[i], &dsn), k_refused[i]);
    }
    MESH_TEST_FAIL_IF(dsn.key[0] != '\0', "a refused DSN leaves nothing behind");
    record_success(test_name);
}

/* The event's value for `key`, read with inkwell's JSON reader. */
static bool crash_json_string(const char *event, const char *key, char *out, size_t out_len) {
    struct inkwell_json json;
    inkwell_json_init(&json, event, strlen(event));
    return inkwell_json_object_find(&json, key) && inkwell_json_read_string(&json, out, out_len);
}

MESH_TEST_CASE(crash_upload_writes_an_envelope_the_server_can_read, unit) {
    struct mesh_crash_report report;
    struct mesh_crash_dsn dsn;
    MESH_TEST_FAIL_IF(!mesh_crash_report_parse(k_report, sizeof k_report - 1U, &report) ||
                          !mesh_crash_dsn_parse(k_dsn, &dsn),
                      "setup");
    char event_id[33];
    mesh_crash_event_id(k_report, sizeof k_report - 1U, event_id);

    static char envelope[MESH_CRASH_ENVELOPE_MAX];
    const int len =
        mesh_crash_envelope_build(&report, event_id, &dsn, &k_context, envelope, sizeof envelope);
    MESH_TEST_FAIL_IF(len <= 0 || (size_t)len != strlen(envelope), "the envelope should build");

    /* Three lines: who is sending, what follows and how long it is, and the event. */
    char *const first = strchr(envelope, '\n');
    MESH_TEST_FAIL_IF(first == NULL, "no envelope header line");
    char *const second = strchr(first + 1, '\n');
    MESH_TEST_FAIL_IF(second == NULL, "no item header line");
    char *const event = second + 1;
    char *const third = strchr(event, '\n');
    MESH_TEST_FAIL_IF(third == NULL || third[1] != '\0', "the event is the last line");
    *first = '\0';
    *second = '\0';
    *third = '\0';

    char text[128];
    MESH_TEST_FAIL_IF(!crash_json_string(envelope, "dsn", text, sizeof text) ||
                          strcmp(text, k_dsn) != 0,
                      "the envelope header names the DSN");
    struct inkwell_json item;
    uint64_t length = 0U;
    inkwell_json_init(&item, first + 1, strlen(first + 1));
    MESH_TEST_FAIL_IF(!inkwell_json_object_find(&item, "length") ||
                          !inkwell_json_read_u64(&item, &length) || length != strlen(event),
                      "the item header's length is the event's");

    /* The whole event walks as JSON, key by key, to the end. */
    struct inkwell_json walk;
    inkwell_json_init(&walk, event, strlen(event));
    MESH_TEST_FAIL_IF(!inkwell_json_enter_object(&walk), "the event is an object");
    char key[64];
    unsigned keys = 0U;
    while (inkwell_json_next_key(&walk, key, sizeof key)) {
        MESH_TEST_FAIL_IF(!inkwell_json_skip_value(&walk), key);
        ++keys;
    }
    MESH_TEST_FAIL_IF(keys < 10U, "the event should have walked to the end");

    MESH_TEST_FAIL_IF(!crash_json_string(event, "event_id", text, sizeof text) ||
                          strcmp(text, event_id) != 0,
                      "the event carries the id");
    MESH_TEST_FAIL_IF(!crash_json_string(event, "release", text, sizeof text) ||
                          strcmp(text, "meshclient@1.2.3") != 0,
                      "the release is the build that crashed, not the one sending");

    /* Nothing from the log section, and no node number. */
    MESH_TEST_FAIL_IF(strstr(event, "SECRET") != NULL || strstr(event, "Alice") != NULL ||
                          strstr(event, "LEAKED") != NULL || strstr(event, "secret") != NULL ||
                          strstr(event, "deadbeef") != NULL,
                      "a line of the log reached the envelope");
    MESH_TEST_FAIL_IF(strstr(event, "0a1b2c3d") != NULL, "a node number reached the envelope");

    /* The form factor is filterable. */
    MESH_TEST_FAIL_IF(strstr(event, "\"backend\":\"fb\"") == NULL ||
                          strstr(event, "\"screen\":\"1024x768@2\"") == NULL ||
                          strstr(event, "\"route\":\"nodes/detail\"") == NULL,
                      "the form factor should be tags");

    /* Oldest frame first, the fault last. */
    const char *const outer = strstr(event, "0xaaaab0003000");
    const char *const pc = strstr(event, "0xaaaab0001234");
    MESH_TEST_FAIL_IF(outer == NULL || pc == NULL || outer > pc,
                      "frames go outermost first, the pc last");

    /*
     * The image, filed under the id a symbol server files an ELF under: the build id's first
     * 16 bytes as a little-endian GUID. 5ef9bb6f a38b 4707 71e7 ... swaps its first three
     * fields; the rest stay as they are.
     */
    MESH_TEST_FAIL_IF(strstr(event, "\"debug_id\":\"6fbbf95e-8ba3-0747-71e7-3e5a14f5709b\"") ==
                          NULL,
                      "the ELF debug id is the build id as a little-endian GUID");
    MESH_TEST_FAIL_IF(strstr(event, "\"code_id\":\"5ef9bb6fa38b470771e73e5a14f5709b60edc5e1\"") ==
                          NULL,
                      "and the code id is the build id as it is");
    MESH_TEST_FAIL_IF(strstr(event, "\"image_addr\":\"0xaaaab0000000\"") == NULL ||
                          strstr(event, "\"image_size\":110928") == NULL,
                      "the image is placed and sized");

    /* A Mach-O UUID is already a debug id, byte for byte. */
    struct mesh_crash_context mac = k_context;
    mac.os = "macos";
    MESH_TEST_FAIL_IF(
        mesh_crash_envelope_build(&report, event_id, &dsn, &mac, envelope, sizeof envelope) <= 0 ||
            strstr(envelope, "\"debug_id\":\"5ef9bb6f-a38b-4707-71e7-3e5a14f5709b\"") == NULL ||
            strstr(envelope, "\"type\":\"macho\"") == NULL,
        "a Mach-O image keeps its UUID's order");

    MESH_TEST_FAIL_IF(
        mesh_crash_envelope_build(&report, event_id, &dsn, &k_context, envelope, 64U) != -ENOSPC ||
            envelope[0] != '\0',
        "an envelope that does not fit is refused, not cut");
    record_success(test_name);
}

MESH_TEST_CASE(crash_upload_escapes_what_it_did_not_write, unit) {
    struct mesh_crash_report report;
    struct mesh_crash_dsn dsn;
    MESH_TEST_FAIL_IF(!mesh_crash_report_parse(k_report, sizeof k_report - 1U, &report) ||
                          !mesh_crash_dsn_parse(k_dsn, &dsn),
                      "setup");
    /* A transport state is a translated word; one with a quote and a control byte in it must
       still leave a well-formed event. */
    snprintf(report.transport, sizeof report.transport, "Con\"ne\\cted\x01");
    report.build_id[0] = '\0';
    static char envelope[MESH_CRASH_ENVELOPE_MAX];
    MESH_TEST_FAIL_IF(mesh_crash_envelope_build(&report, "0123456789abcdef0123456789abcdef", &dsn,
                                                &k_context, envelope, sizeof envelope) <= 0,
                      "build");
    const char *const event = strchr(strchr(envelope, '\n') + 1, '\n') + 1;
    char link[64];
    struct inkwell_json json;
    inkwell_json_init(&json, event, strlen(event));
    MESH_TEST_FAIL_IF(!inkwell_json_object_find(&json, "tags") ||
                          !inkwell_json_object_find(&json, "link") ||
                          !inkwell_json_read_string(&json, link, sizeof link),
                      "the escaped tag should read back");
    MESH_TEST_FAIL_IF(strncmp(link, "Con\"ne\\cted", 11U) != 0,
                      "and read back as what was written");
    MESH_TEST_FAIL_IF(strstr(event, "debug_meta") != NULL,
                      "a report without a build id names no image");
    record_success(test_name);
}

MESH_TEST_CASE(crash_upload_names_one_report_the_same_way_twice, unit) {
    char first[33];
    char again[33];
    char other[33];
    mesh_crash_event_id(k_report, sizeof k_report - 1U, first);
    mesh_crash_event_id(k_report, sizeof k_report - 1U, again);
    mesh_crash_event_id(k_report, sizeof k_report - 2U, other);
    MESH_TEST_FAIL_IF(strlen(first) != 32U || strcmp(first, again) != 0,
                      "the same report is the same event, so a resend is kept once");
    MESH_TEST_FAIL_IF(strcmp(first, other) == 0, "a different report is a different event");
    record_success(test_name);
}

MESH_TEST_CASE(crash_upload_without_a_dsn_offers_nothing, unit) {
    struct inkwell_loop loop;
    MESH_TEST_FAIL_IF(inkwell_loop_init(&loop) != 0, "loop");
    struct mesh_crash_upload upload;
    MESH_TEST_FAIL_IF_CLEANUP(mesh_crash_upload_init(&upload, &loop, "", NULL, NULL) != 0,
                              inkwell_loop_shutdown(&loop), "init with no DSN still succeeds");
    const bool available = mesh_crash_upload_available(&upload);
    const int sent = mesh_crash_upload_send(&upload, "/nonexistent", &k_context, 0U);
    mesh_crash_upload_shutdown(&upload);
    MESH_TEST_FAIL_IF_CLEANUP(available, inkwell_loop_shutdown(&loop),
                              "a build with nowhere to send is not available");
    MESH_TEST_FAIL_IF_CLEANUP(sent != -ENOTSUP, inkwell_loop_shutdown(&loop),
                              "and refuses to send");

    MESH_TEST_FAIL_IF_CLEANUP(mesh_crash_upload_init(&upload, &loop, "not a dsn", NULL, NULL) != 0,
                              inkwell_loop_shutdown(&loop), "a bad DSN is not fatal");
    const bool bad_available = mesh_crash_upload_available(&upload);
    mesh_crash_upload_shutdown(&upload);
    inkwell_loop_shutdown(&loop);
    MESH_TEST_FAIL_IF(bad_available, "but it is not somewhere to send either");
    record_success(test_name);
}

#ifdef INKWELL_HAVE_TLS

#include "support/https_fixture.h"

/* The fake ingest: writes what arrived to the file it was given, and answers as Sentry does -
   or with a 429 on the path that asks for one. */
static void crash_serve(void *userdata, const struct https_fixture_request *request,
                        struct https_fixture_conn *conn) {
    const char *const path = (const char *)userdata;
    FILE *out = fopen(path, "we");
    if (out != NULL) {
        fprintf(out, "%s %s\n", request->method, request->target);
        if (request->body != NULL) {
            fwrite(request->body, 1U, request->body_len, out);
        }
        fclose(out);
    }
    if (strstr(request->target, "/api/429/") != NULL) {
        https_fixture_reply(conn, 429, "Retry-After: 60\r\n", "", 0U);
        return;
    }
    static const char k_ok[] = "{\"id\":\"accepted\"}";
    https_fixture_reply(conn, 200, "Content-Type: application/json\r\n", k_ok, sizeof k_ok - 1U);
}

struct crash_probe {
    unsigned calls;
    enum mesh_crash_upload_state state;
    int status;
};

static void crash_probe_done(void *userdata, const struct mesh_crash_upload *upload) {
    struct crash_probe *const probe = (struct crash_probe *)userdata;
    probe->calls++;
    probe->state = upload->state;
    probe->status = upload->status;
}

static bool crash_wait(struct inkwell_loop *loop, struct mesh_crash_upload *upload,
                       const struct crash_probe *probe, unsigned calls) {
    for (int i = 0; i < 1000 && probe->calls < calls; ++i) {
        inkwell_loop_run(loop, 10);
        mesh_crash_upload_tick(upload, (uint64_t)i * 10U);
    }
    return probe->calls >= calls;
}

MESH_TEST_CASE(crash_upload_posts_the_envelope_and_leaves_the_file, unit) {
    char dir[] = "/tmp/mesh_crash_uploadXXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "mkdtemp");
    char report_path[256];
    char seen_path[256];
    snprintf(report_path, sizeof report_path, "%s/crash.txt", dir);
    snprintf(seen_path, sizeof seen_path, "%s/seen.txt", dir);
    FILE *file = fopen(report_path, "we");
    MESH_TEST_FAIL_IF(file == NULL, "write the report");
    fwrite(k_report, 1U, sizeof k_report - 1U, file);
    fclose(file);

    const char *failure = NULL;
    struct https_fixture server;
    memset(&server, 0, sizeof server);
    struct inkwell_loop loop;
    bool loop_up = false;
    struct mesh_crash_upload upload;
    bool upload_up = false;
    struct crash_probe probe = {0};
    static char seen[MESH_CRASH_ENVELOPE_MAX + 256U];

    if (!https_fixture_start(&server, crash_serve, seen_path)) {
        failure = "could not stand up the fake ingest";
        goto cleanup;
    }
    if (inkwell_loop_init(&loop) != 0) {
        failure = "loop";
        goto cleanup;
    }
    loop_up = true;
    if (mesh_crash_upload_init(&upload, &loop, k_dsn, crash_probe_done, &probe) != 0) {
        failure = "init";
        goto cleanup;
    }
    upload_up = true;
    https_fixture_attach(&server, &upload.fetch);
    if (!mesh_crash_upload_available(&upload)) {
        failure = "a DSN and TLS should be somewhere to send";
        goto cleanup;
    }

    if (mesh_crash_upload_send(&upload, report_path, &k_context, 0U) != 0 ||
        !mesh_crash_upload_busy(&upload) || upload.state != MESH_CRASH_UPLOAD_SENDING) {
        failure = "a send should start";
        goto cleanup;
    }
    if (mesh_crash_upload_send(&upload, report_path, &k_context, 0U) != -EBUSY) {
        failure = "a second send while one is in flight should be refused";
        goto cleanup;
    }
    if (!crash_wait(&loop, &upload, &probe, 1U) || probe.state != MESH_CRASH_UPLOAD_SENT ||
        probe.status != 200) {
        failure = "the send should finish as sent";
        goto cleanup;
    }

    FILE *back = fopen(seen_path, "re");
    const size_t got = back != NULL ? fread(seen, 1U, sizeof seen - 1U, back) : 0U;
    seen[got] = '\0';
    if (back != NULL) {
        fclose(back);
    }
    if (strncmp(seen, "POST /api/42/envelope/\n{\"event_id\":\"", 36U) != 0) {
        failure = "the envelope should be POSTed to the project's envelope endpoint";
        goto cleanup;
    }
    if (strstr(seen, "SECRET") != NULL || strstr(seen, "Alice") != NULL) {
        failure = "the log must not be what arrived";
        goto cleanup;
    }
    if (access(report_path, F_OK) != 0) {
        failure = "the file is the caller's to discard, not the sender's";
        goto cleanup;
    }

    /* A server that says no is a failure with its status, and the file stays. */
    mesh_crash_upload_shutdown(&upload);
    upload_up = false;
    if (mesh_crash_upload_init(&upload, &loop, "https://abc123@example.invalid/429",
                               crash_probe_done, &probe) != 0) {
        failure = "init for the refusal";
        goto cleanup;
    }
    upload_up = true;
    https_fixture_attach(&server, &upload.fetch);
    if (mesh_crash_upload_send(&upload, report_path, &k_context, 0U) != 0 ||
        !crash_wait(&loop, &upload, &probe, 2U) || probe.state != MESH_CRASH_UPLOAD_FAILED ||
        probe.status != 429) {
        failure = "a 429 should be a failure that names its status";
        goto cleanup;
    }

    if (mesh_crash_upload_send(&upload, "/nonexistent/crash.txt", &k_context, 0U) != -ENOENT) {
        failure = "no report is -ENOENT";
        goto cleanup;
    }

cleanup:
    if (upload_up) {
        mesh_crash_upload_shutdown(&upload);
    }
    if (loop_up) {
        inkwell_loop_shutdown(&loop);
    }
    https_fixture_stop(&server);
    (void)unlink(report_path);
    (void)unlink(seen_path);
    (void)rmdir(dir);
    if (failure != NULL) {
        record_failure(test_name, failure);
    } else {
        record_success(test_name);
    }
}

#endif /* INKWELL_HAVE_TLS */
