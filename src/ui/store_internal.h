#ifndef MESH_UI_STORE_INTERNAL_H
#define MESH_UI_STORE_INTERNAL_H

/*
 * The seam between store.c, the cache file split out of it, and the archive beside that.
 *
 * `store.c` is the state machine: what the store holds, what moved, and who gets told. The file
 * on the card is a different subject with a different audience - it is a *format*, with
 * compatibility rules of its own (see include/mesh/ui/store_keys.def), and it had grown to
 * roughly half of store.c without ever needing anything from the other half. It is now
 * `store_file.c`, over the first call below.
 *
 * `store_archive.c` is the third file in the group and the reason the rest of this header
 * exists: it writes a *second* format made of the same records (mesh/ui/store_archive.h says
 * why there are two), so the five lines a message is spelled as have to be written and read in
 * one place or they are two formats sharing a name. That place is store_file.c, which already
 * held both halves.
 *
 * Nothing here is public - that is include/mesh/ui/store.h, which already declares
 * mesh_ui_store_save() and mesh_ui_store_load(). This is what would still be `static` if the
 * three were one translation unit, so keep the list short.
 */

#include "mesh/ui/store.h"
#include "mesh/ui/store_keys.h"

#include <stdio.h>

/*
 * Raise `flags` and wake the loop.
 *
 * The one thing loading a cache needs from the store proper: a roster read off the card has to
 * reach the backends the same way one off the air does, or the first frame is drawn from a
 * store nobody was told had changed.
 */
void mesh_ui_store_mark_dirty(struct mesh_ui_store *store, mesh_ui_update_flags flags);

/*
 * One message as five keyed lines, and the mirror that reads them back a line at a time.
 *
 * `index` groups the five and carries no other meaning - see the writer's own comment. The
 * reader returns true for the msg[] line that opens a record, and tolerates a NULL `message`,
 * so a caller bounds-checks its destination once per line rather than once per field.
 */
void mesh_ui_store_write_message(FILE *file, uint32_t index, const struct mesh_ui_message *message);
bool mesh_ui_store_key_is_message(enum mesh_ui_store_key key);
bool mesh_ui_store_read_message_line(struct mesh_ui_message *message, enum mesh_ui_store_key key,
                                     const char *value);

#endif /* MESH_UI_STORE_INTERNAL_H */
