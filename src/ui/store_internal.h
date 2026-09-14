#ifndef MESH_UI_STORE_INTERNAL_H
#define MESH_UI_STORE_INTERNAL_H

/*
 * The seam between store.c and the cache file split out of it.
 *
 * `store.c` is the state machine: what the store holds, what moved, and who gets told. The file
 * on the card is a different subject with a different audience - it is a *format*, with
 * compatibility rules of its own (see include/mesh/ui/store_keys.def), and it had grown to
 * roughly half of store.c without ever needing anything from the other half. It is now
 * `store_file.c`, over the one call below.
 *
 * Nothing here is public - that is include/mesh/ui/store.h, which already declares
 * mesh_ui_store_save() and mesh_ui_store_load(). This is what would still be `static` if the
 * two were one translation unit, so keep the list short.
 */

#include "mesh/ui/store.h"

/*
 * Raise `flags` and wake the loop.
 *
 * The one thing loading a cache needs from the store proper: a roster read off the card has to
 * reach the backends the same way one off the air does, or the first frame is drawn from a
 * store nobody was told had changed.
 */
void mesh_ui_store_mark_dirty(struct mesh_ui_store *store, mesh_ui_update_flags flags);

#endif /* MESH_UI_STORE_INTERNAL_H */
