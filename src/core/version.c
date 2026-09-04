#include "mesh/version.h"

#include <ctype.h>
#include <string.h>

#ifndef MESHCLIENT_VERSION
#define MESHCLIENT_VERSION "0.0.0-dev"
#endif

const char *mesh_version_string(void) { return MESHCLIENT_VERSION; }

/*
 * Only the release build defines MESHCLIENT_RELEASE_BUILD, and it is what the updater gates on
 * - not the shape of the version string. A local build reports "<version>-dev" and answers
 * false here, so the updater never offers to replace a binary someone just built.
 */
bool mesh_version_is_release(void) {
#ifdef MESHCLIENT_RELEASE_BUILD
    return true;
#else
    return false;
#endif
}

/* One parsed version. `pre` points into the caller's string and is NULL when there is none. */
struct semver {
    unsigned long major;
    unsigned long minor;
    unsigned long patch;
    const char *pre;
    size_t pre_len;
};

/* Reads digits into *out, capped so a long run of digits cannot wrap. Returns how many it
   consumed; 0 means there was no number here. */
static size_t parse_number(const char *text, unsigned long *out) {
    size_t used = 0U;
    unsigned long value = 0UL;
    while (isdigit((unsigned char)text[used])) {
        if (value < 100000000UL) {
            value = value * 10UL + (unsigned long)(text[used] - '0');
        }
        used++;
    }
    *out = value;
    return used;
}

static bool semver_parse(const char *text, struct semver *out) {
    if (text == NULL) {
        return false;
    }
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    if (*text == 'v' || *text == 'V') {
        text++;
    }
    memset(out, 0, sizeof *out);

    size_t used = parse_number(text, &out->major);
    if (used == 0U) {
        return false;
    }
    text += used;
    if (*text == '.') {
        used = parse_number(text + 1, &out->minor);
        if (used == 0U) {
            return false;
        }
        text += used + 1U;
        if (*text == '.') {
            used = parse_number(text + 1, &out->patch);
            if (used == 0U) {
                return false;
            }
            text += used + 1U;
        }
    }

    if (*text == '-') {
        out->pre = text + 1;
        /* Build metadata is not part of precedence, so it ends the prerelease and is dropped. */
        const char *plus = strchr(out->pre, '+');
        out->pre_len = plus != NULL ? (size_t)(plus - out->pre) : strlen(out->pre);
        if (out->pre_len == 0U) {
            return false;
        }
        return true;
    }
    /* Only build metadata or nothing may follow the numbers. */
    return *text == '\0' || *text == '+';
}

/* The next dot-separated identifier of a prerelease string, or false when it is exhausted. */
static bool pre_next(const char **cursor, const char *end, const char **out, size_t *out_len) {
    if (*cursor >= end) {
        return false;
    }
    const char *start = *cursor;
    const char *stop = start;
    while (stop < end && *stop != '.') {
        stop++;
    }
    *out = start;
    *out_len = (size_t)(stop - start);
    *cursor = stop < end ? stop + 1 : end;
    return true;
}

static bool pre_is_numeric(const char *text, size_t len) {
    if (len == 0U) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!isdigit((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

static int compare_prerelease(const struct semver *a, const struct semver *b) {
    /* No prerelease outranks any prerelease: 1.2.0 > 1.2.0-rc.1. */
    if (a->pre == NULL && b->pre == NULL) {
        return 0;
    }
    if (a->pre == NULL) {
        return 1;
    }
    if (b->pre == NULL) {
        return -1;
    }

    const char *a_cursor = a->pre;
    const char *b_cursor = b->pre;
    const char *a_end = a->pre + a->pre_len;
    const char *b_end = b->pre + b->pre_len;
    for (;;) {
        const char *a_part = NULL;
        const char *b_part = NULL;
        size_t a_len = 0U;
        size_t b_len = 0U;
        const bool has_a = pre_next(&a_cursor, a_end, &a_part, &a_len);
        const bool has_b = pre_next(&b_cursor, b_end, &b_part, &b_len);
        if (!has_a && !has_b) {
            return 0;
        }
        /* A shorter run of identifiers sorts lower when every shared one matched. */
        if (!has_a) {
            return -1;
        }
        if (!has_b) {
            return 1;
        }

        const bool a_num = pre_is_numeric(a_part, a_len);
        const bool b_num = pre_is_numeric(b_part, b_len);
        if (a_num && b_num) {
            unsigned long a_value = 0UL;
            unsigned long b_value = 0UL;
            (void)parse_number(a_part, &a_value);
            (void)parse_number(b_part, &b_value);
            if (a_value != b_value) {
                return a_value < b_value ? -1 : 1;
            }
            continue;
        }
        /* Numeric identifiers always sort below alphanumeric ones. */
        if (a_num != b_num) {
            return a_num ? -1 : 1;
        }
        const size_t shared = a_len < b_len ? a_len : b_len;
        const int diff = strncmp(a_part, b_part, shared);
        if (diff != 0) {
            return diff < 0 ? -1 : 1;
        }
        if (a_len != b_len) {
            return a_len < b_len ? -1 : 1;
        }
    }
}

int mesh_version_compare(const char *a, const char *b) {
    struct semver left;
    struct semver right;
    const bool left_ok = semver_parse(a, &left);
    const bool right_ok = semver_parse(b, &right);
    if (!left_ok || !right_ok) {
        if (left_ok == right_ok) {
            return 0;
        }
        return left_ok ? 1 : -1;
    }
    if (left.major != right.major) {
        return left.major < right.major ? -1 : 1;
    }
    if (left.minor != right.minor) {
        return left.minor < right.minor ? -1 : 1;
    }
    if (left.patch != right.patch) {
        return left.patch < right.patch ? -1 : 1;
    }
    return compare_prerelease(&left, &right);
}

bool mesh_version_is_newer_than_running(const char *candidate) {
    if (!mesh_version_is_release()) {
        return false;
    }
    struct semver parsed;
    if (!semver_parse(candidate, &parsed)) {
        return false;
    }
    return mesh_version_compare(candidate, mesh_version_string()) > 0;
}

bool mesh_version_is_prerelease(void) {
    struct semver parsed;
    return semver_parse(MESHCLIENT_VERSION, &parsed) && parsed.pre != NULL;
}
