#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mesh_log_level {
    MESH_LOG_LEVEL_TRACE = 0,
    MESH_LOG_LEVEL_DEBUG,
    MESH_LOG_LEVEL_INFO,
    MESH_LOG_LEVEL_WARN,
    MESH_LOG_LEVEL_ERROR,
    MESH_LOG_LEVEL_NONE
};

void mesh_log_set_level(enum mesh_log_level level);
enum mesh_log_level mesh_log_get_level(void);
const char *mesh_log_level_to_string(enum mesh_log_level level);

/*
 * The archetype is `printf` and the argument index is 0, which is how the attribute spells a
 * va_list variant: there are no further arguments here to check `fmt` against, and saying so is
 * what tells the compiler this format *is* a parameter rather than a string assembled somewhere
 * it cannot see. Without it the vfprintf() inside is a -Wformat-nonliteral on every clang build.
 */
void mesh_log_message_v(enum mesh_log_level level, const char *component, const char *fmt,
                        va_list args) __attribute__((format(printf, 3, 0)));

static inline void mesh_log_trace(const char *component, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static inline void mesh_log_debug(const char *component, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static inline void mesh_log_info(const char *component, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static inline void mesh_log_warn(const char *component, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static inline void mesh_log_error(const char *component, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static inline void mesh_log_trace(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mesh_log_message_v(MESH_LOG_LEVEL_TRACE, component, fmt, args);
    va_end(args);
}

static inline void mesh_log_debug(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mesh_log_message_v(MESH_LOG_LEVEL_DEBUG, component, fmt, args);
    va_end(args);
}

static inline void mesh_log_info(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mesh_log_message_v(MESH_LOG_LEVEL_INFO, component, fmt, args);
    va_end(args);
}

static inline void mesh_log_warn(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mesh_log_message_v(MESH_LOG_LEVEL_WARN, component, fmt, args);
    va_end(args);
}

static inline void mesh_log_error(const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mesh_log_message_v(MESH_LOG_LEVEL_ERROR, component, fmt, args);
    va_end(args);
}

/* ---- the file on the card ------------------------------------------------------------------- */

/*
 * Everything above writes to `stderr` and nothing here owns a file. On device it is `launch.sh`
 * that gives the stream somewhere to land, by piping the client through `tee -a` into
 * `/.userdata/$PLATFORM/logs/<pak>.txt` - an append, every run, with nothing ever cutting it
 * back. The three other files this client writes to the card all compact themselves
 * (MESH_UI_ARCHIVE_FILE_MAX_BYTES, MESH_UI_TRENDS_FILE_MAX_BYTES, and the bounded ring a crash
 * report carries); the log was the one that grew without a limit, because it is owned by a shell
 * script rather than by any of this.
 *
 * So the client cuts it back itself, at startup.
 *
 * **In place, and deliberately not by renaming.** `tee` already holds the file open by the time
 * `main()` runs, and its descriptor follows the inode: rename the file to `.1` and the whole of
 * *this* run goes into the rotated copy while the live path stays empty. Rewriting the same inode
 * is what keeps `tee` correct instead - it appends with `O_APPEND`, so its next write positions
 * itself at the new, shorter end and nothing is lost and nothing is written into a hole.
 *
 * The rewrite opens the path once and works on that descriptor from then on: the tail is shifted
 * down the file itself and the file is cut to what moved. Nothing temporary is written beside the
 * log, and the size that decides whether to truncate is read from the same descriptor that gets
 * truncated - measuring one file by name and then truncating whatever the name points at later is
 * the race CodeQL reports, and keeping tee's inode stops being luck once the name is out of it.
 *
 * Startup is the only moment this runs. A single session is left to grow, which is the trade:
 * what made the file unbounded was accumulating across every run since the card was written, and
 * mid-run truncation would be cutting the file underneath a `tee` that is actively writing a
 * line into it.
 */

/* When the log is cut back, and how much of the tail survives it.
 *
 * The threshold sits well above what is kept for the reason MESH_UI_ARCHIVE_FILE_MAX_BYTES does:
 * a file that came back from a compaction already close to tripping it would be rewritten again
 * on the next launch, and a launch is not a rare event. 128 KB of ordinary log lines is on the
 * order of fifteen hundred of them - far more than the 32 a crash report carries, and enough to
 * read what the client was doing before whatever is being investigated. */
#define MESH_LOG_FILE_MAX_BYTES (512U * 1024U)
#define MESH_LOG_FILE_KEEP_BYTES (128U * 1024U)

/* Enough for the logs directory plus a pak name and the extension. MESH_CRASH_PATH_MAX's number,
   for its reason: a path here is a value, not an allocation. */
#define MESH_LOG_FILE_PATH_MAX 256U

/*
 * Where `launch.sh` puts the log, worked out without its help.
 *
 * `MESHCLIENT_LOG_FILE` answers directly when it is set. Otherwise the path is derived from
 * `HOME`, which the launcher points at `.userdata/$PLATFORM/<pak>`: the log is that directory's
 * sibling `logs/<pak>.txt`. Deriving it rather than being told is the whole point - `launch.sh`
 * does not ship through self-update and the bare binary does, so a launcher that has never heard
 * of any of this still gets its log bounded on the next update.
 *
 * **`HOME` must have that exact shape for anything to be derived.** The derivation is two
 * components of guesswork, and on an ordinary host it lands somewhere real: `HOME=/srv/users/alice`
 * would name `/srv/users/logs/alice.txt` and the cap would then cut back a file the client has
 * nothing to do with. So the `.userdata/<platform>/<pak>` shape is required, and the override
 * above - which is somebody saying where their log is - is not subject to it.
 *
 * False when there is no `HOME`, or it is not a pak's userdata directory, leaving `out` empty.
 */
bool mesh_log_file_default_path(char *out, size_t out_len);

/*
 * Cut `path` back to its newest MESH_LOG_FILE_KEEP_BYTES when it has outgrown the cap.
 *
 * Returns the number of bytes reclaimed, 0 when the file was already small enough or is not
 * there, or -errno. **A file that does not exist is not an error and is not created** - which is
 * what makes a wrong guess at the path harmless: off device `HOME` derives somewhere that was
 * never a log, and the answer is that there is nothing to do rather than a stray file.
 *
 * The retained tail always starts at a line boundary, so the file never begins mid-sentence.
 */
long mesh_log_file_compact(const char *path);

/* mesh_log_file_default_path() and then mesh_log_file_compact(), which is all a startup wants.
   Reports what it did through the log itself, so the reason a log begins where it does is in the
   file the reader is already holding. */
void mesh_log_file_compact_default(void);

#ifdef __cplusplus
}
#endif
