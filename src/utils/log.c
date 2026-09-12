#define _POSIX_C_SOURCE 200809L

#include "mesh/utils/log.h"

#include "mesh/utils/crash.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
