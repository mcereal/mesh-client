/*
 * The cache key table, and the two directions it is read in.
 *
 * Everything here comes off include/mesh/ui/store_keys.def, so the id, the spelling on disk and
 * the shape of the brackets are one line rather than three places that have to agree. See
 * store_keys.h for why that matters.
 */

#include "mesh/ui/store_keys.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct key_row {
    const char *name;
    size_t length;
    enum mesh_ui_store_key_kind kind;
};

/*
 * The table, in .def order, offset by one so the enum's NONE sits at index 0.
 *
 * `length` is taken with sizeof on the literal rather than written out, which is the point: the
 * loader used to carry a hand-counted length beside every prefix it matched, and a miscount
 * there is a key that silently never matches.
 */
static const struct key_row k_keys[MESH_UI_STORE_KEY_COUNT] = {
    {NULL, 0U, MESH_UI_STORE_KEY_KIND_PLAIN},
#define MESH_STORE_KEY_ENTRY(id, name) {name, sizeof(name) - 1U, MESH_UI_STORE_KEY_KIND_PLAIN},
#define MESH_STORE_KEY_ROW(id, name) {name, sizeof(name) - 1U, MESH_UI_STORE_KEY_KIND_ROW},
#define MESH_STORE_KEY_SLOT(id, name) {name, sizeof(name) - 1U, MESH_UI_STORE_KEY_KIND_SLOT},
#include "mesh/ui/store_keys.def"
#undef MESH_STORE_KEY_ENTRY
#undef MESH_STORE_KEY_ROW
#undef MESH_STORE_KEY_SLOT
};

static bool key_in_range(enum mesh_ui_store_key key) {
    return key > MESH_UI_STORE_KEY_NONE && key < MESH_UI_STORE_KEY_COUNT;
}

const char *mesh_ui_store_key_name(enum mesh_ui_store_key key) {
    return key_in_range(key) ? k_keys[key].name : NULL;
}

enum mesh_ui_store_key_kind mesh_ui_store_key_kind(enum mesh_ui_store_key key) {
    return key_in_range(key) ? k_keys[key].kind : MESH_UI_STORE_KEY_KIND_PLAIN;
}

/*
 * One unsigned number, and where it stopped.
 *
 * strtoul rather than sscanf because the caller has to know the terminator to tell `name[3]`
 * from `name[3.1]`, and because a number wider than the roster can hold should read as a key
 * this build does not know rather than as a wrapped index into it.
 *
 * Both halves of that last part are needed, and which one catches an absurd index depends on the
 * target: where `unsigned long` is 64 bits the range check does it, and where it is 32 - armv7,
 * which is still on the table - strtoul has already saturated at ULONG_MAX and only ERANGE can
 * still tell "4294967295" from a number far past it. Checked the way mesh_env_parse_int() checks
 * it.
 */
static bool parse_index(const char *text, uint32_t *out, const char **end) {
    if (text == NULL || *text < '0' || *text > '9') {
        return false;
    }
    char *stop = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &stop, 10);
    if (stop == text || errno == ERANGE || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    *end = stop;
    return true;
}

/* The first entry whose name and kind both match. A name may appear twice with two kinds -
   `airtime` is a count and `airtime[0]` is a sample - so both halves have to agree. */
static enum mesh_ui_store_key find_key(const char *name, size_t length,
                                       enum mesh_ui_store_key_kind kind) {
    for (int i = MESH_UI_STORE_KEY_NONE + 1; i < MESH_UI_STORE_KEY_COUNT; ++i) {
        if (k_keys[i].kind == kind && k_keys[i].length == length &&
            memcmp(k_keys[i].name, name, length) == 0) {
            return (enum mesh_ui_store_key)i;
        }
    }
    return MESH_UI_STORE_KEY_NONE;
}

enum mesh_ui_store_key mesh_ui_store_key_lookup(const char *key, uint32_t *index, uint32_t *slot) {
    if (index != NULL) {
        *index = 0U;
    }
    if (slot != NULL) {
        *slot = 0U;
    }
    if (key == NULL || key[0] == '\0') {
        return MESH_UI_STORE_KEY_NONE;
    }

    const char *bracket = strchr(key, '[');
    if (bracket == NULL) {
        return find_key(key, strlen(key), MESH_UI_STORE_KEY_KIND_PLAIN);
    }

    const size_t length = (size_t)(bracket - key);
    if (length == 0U) {
        return MESH_UI_STORE_KEY_NONE;
    }

    uint32_t parsed_index = 0U;
    const char *after = NULL;
    if (!parse_index(bracket + 1, &parsed_index, &after)) {
        return MESH_UI_STORE_KEY_NONE;
    }

    /* `name[3]` is a row; `name[3.1]` is a slot in one. Anything else is neither. */
    enum mesh_ui_store_key_kind kind = MESH_UI_STORE_KEY_KIND_ROW;
    uint32_t parsed_slot = 0U;
    if (*after == '.') {
        if (!parse_index(after + 1, &parsed_slot, &after)) {
            return MESH_UI_STORE_KEY_NONE;
        }
        kind = MESH_UI_STORE_KEY_KIND_SLOT;
    }
    if (after[0] != ']' || after[1] != '\0') {
        return MESH_UI_STORE_KEY_NONE;
    }

    const enum mesh_ui_store_key found = find_key(key, length, kind);
    if (found == MESH_UI_STORE_KEY_NONE) {
        return MESH_UI_STORE_KEY_NONE;
    }
    if (index != NULL) {
        *index = parsed_index;
    }
    if (slot != NULL) {
        *slot = parsed_slot;
    }
    return found;
}

/*
 * The escape the cache has always used: anything a line cannot carry goes out as \xNN.
 *
 * A newline or an '=' inside a node's name would otherwise forge a second line, and the names
 * come off the air. The loader's unescape is its mirror.
 */
static void write_escaped(FILE *file, const char *value) {
    if (value == NULL) {
        return;
    }
    for (const unsigned char *ptr = (const unsigned char *)value; *ptr != '\0'; ++ptr) {
        if (*ptr < 0x20U || *ptr == '\\' || *ptr == '=') {
            fprintf(file, "\\x%02x", *ptr);
        } else {
            fputc((int)*ptr, file);
        }
    }
}

void mesh_ui_store_unescape_value(char *value) {
    if (value == NULL) {
        return;
    }

    char *write_ptr = value;
    for (char *read_ptr = value; *read_ptr != '\0'; ++read_ptr) {
        if (*read_ptr == '\\') {
            if (read_ptr[1] == 'x' && read_ptr[2] != '\0' && read_ptr[3] != '\0') {
                char hex[3] = {read_ptr[2], read_ptr[3], '\0'};
                *write_ptr++ = (char)strtol(hex, NULL, 16);
                read_ptr += 3;
            }
        } else {
            *write_ptr++ = *read_ptr;
        }
    }
    *write_ptr = '\0';
}

/* Every writer funnels through here, so a key that is not in the table writes nothing at all
   rather than a line the loader would skip. */
static bool write_key(FILE *file, enum mesh_ui_store_key key) {
    const char *name = mesh_ui_store_key_name(key);
    if (file == NULL || name == NULL) {
        return false;
    }
    fputs(name, file);
    return true;
}

void mesh_ui_store_write(FILE *file, enum mesh_ui_store_key key, const char *fmt, ...) {
    if (!write_key(file, key)) {
        return;
    }
    fputc('=', file);
    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fputc('\n', file);
}

void mesh_ui_store_write_row(FILE *file, enum mesh_ui_store_key key, uint32_t index,
                             const char *fmt, ...) {
    if (!write_key(file, key)) {
        return;
    }
    fprintf(file, "[%u]=", index);
    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fputc('\n', file);
}

void mesh_ui_store_write_slot(FILE *file, enum mesh_ui_store_key key, uint32_t index, uint32_t slot,
                              const char *fmt, ...) {
    if (!write_key(file, key)) {
        return;
    }
    fprintf(file, "[%u.%u]=", index, slot);
    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fputc('\n', file);
}

void mesh_ui_store_write_text(FILE *file, enum mesh_ui_store_key key, const char *text) {
    if (!write_key(file, key)) {
        return;
    }
    fputc('=', file);
    write_escaped(file, text);
    fputc('\n', file);
}

void mesh_ui_store_write_row_text(FILE *file, enum mesh_ui_store_key key, uint32_t index,
                                  const char *text) {
    if (!write_key(file, key)) {
        return;
    }
    fprintf(file, "[%u]=", index);
    write_escaped(file, text);
    fputc('\n', file);
}
