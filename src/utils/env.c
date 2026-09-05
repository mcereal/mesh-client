#define _POSIX_C_SOURCE 200809L

#include "mesh/utils/env.h"

#include "mesh/utils/log.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const kTrue[] = {"1", "true", "yes", "on"};
static const char *const kFalse[] = {"0", "false", "no", "off"};

static bool matches_any(const char *value, const char *const *words, size_t count) {
    for (size_t i = 0U; i < count; ++i) {
        if (strcasecmp(value, words[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool mesh_env_bool(const char *name, const char *label, bool fallback) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    if (matches_any(value, kTrue, sizeof kTrue / sizeof kTrue[0])) {
        return true;
    }
    if (matches_any(value, kFalse, sizeof kFalse / sizeof kFalse[0])) {
        return false;
    }
    mesh_log_warn("env", "%s='%s' is not a boolean; keeping %s %s", name, value,
                  label != NULL ? label : name, fallback ? "on" : "off");
    return fallback;
}

long mesh_env_int(const char *name, long min, long max, long fallback) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    char *end = NULL;
    errno = 0;
    const long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || errno == ERANGE || parsed < min || parsed > max) {
        mesh_log_warn("env", "%s='%s' is not an integer in %ld..%ld; using %ld", name, value, min,
                      max, fallback);
        return fallback;
    }
    return parsed;
}
