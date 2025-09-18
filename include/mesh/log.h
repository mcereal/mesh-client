#pragma once

#include <stdarg.h>

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

void mesh_log_message(enum mesh_log_level level, const char *component, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void mesh_log_message_v(enum mesh_log_level level, const char *component, const char *fmt, va_list args);

static inline void mesh_log_trace(const char *component, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void mesh_log_debug(const char *component, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void mesh_log_info(const char *component, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void mesh_log_warn(const char *component, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void mesh_log_error(const char *component, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

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

#ifdef __cplusplus
}
#endif
