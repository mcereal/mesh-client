#ifndef MESH_UI_STORE_KEY_TABLE_H
#define MESH_UI_STORE_KEY_TABLE_H

/*
 * A table of keys, and the two directions a line's key is read in: spelled out by a writer, and
 * looked up by a loader.
 *
 * This is the mechanism under store_keys.h with the keys taken out. A key is an index into a
 * table the caller hands in, so the rules here - what brackets a key carries, what a loader
 * refuses, how a writer escapes - hold for any file in the `key=value` format inkwell's
 * record_file.h reads, and name none of the keys in any of them. The keys are the caller's, and
 * so is the enum they are indexed by.
 *
 * Index 0 is reserved and is never matched: it is what a line the table does not know reads as,
 * and it is where a caller's `NONE` sits in the enum the table is indexed by. Its row may be
 * anything; `{NULL, 0, PLAIN}` is the natural spelling.
 */

#include "inkwell/base/log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a key carries between its brackets, if anything. */
enum mesh_ui_store_key_kind {
    MESH_UI_STORE_KEY_KIND_PLAIN = 0, /* name=          */
    MESH_UI_STORE_KEY_KIND_ROW,       /* name[3]=       */
    MESH_UI_STORE_KEY_KIND_SLOT,      /* name[3.1]=     */
};

/* One key: its spelling on disk, that spelling's length, and what it carries. */
struct mesh_ui_store_key_row {
    const char *name;
    size_t length;
    enum mesh_ui_store_key_kind kind;
};

/*
 * A row, from a string literal.
 *
 * `length` is taken with sizeof on the literal rather than written out, which is the point: the
 * loader used to carry a hand-counted length beside every prefix it matched, and a miscount
 * there is a key that silently never matches.
 */
#define MESH_UI_STORE_KEY_ROW(literal, kind_)                                                      \
    { (literal), sizeof(literal) - 1U, (kind_) }

/* The rows, indexed by the caller's key. `count` includes the reserved row 0. */
struct mesh_ui_store_key_table {
    const struct mesh_ui_store_key_row *rows;
    size_t count;
};

/* The key's spelling on disk. NULL for 0 and for anything out of range. */
const char *mesh_ui_store_key_table_name(const struct mesh_ui_store_key_table *table, int key);

/* What the key carries. PLAIN for 0 and for anything out of range. */
enum mesh_ui_store_key_kind
mesh_ui_store_key_table_kind(const struct mesh_ui_store_key_table *table, int key);

/*
 * Read a key off a line.
 *
 * `text` is the text left of the '=', already unescaped. On a row or slot key the indices are
 * written through `index` and `slot`; on a plain key both are left at zero. Either pointer may
 * be NULL if the caller does not want it.
 *
 * Returns 0 for a key the table does not know, and for one whose brackets do not parse. A line
 * that is not exactly `name`, `name[i]` or `name[i.n]` is not half a record: the file is text a
 * user can edit, so it is an ingress, and the lookup is exact in both directions. A name may
 * appear twice with two kinds - a count and the rows it counts - so the brackets are part of
 * what is matched, not decoration on it.
 */
int mesh_ui_store_key_table_lookup(const struct mesh_ui_store_key_table *table, const char *text,
                                   uint32_t *index, uint32_t *slot);

/*
 * The writers. Each spells its key through the table, so a key's text appears once, in the
 * table, and a key the table does not have writes nothing at all rather than a line the loader
 * would skip.
 *
 * The `_text` three escape their value through inkwell_record_write_escaped(); everything below
 * 0x20, plus '\' and '=', goes out as \xNN so a value carrying a newline cannot forge a second
 * line. The formatted three take their value already formatted and do not escape it, which is
 * why they should only ever be handed numbers. Each takes a va_list so a caller's own printf-like
 * wrapper can forward to it.
 */
void mesh_ui_store_key_table_vwrite(FILE *file, const struct mesh_ui_store_key_table *table,
                                    int key, const char *fmt, va_list args)
    __attribute__((format(INKWELL_PRINTF_ARCHETYPE, 4, 0)));
void mesh_ui_store_key_table_vwrite_row(FILE *file, const struct mesh_ui_store_key_table *table,
                                        int key, uint32_t index, const char *fmt, va_list args)
    __attribute__((format(INKWELL_PRINTF_ARCHETYPE, 5, 0)));
void mesh_ui_store_key_table_vwrite_slot(FILE *file, const struct mesh_ui_store_key_table *table,
                                         int key, uint32_t index, uint32_t slot, const char *fmt,
                                         va_list args)
    __attribute__((format(INKWELL_PRINTF_ARCHETYPE, 6, 0)));
void mesh_ui_store_key_table_write_text(FILE *file, const struct mesh_ui_store_key_table *table,
                                        int key, const char *text);
void mesh_ui_store_key_table_write_row_text(FILE *file, const struct mesh_ui_store_key_table *table,
                                            int key, uint32_t index, const char *text);
void mesh_ui_store_key_table_write_slot_text(FILE *file,
                                             const struct mesh_ui_store_key_table *table, int key,
                                             uint32_t index, uint32_t slot, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_STORE_KEY_TABLE_H */
