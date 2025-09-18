#define _POSIX_C_SOURCE 200809L

#include "mesh/log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static enum mesh_log_level g_log_level = MESH_LOG_LEVEL_INFO;

void mesh_log_set_level(enum mesh_log_level level) {
    g_log_level = level;
}

enum mesh_log_level mesh_log_get_level(void) {
    return g_log_level;
}

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
    snprintf(buffer, buffer_len, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             tm_result.tm_year + 1900,
             tm_result.tm_mon + 1,
             tm_result.tm_mday,
             tm_result.tm_hour,
             tm_result.tm_min,
             tm_result.tm_sec,
             millis);
}

void mesh_log_message_v(enum mesh_log_level level, const char *component, const char *fmt, va_list args) {
    if (level < g_log_level || level == MESH_LOG_LEVEL_NONE) {
        return;
    }

    char timestamp[32];
    format_timestamp(timestamp, sizeof timestamp);

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

void mesh_log_message(enum mesh_log_level level, const char *component, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mesh_log_message_v(level, component, fmt, args);
    va_end(args);
}
