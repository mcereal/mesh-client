#ifndef MESH_UI_STORE_KEYS_H
#define MESH_UI_STORE_KEYS_H

/*
 * The keys of the handshake cache, as one table both halves of the format read.
 *
 * The cache is the file the client writes at shutdown and reads at launch, and it is the reason
 * a Brick with no radio in range still opens on a roster. Its format used to be spelled twice -
 * once in mesh_ui_store_save() as a printf literal, once in mesh_ui_store_load() as a strcmp
 * against the same text next to a hand-counted prefix length. Nothing checked that the two
 * agreed, and two keys had already drifted into being written every save and read by nobody.
 *
 * The table is include/mesh/ui/store_keys.def, one line per key. Adding a key is a line there,
 * a writer call, and a case in the loader's switch - which has no `default`, so -Wswitch names
 * the case you forgot. What the switch cannot see, the ui_store_cache_keys_round_trip test does:
 * it walks this enum and holds every key to being written and read.
 *
 * See docs/ui.md for what the cache is for and what it deliberately does not hold.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Every key in the cache. MESH_UI_STORE_KEY_NONE is what a line the format does not know reads
 * as - a cache written by a newer build, or edited by hand - and the loader skips it.
 */
enum mesh_ui_store_key {
    MESH_UI_STORE_KEY_NONE = 0,
#define MESH_STORE_KEY_ENTRY(id, name) MESH_UI_STORE_KEY_##id,
#define MESH_STORE_KEY_ROW(id, name) MESH_UI_STORE_KEY_##id,
#define MESH_STORE_KEY_SLOT(id, name) MESH_UI_STORE_KEY_##id,
#include "mesh/ui/store_keys.def"
#undef MESH_STORE_KEY_ENTRY
#undef MESH_STORE_KEY_ROW
#undef MESH_STORE_KEY_SLOT
    MESH_UI_STORE_KEY_COUNT
};

/* What a key carries between its brackets, if anything. */
enum mesh_ui_store_key_kind {
    MESH_UI_STORE_KEY_KIND_PLAIN = 0, /* name=          */
    MESH_UI_STORE_KEY_KIND_ROW,       /* name[3]=       */
    MESH_UI_STORE_KEY_KIND_SLOT,      /* name[3.1]=     */
};

/* The key's spelling on disk. NULL for NONE and for anything out of range. */
const char *mesh_ui_store_key_name(enum mesh_ui_store_key key);

/* What the key carries. PLAIN for NONE and for anything out of range. */
enum mesh_ui_store_key_kind mesh_ui_store_key_kind(enum mesh_ui_store_key key);

/*
 * Read a key off a cache line.
 *
 * `key` is the text left of the '=', already unescaped. On a row or slot key the indices are
 * written through `index` and `slot`; on a plain key both are left at zero. Either pointer may
 * be NULL if the caller does not want it.
 *
 * Returns MESH_UI_STORE_KEY_NONE for a key this build does not know, and for one whose brackets
 * do not parse. That last part is stricter than the sscanf() it replaced, deliberately: the
 * cache is a text file on a card the user can edit, so it is an ingress like the air is, and a
 * line that is not exactly `name`, `name[i]` or `name[i.n]` is not half a record.
 */
enum mesh_ui_store_key mesh_ui_store_key_lookup(const char *key, uint32_t *index, uint32_t *slot);

/*
 * The writers. Each spells its key through the table, so the only place a key's text appears is
 * store_keys.def.
 *
 * The `_text` pair escape their value; everything below 0x20, plus '\' and '=', goes out as
 * \xNN so a name carrying a newline cannot forge a second line. The printf-style three take
 * their value already formatted and do not escape it, which is why they are only ever handed
 * numbers.
 */
void mesh_ui_store_write(FILE *file, enum mesh_ui_store_key key, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void mesh_ui_store_write_row(FILE *file, enum mesh_ui_store_key key, uint32_t index,
                             const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void mesh_ui_store_write_slot(FILE *file, enum mesh_ui_store_key key, uint32_t index, uint32_t slot,
                              const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void mesh_ui_store_write_text(FILE *file, enum mesh_ui_store_key key, const char *text);
void mesh_ui_store_write_row_text(FILE *file, enum mesh_ui_store_key key, uint32_t index,
                                  const char *text);

#endif /* MESH_UI_STORE_KEYS_H */
