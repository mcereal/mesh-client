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
 * The mechanism under the table - what a key's brackets carry, what the lookup refuses, how a
 * writer escapes - is inkstand's persist/keys.h, which names none of these keys. Each function
 * below is that one over this table.
 *
 * See docs/ui.md for what the cache is for and what it deliberately does not hold.
 */

#include "inkstand/persist/keys.h"
#include "inkwell/base/log.h"

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

/* The key's spelling on disk. NULL for NONE and for anything out of range. */
const char *mesh_ui_store_key_name(enum mesh_ui_store_key key);

/* What the key carries. PLAIN for NONE and for anything out of range. */
enum inkstand_key_kind mesh_ui_store_key_kind(enum mesh_ui_store_key key);

/*
 * Whether this key is one the handshake cache writes.
 *
 * Almost all of them are, and the table above reads as though all of them were - which is the
 * point of stating the exceptions in one function rather than in a comment on each. A key here
 * may belong to one of the other files that share this format: the trend log writes `trend` and
 * the cache never does. See include/mesh/ui/store_keys.def.
 *
 * Two callers, and they ask it for the same reason from opposite sides: the cache's loader,
 * whose switch has no `default` and so has to say something about every key, and the
 * round-trip test, which walks this enum holding every key to being written and read back.
 */
bool mesh_ui_store_key_in_cache(enum mesh_ui_store_key key);

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
    __attribute__((format(INKWELL_PRINTF_ARCHETYPE, 3, 4)));
void mesh_ui_store_write_row(FILE *file, enum mesh_ui_store_key key, uint32_t index,
                             const char *fmt, ...)
    __attribute__((format(INKWELL_PRINTF_ARCHETYPE, 4, 5)));
void mesh_ui_store_write_slot(FILE *file, enum mesh_ui_store_key key, uint32_t index, uint32_t slot,
                              const char *fmt, ...)
    __attribute__((format(INKWELL_PRINTF_ARCHETYPE, 5, 6)));
void mesh_ui_store_write_text(FILE *file, enum mesh_ui_store_key key, const char *text);
void mesh_ui_store_write_row_text(FILE *file, enum mesh_ui_store_key key, uint32_t index,
                                  const char *text);
void mesh_ui_store_write_slot_text(FILE *file, enum mesh_ui_store_key key, uint32_t index,
                                   uint32_t slot, const char *text);

/*
 * The mirror of that escape, in place.
 *
 * A \xNN goes back to the byte it stood for and everything else is copied through; the result
 * is never longer than the input, so the line buffer the loader read into is the buffer it is
 * written back to. A trailing backslash that cannot be a whole escape is dropped, which is
 * what a truncated line leaves behind.
 */
void mesh_ui_store_unescape_value(char *value);

#endif /* MESH_UI_STORE_KEYS_H */
