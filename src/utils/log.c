#define _POSIX_C_SOURCE 200809L

#include "mesh/utils/log.h"

#include "mesh/utils/crash.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static enum mesh_log_level g_log_level = MESH_LOG_LEVEL_INFO;

void mesh_log_set_level(enum mesh_log_level level) { g_log_level = level; }

enum mesh_log_level mesh_log_get_level(void) { return g_log_level; }

const char *mesh_log_level_to_string(enum mesh_log_level level) {
    switch (level) {
    case MESH_LOG_LEVEL_TRACE:
        return "TRACE";
    case MESH_LOG_LEVEL_DEBUG:
        return "DEBUG";
    case MESH_LOG_LEVEL_INFO:
        return "INFO";
    case MESH_LOG_LEVEL_WARN:
        return "WARN";
    case MESH_LOG_LEVEL_ERROR:
        return "ERROR";
    case MESH_LOG_LEVEL_NONE:
        return "NONE";
    }
    return "UNKNOWN";
}

static void format_timestamp(char *buffer, size_t buffer_len) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == 0) {
        snprintf(buffer, buffer_len, "0000-00-00T00:00:00.000Z");
        return;
    }

    struct tm tm_result;
    if (gmtime_r(&ts.tv_sec, &tm_result) == NULL) {
        snprintf(buffer, buffer_len, "0000-00-00T00:00:00.000Z");
        return;
    }

    const long millis = ts.tv_nsec / 1000000L;
    const int written = snprintf(buffer, buffer_len, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
                                 tm_result.tm_year + 1900, tm_result.tm_mon + 1, tm_result.tm_mday,
                                 tm_result.tm_hour, tm_result.tm_min, tm_result.tm_sec, millis);
    /* The same answer as the two failures above, and for the same reason: a timestamp cut off
       mid-field still reads as a time. A struct tm that gmtime_r filled in cannot overflow the
       32 bytes every caller passes - but neither the compiler nor this function knows that the
       buffer is that big or that the fields are in range. */
    if (written < 0 || (size_t)written >= buffer_len) {
        snprintf(buffer, buffer_len, "0000-00-00T00:00:00.000Z");
    }
}

/*
 * The same line again, into the ring a crash report carries.
 *
 * A copy rather than a redirect, which is the point: stderr keeps streaming through vfprintf
 * exactly as it did, so nothing about the log on the card changes, and the ring gets a bounded
 * rendering of the same line. `va_copy` is what lets both read the arguments - a va_list is
 * consumed by the first thing that walks it, and handing the same one to two formatters is the
 * kind of bug that works on one architecture.
 *
 * It is taken *after* the level check on purpose. The ring is meant to be the tail of the log
 * the user is looking at, and a ring holding lines the log file never showed would have the
 * crash report and the log disagree about what the client did - which is worse than a ring that
 * is quiet because the level was set high.
 */
static void log_capture(const char *timestamp, enum mesh_log_level level, const char *component,
                        const char *fmt, va_list args) __attribute__((format(printf, 4, 0)));

static void log_capture(const char *timestamp, enum mesh_log_level level, const char *component,
                        const char *fmt, va_list args) {
    char line[MESH_CRASH_LOG_LINE_MAX];
    int used = snprintf(line, sizeof line, "%s [%s]", timestamp, mesh_log_level_to_string(level));
    if (used < 0) {
        return;
    }
    size_t offset = (size_t)used < sizeof line ? (size_t)used : sizeof line - 1U;

    if (component != NULL && component[0] != '\0') {
        used = snprintf(line + offset, sizeof line - offset, " (%s)", component);
        if (used > 0) {
            offset +=
                (size_t)used < sizeof line - offset ? (size_t)used : sizeof line - offset - 1U;
        }
    }
    used = snprintf(line + offset, sizeof line - offset, ": ");
    if (used > 0) {
        offset += (size_t)used < sizeof line - offset ? (size_t)used : sizeof line - offset - 1U;
    }
    (void)vsnprintf(line + offset, sizeof line - offset, fmt, args);

    /* The message may have ended in the newline the stderr path below adds for itself. A ring
       entry is one line by construction, so it is taken back off rather than written out as a
       blank line in the middle of the report. */
    size_t len = strlen(line);
    while (len > 0U && (line[len - 1U] == '\n' || line[len - 1U] == '\r')) {
        line[--len] = '\0';
    }
    mesh_crash_log_line(line);
}

void mesh_log_message_v(enum mesh_log_level level, const char *component, const char *fmt,
                        va_list args) {
    if (level < g_log_level || level == MESH_LOG_LEVEL_NONE) {
        return;
    }

    char timestamp[32];
    format_timestamp(timestamp, sizeof timestamp);

    va_list captured;
    va_copy(captured, args);
    log_capture(timestamp, level, component, fmt, captured);
    va_end(captured);

    fprintf(stderr, "%s [%s]", timestamp, mesh_log_level_to_string(level));
    if (component != NULL && component[0] != '\0') {
        fprintf(stderr, " (%s)", component);
    }
    fprintf(stderr, ": ");

    vfprintf(stderr, fmt, args);

    if (fmt[0] == '\0' || fmt[strlen(fmt) - 1] != '\n') {
        fputc('\n', stderr);
    }
}

/* ---- the file on the card ------------------------------------------------------------------- */

/* One pass's worth of copying. Small on purpose: this runs twice over at most
   MESH_LOG_FILE_KEEP_BYTES, at startup, and a bigger buffer would buy nothing a card's own
   readahead does not already. */
#define LOG_FILE_CHUNK 4096U

bool mesh_log_file_default_path(char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';

    const char *override = getenv("MESHCLIENT_LOG_FILE");
    if (override != NULL && override[0] != '\0') {
        const int written = snprintf(out, out_len, "%s", override);
        if (written < 0 || (size_t)written >= out_len) {
            out[0] = '\0';
            return false;
        }
        return true;
    }

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return false;
    }

    /* `HOME` is `.userdata/$PLATFORM/<pak>`, so the pak name is its last component and the logs
       directory is its sibling. A trailing slash would otherwise make the name empty and the
       whole derivation nonsense, so it is stepped over first. */
    size_t home_len = strlen(home);
    while (home_len > 1U && home[home_len - 1U] == '/') {
        home_len--;
    }
    /* Scanned by hand rather than with memrchr(), which is a GNU extension and not declared
       under the _POSIX_C_SOURCE this file asks for - the cross build does not have it. */
    size_t split = home_len;
    while (split > 0U && home[split - 1U] != '/') {
        split--;
    }
    if (split <= 1U) {
        return false;
    }
    const size_t parent_len = split - 1U;
    const char *name = home + split;
    const size_t name_len = home_len - split;
    if (name_len == 0U) {
        return false;
    }

    const int written =
        snprintf(out, out_len, "%.*s/logs/%.*s.txt", (int)parent_len, home, (int)name_len, name);
    if (written < 0 || (size_t)written >= out_len) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* Copies from `from`'s current position to the end of `into`, in LOG_FILE_CHUNK bites. */
static int log_file_copy_rest(FILE *from, FILE *into) {
    char chunk[LOG_FILE_CHUNK];
    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1U, sizeof chunk, from)) > 0U) {
        if (fwrite(chunk, 1U, read_bytes, into) != read_bytes) {
            return -EIO;
        }
    }
    return ferror(from) ? -EIO : 0;
}

long mesh_log_file_compact(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    struct stat info;
    if (stat(path, &info) != 0) {
        /* Not there is not a failure: see the header on why a derived path that was never a log
           has to be a no-op rather than an error. */
        return errno == ENOENT ? 0L : -errno;
    }
    if (!S_ISREG(info.st_mode)) {
        return 0L;
    }
    if (info.st_size <= (off_t)MESH_LOG_FILE_MAX_BYTES) {
        return 0L;
    }

    char scratch[MESH_LOG_FILE_PATH_MAX];
    const int scratch_written = snprintf(scratch, sizeof scratch, "%s.compact", path);
    if (scratch_written < 0 || (size_t)scratch_written >= sizeof scratch) {
        return -ENAMETOOLONG;
    }

    FILE *source = fopen(path, "r");
    if (source == NULL) {
        return -errno;
    }
    if (fseeko(source, info.st_size - (off_t)MESH_LOG_FILE_KEEP_BYTES, SEEK_SET) != 0) {
        const int failed = -errno;
        (void)fclose(source);
        return failed;
    }
    /* Landing mid-line is the ordinary case, since the cut is a byte count. Discarding the
       remainder of that line is what keeps the file from starting mid-sentence; end-of-file here
       would mean a single line longer than the whole retained tail, and then there is nothing
       worth keeping. */
    int discard;
    while ((discard = fgetc(source)) != '\n' && discard != EOF) {
        /* stepping to the line boundary */
    }
    if (discard == EOF) {
        (void)fclose(source);
        return 0L;
    }
    /*
     * Nothing after that boundary means the newline just stepped over was the file's last byte -
     * a single line longer than the whole retained window. Emptying the file is the one outcome
     * worse than leaving it oversized: the line that made it oversized is also the only thing in
     * it worth reading, so the cap yields rather than throwing the log away.
     */
    const off_t resume = ftello(source);
    if (resume < 0 || resume >= info.st_size) {
        (void)fclose(source);
        return 0L;
    }

    FILE *tail = fopen(scratch, "w");
    if (tail == NULL) {
        const int failed = -errno;
        (void)fclose(source);
        return failed;
    }
    int result = log_file_copy_rest(source, tail);
    (void)fclose(source);
    if (result == 0 && fclose(tail) != 0) {
        result = -errno;
    } else if (result != 0) {
        (void)fclose(tail);
    }
    if (result != 0) {
        (void)unlink(scratch);
        return result;
    }

    /* The original is reopened rather than replaced, which is the point: `tee` is holding this
       inode and appending to it. "w" truncates it to nothing and the tail goes back in at the
       front, so tee's next O_APPEND write lands after it. */
    tail = fopen(scratch, "r");
    if (tail == NULL) {
        const int failed = -errno;
        (void)unlink(scratch);
        return failed;
    }
    FILE *live = fopen(path, "w");
    if (live == NULL) {
        const int failed = -errno;
        (void)fclose(tail);
        (void)unlink(scratch);
        return failed;
    }
    result = log_file_copy_rest(tail, live);
    (void)fclose(tail);
    if (fclose(live) != 0 && result == 0) {
        result = -errno;
    }
    (void)unlink(scratch);
    if (result != 0) {
        return result;
    }

    struct stat after;
    const off_t now = stat(path, &after) == 0 ? after.st_size : 0;
    return (long)(info.st_size - now);
}

void mesh_log_file_compact_default(void) {
    char path[MESH_LOG_FILE_PATH_MAX];
    if (!mesh_log_file_default_path(path, sizeof path)) {
        return;
    }
    const long reclaimed = mesh_log_file_compact(path);
    if (reclaimed < 0) {
        mesh_log_warn("log", "Could not cut back %s: %s", path, strerror((int)-reclaimed));
    } else if (reclaimed > 0) {
        mesh_log_info("log", "Cut %s back to its newest %u KB (reclaimed %ld KB)", path,
                      (unsigned)(MESH_LOG_FILE_KEEP_BYTES / 1024U), reclaimed / 1024L);
    }
}
