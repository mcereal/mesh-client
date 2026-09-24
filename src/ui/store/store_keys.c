/*
 * The cache key table, and the two directions it is read in.
 *
 * Everything here comes off include/mesh/ui/store_keys.def, so the id, the spelling on disk and
 * the shape of the brackets are one line rather than three places that have to agree. See
 * store_keys.h for why that matters. What a table *does* - the lookup, the brackets, the writers
 * and their escape - is store_key_table.c's, and names no key; this file is the keys.
 */

#include "mesh/ui/store_keys.h"
#include "inkwell/base/record_file.h"

#include <stdarg.h>

/* The table, in .def order, offset by one so the enum's NONE sits at index 0. */
static const struct mesh_ui_store_key_row k_rows[MESH_UI_STORE_KEY_COUNT] = {
    {NULL, 0U, MESH_UI_STORE_KEY_KIND_PLAIN},
#define MESH_STORE_KEY_ENTRY(id, name) MESH_UI_STORE_KEY_ROW(name, MESH_UI_STORE_KEY_KIND_PLAIN),
#define MESH_STORE_KEY_ROW(id, name) MESH_UI_STORE_KEY_ROW(name, MESH_UI_STORE_KEY_KIND_ROW),
#define MESH_STORE_KEY_SLOT(id, name) MESH_UI_STORE_KEY_ROW(name, MESH_UI_STORE_KEY_KIND_SLOT),
#include "mesh/ui/store_keys.def"
#undef MESH_STORE_KEY_ENTRY
#undef MESH_STORE_KEY_ROW
#undef MESH_STORE_KEY_SLOT
};

static const struct mesh_ui_store_key_table k_keys = {k_rows, MESH_UI_STORE_KEY_COUNT};

const char *mesh_ui_store_key_name(enum mesh_ui_store_key key) {
    return mesh_ui_store_key_table_name(&k_keys, (int)key);
}

enum mesh_ui_store_key_kind mesh_ui_store_key_kind(enum mesh_ui_store_key key) {
    return mesh_ui_store_key_table_kind(&k_keys, (int)key);
}

bool mesh_ui_store_key_in_cache(enum mesh_ui_store_key key) {
    switch (key) {
    /* The trend log's own record. Everything else in the table is the cache's, and a new key
       that is not belongs here rather than in a comment nobody reads twice. */
    case MESH_UI_STORE_KEY_TREND:
        return false;
    default:
        return mesh_ui_store_key_name(key) != NULL;
    }
}

enum mesh_ui_store_key mesh_ui_store_key_lookup(const char *key, uint32_t *index, uint32_t *slot) {
    return (enum mesh_ui_store_key)mesh_ui_store_key_table_lookup(&k_keys, key, index, slot);
}

/*
 * The escape the cache has always used: anything a line cannot carry goes out as \xNN.
 *
 * A newline or an '=' inside a node's name would otherwise forge a second line, and the names
 * come off the air. The loader's unescape is its mirror.
 */
void mesh_ui_store_unescape_value(char *value) { inkwell_record_unescape(value); }

void mesh_ui_store_write(FILE *file, enum mesh_ui_store_key key, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mesh_ui_store_key_table_vwrite(file, &k_keys, (int)key, fmt, args);
    va_end(args);
}

void mesh_ui_store_write_row(FILE *file, enum mesh_ui_store_key key, uint32_t index,
                             const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mesh_ui_store_key_table_vwrite_row(file, &k_keys, (int)key, index, fmt, args);
    va_end(args);
}

void mesh_ui_store_write_slot(FILE *file, enum mesh_ui_store_key key, uint32_t index, uint32_t slot,
                              const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mesh_ui_store_key_table_vwrite_slot(file, &k_keys, (int)key, index, slot, fmt, args);
    va_end(args);
}

void mesh_ui_store_write_text(FILE *file, enum mesh_ui_store_key key, const char *text) {
    mesh_ui_store_key_table_write_text(file, &k_keys, (int)key, text);
}

void mesh_ui_store_write_row_text(FILE *file, enum mesh_ui_store_key key, uint32_t index,
                                  const char *text) {
    mesh_ui_store_key_table_write_row_text(file, &k_keys, (int)key, index, text);
}

void mesh_ui_store_write_slot_text(FILE *file, enum mesh_ui_store_key key, uint32_t index,
                                   uint32_t slot, const char *text) {
    mesh_ui_store_key_table_write_slot_text(file, &k_keys, (int)key, index, slot, text);
}
