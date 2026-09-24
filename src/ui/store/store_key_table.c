/*
 * A key table, and the two directions it is read in. See store_key_table.h; the keys themselves
 * are store_keys.c's.
 */

#include "mesh/ui/store_key_table.h"
#include "inkwell/base/record_file.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool key_in_range(const struct mesh_ui_store_key_table *table, int key) {
    return table != NULL && table->rows != NULL && key > 0 && (size_t)key < table->count;
}

const char *mesh_ui_store_key_table_name(const struct mesh_ui_store_key_table *table, int key) {
    return key_in_range(table, key) ? table->rows[key].name : NULL;
}

enum mesh_ui_store_key_kind
mesh_ui_store_key_table_kind(const struct mesh_ui_store_key_table *table, int key) {
    return key_in_range(table, key) ? table->rows[key].kind : MESH_UI_STORE_KEY_KIND_PLAIN;
}

/*
 * One unsigned number, and where it stopped.
 *
 * strtoul rather than sscanf because the caller has to know the terminator to tell `name[3]`
 * from `name[3.1]`, and because a number wider than an index can hold should read as a key this
 * build does not know rather than as a wrapped index into it.
 *
 * Both halves of that last part are needed, and which one catches an absurd index depends on the
 * target: where `unsigned long` is 64 bits the range check does it, and where it is 32 - armv7,
 * which is still on the table - strtoul has already saturated at ULONG_MAX and only ERANGE can
 * still tell "4294967295" from a number far past it. Checked the way inkwell_env_int() checks
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

/* The first row whose name and kind both match. A name may appear twice with two kinds - a
   count and the rows it counts - so both halves have to agree. */
static int find_key(const struct mesh_ui_store_key_table *table, const char *name, size_t length,
                    enum mesh_ui_store_key_kind kind) {
    for (size_t i = 1U; i < table->count; ++i) {
        const struct mesh_ui_store_key_row *row = &table->rows[i];
        if (row->name != NULL && row->kind == kind && row->length == length &&
            memcmp(row->name, name, length) == 0) {
            return (int)i;
        }
    }
    return 0;
}

int mesh_ui_store_key_table_lookup(const struct mesh_ui_store_key_table *table, const char *text,
                                   uint32_t *index, uint32_t *slot) {
    if (index != NULL) {
        *index = 0U;
    }
    if (slot != NULL) {
        *slot = 0U;
    }
    if (table == NULL || table->rows == NULL || text == NULL || text[0] == '\0') {
        return 0;
    }

    const char *bracket = strchr(text, '[');
    if (bracket == NULL) {
        return find_key(table, text, strlen(text), MESH_UI_STORE_KEY_KIND_PLAIN);
    }

    const size_t length = (size_t)(bracket - text);
    if (length == 0U) {
        return 0;
    }

    uint32_t parsed_index = 0U;
    const char *after = NULL;
    if (!parse_index(bracket + 1, &parsed_index, &after)) {
        return 0;
    }

    /* `name[3]` is a row; `name[3.1]` is a slot in one. Anything else is neither. */
    enum mesh_ui_store_key_kind kind = MESH_UI_STORE_KEY_KIND_ROW;
    uint32_t parsed_slot = 0U;
    if (*after == '.') {
        if (!parse_index(after + 1, &parsed_slot, &after)) {
            return 0;
        }
        kind = MESH_UI_STORE_KEY_KIND_SLOT;
    }
    if (after[0] != ']' || after[1] != '\0') {
        return 0;
    }

    const int found = find_key(table, text, length, kind);
    if (found == 0) {
        return 0;
    }
    if (index != NULL) {
        *index = parsed_index;
    }
    if (slot != NULL) {
        *slot = parsed_slot;
    }
    return found;
}

/* Every writer funnels through here, so a key that is not in the table writes nothing at all
   rather than a line the loader would skip. */
static bool write_key(FILE *file, const struct mesh_ui_store_key_table *table, int key) {
    const char *name = mesh_ui_store_key_table_name(table, key);
    if (file == NULL || name == NULL) {
        return false;
    }
    fputs(name, file);
    return true;
}

void mesh_ui_store_key_table_vwrite(FILE *file, const struct mesh_ui_store_key_table *table,
                                    int key, const char *fmt, va_list args) {
    if (!write_key(file, table, key)) {
        return;
    }
    fputc('=', file);
    vfprintf(file, fmt, args);
    fputc('\n', file);
}

void mesh_ui_store_key_table_vwrite_row(FILE *file, const struct mesh_ui_store_key_table *table,
                                        int key, uint32_t index, const char *fmt, va_list args) {
    if (!write_key(file, table, key)) {
        return;
    }
    fprintf(file, "[%u]=", index);
    vfprintf(file, fmt, args);
    fputc('\n', file);
}

void mesh_ui_store_key_table_vwrite_slot(FILE *file, const struct mesh_ui_store_key_table *table,
                                         int key, uint32_t index, uint32_t slot, const char *fmt,
                                         va_list args) {
    if (!write_key(file, table, key)) {
        return;
    }
    fprintf(file, "[%u.%u]=", index, slot);
    vfprintf(file, fmt, args);
    fputc('\n', file);
}

void mesh_ui_store_key_table_write_text(FILE *file, const struct mesh_ui_store_key_table *table,
                                        int key, const char *text) {
    if (!write_key(file, table, key)) {
        return;
    }
    fputc('=', file);
    inkwell_record_write_escaped(file, text);
    fputc('\n', file);
}

void mesh_ui_store_key_table_write_row_text(FILE *file, const struct mesh_ui_store_key_table *table,
                                            int key, uint32_t index, const char *text) {
    if (!write_key(file, table, key)) {
        return;
    }
    fprintf(file, "[%u]=", index);
    inkwell_record_write_escaped(file, text);
    fputc('\n', file);
}

void mesh_ui_store_key_table_write_slot_text(FILE *file,
                                             const struct mesh_ui_store_key_table *table, int key,
                                             uint32_t index, uint32_t slot, const char *text) {
    if (!write_key(file, table, key)) {
        return;
    }
    fprintf(file, "[%u.%u]=", index, slot);
    inkwell_record_write_escaped(file, text);
    fputc('\n', file);
}
