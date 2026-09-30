#pragma once

/*
 * Sending a crash report somewhere, when - and only when - the user says so.
 *
 * inkwell's crash handler writes a file and promises that nothing leaves the device by itself
 * (inkwell/runtime/crash.h says why: a faulted process was holding whatever it was holding).
 * That promise stands. What this adds is the step after it: a press on About that sends *some*
 * of that file to a Sentry project, so a fault on a device nobody here owns becomes a stack
 * with symbols rather than "it crashed".
 *
 * **What goes is an allowlist, not the file.** The report's log tail is this client's ordinary
 * log, and mesh/utils/crash.h spells out what that can hold: node names, channel names, a
 * message the user sent, a position they typed. None of it is sent. What is sent is what the
 * parser below lifts out by label - the signal, the addresses, the build id, the version, and
 * the notes this client records about where it was standing - and each of those is a fact about
 * the program rather than about the user. The one note that could name something, the route,
 * is cut to its screen and level: `nodes/detail:0a1b2c3d` goes as `nodes/detail`, because a
 * node number is somebody's radio.
 *
 * **No SDK.** Sentry's own native client runs threads of its own and brings a crash handler to
 * sit beside the one inkwell already has; this client has one loop and no threads. What Sentry
 * takes over the wire is one HTTPS POST of an "envelope" - a line of JSON saying who is
 * sending, a line saying what follows, and the event - so that is what this writes, through
 * inkwell's fetch.
 *
 * **The DSN is compiled in, and is not a secret.** A DSN is a write-only key every Sentry
 * client ships with. It comes from the release build (`-DMESHCLIENT_CRASH_DSN`), so a local
 * build has none and the About row simply is not offered; `MESHCLIENT_CRASH_DSN` in the
 * environment overrides it, which is how a developer points a dev build at a project of their
 * own.
 *
 * **One report, one id.** The event id is a digest of the report's text, so pressing Send twice
 * for the same file - a press whose answer was lost with the network - is the same event both
 * times, and the server keeps one.
 */

#include "inkwell/net/fetch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct inkwell_loop;

/* The deepest stack the report walks (inkwell writes at most 32 frames), plus the pc. */
#define MESH_CRASH_FRAMES_MAX 33U
/* A build id in hex: SHA-1's 40 digits and room for a longer one. */
#define MESH_CRASH_BUILD_ID_MAX 65U
#define MESH_CRASH_FIELD_MAX 64U
/* An envelope with every field at its longest is under 6 KB; this leaves room to be wrong. */
#define MESH_CRASH_ENVELOPE_MAX 16384U
/* The report file is a few KB; one past this is not a report this client wrote. */
#define MESH_CRASH_REPORT_FILE_MAX 65536U

/*
 * What is lifted out of a report, and all that is. A field the report did not carry is left
 * zero/empty, and the event is built from what is there - a report from an older build without
 * a build id still sends, and simply cannot be symbolicated.
 */
struct mesh_crash_report {
    int signal;
    bool windows_exception;
    uint32_t exception_code;
    char signal_name[MESH_CRASH_FIELD_MAX];
    bool have_code;
    int code;
    bool have_fault_addr;
    uint64_t fault_addr;
    uint64_t uptime_ms;
    bool have_load_base;
    uint64_t load_base;
    uint64_t image_size;
    char build_id[MESH_CRASH_BUILD_ID_MAX];
    char code_id[MESH_CRASH_BUILD_ID_MAX];
    /* The notes, by the labels mesh/utils/crash.h gives them. */
    char version[MESH_CRASH_FIELD_MAX];
    char route[MESH_CRASH_FIELD_MAX]; /* screen and level only - see the top of this file */
    char transport[MESH_CRASH_FIELD_MAX];
    char backend[MESH_CRASH_FIELD_MAX];
    char screen[MESH_CRASH_FIELD_MAX];
    /* Innermost first, as the report writes them: the pc, then each return address. */
    uint64_t frames[MESH_CRASH_FRAMES_MAX];
    size_t frame_count;
};

/* Reads a report's text. False when it is not one - no signal line - and `out` is then empty. */
bool mesh_crash_report_parse(const char *text, size_t len, struct mesh_crash_report *out);

/* A DSN, taken apart: `https://<key>@<host>[:port]/<project>`. */
struct mesh_crash_dsn {
    char dsn[256];
    char key[64];
    char project[32];
    /* https://<host>/api/<project>/envelope/ */
    char envelope_url[320];
};

/* False for anything that is not an https DSN with a key and a numeric project. */
bool mesh_crash_dsn_parse(const char *dsn, struct mesh_crash_dsn *out);

/* What is true of the process sending, rather than the one that crashed. */
struct mesh_crash_context {
    /* The version of this client, for the envelope's client name. */
    const char *sender_version;
    /* "production" for a release build, "development" otherwise. */
    const char *environment;
    /* The name the report's addresses resolve against - the binary's file name. */
    const char *binary;
    /* The operating system and architecture this build is for. */
    const char *os;
    const char *arch;
    /* Unix seconds now. The report's own wall clock is not used: a Brick with no RTC that
       has not reached a network reads 1970. */
    uint64_t now_unix;
};

/* This build's context: its version, whether it is a release, its binary, its system and
   architecture - all fixed at compile time - and `now_unix`. */
void mesh_crash_context_init(struct mesh_crash_context *out, uint64_t now_unix);

/* The digest the event id is made of: the first 16 bytes of the text's SHA-256, in hex. */
void mesh_crash_event_id(const char *text, size_t len, char out[33]);

/*
 * The envelope for `report`: the header, the item header and the event, one line each. Returns
 * its length, or -ENOSPC when it does not fit `cap`, -EINVAL for a missing argument.
 */
int mesh_crash_envelope_build(const struct mesh_crash_report *report, const char *event_id,
                              const struct mesh_crash_dsn *dsn,
                              const struct mesh_crash_context *context, char *out, size_t cap);

/* ---- sending one ---------------------------------------------------------------------------- */

enum mesh_crash_upload_state {
    MESH_CRASH_UPLOAD_IDLE = 0,
    MESH_CRASH_UPLOAD_SENDING,
    MESH_CRASH_UPLOAD_SENT,
    MESH_CRASH_UPLOAD_FAILED,
};

struct mesh_crash_upload;

/* Called once per send that started, from the loop, when it is over. */
typedef void (*mesh_crash_upload_done_fn)(void *userdata, const struct mesh_crash_upload *upload);

struct mesh_crash_upload {
    /* Its own fetcher, for the reason the updater and the firmware check each have one: two
       presses must not take each other's request. */
    struct inkwell_fetch fetch;
    struct mesh_crash_dsn dsn;
    bool configured;
    enum mesh_crash_upload_state state;
    /* How the last send ended, for the toast and the log. */
    enum inkwell_fetch_outcome outcome;
    int status;
    /* Why the connection failed, and to which host - what mesh_net_reason_format() turns into
       a sentence a reader can act on. */
    struct inkwell_net_failure failure;
    char host[128];
    mesh_crash_upload_done_fn on_done;
    void *userdata;
};

/*
 * The DSN this build sends to: MESHCLIENT_CRASH_DSN from the environment, else the one the
 * release build compiled in, else "". Never NULL.
 */
const char *mesh_crash_upload_default_dsn(void);

/*
 * Ready a sender. `dsn` may be NULL or "", which is a build with nowhere to send: init still
 * succeeds and _available() says no. Returns 0, or the fetcher's -errno.
 */
int mesh_crash_upload_init(struct mesh_crash_upload *upload, struct inkwell_loop *loop,
                           const char *dsn, mesh_crash_upload_done_fn on_done, void *userdata);
void mesh_crash_upload_shutdown(struct mesh_crash_upload *upload);

/* True when there is a DSN and a way to reach it (TLS, a loop). */
bool mesh_crash_upload_available(const struct mesh_crash_upload *upload);
bool mesh_crash_upload_busy(const struct mesh_crash_upload *upload);

/*
 * Read the report at `path`, build its envelope, and send it. The file is not touched: whether
 * a sent report is then discarded is the caller's decision, made in `on_done`.
 *
 * Returns 0 when a request started, -ENOTSUP when unavailable, -EBUSY while one is in flight,
 * -ENOENT when there is no report, -EBADMSG when the file is not one, or the fetcher's -errno.
 */
int mesh_crash_upload_send(struct mesh_crash_upload *upload, const char *path,
                           const struct mesh_crash_context *context, uint64_t now_ms);

/* Enforces the request's deadline. Call every loop turn. */
void mesh_crash_upload_tick(struct mesh_crash_upload *upload, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
