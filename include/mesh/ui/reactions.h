#pragma once

#include "mesh/i18n/strings.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The tapbacks the React picker offers.
 *
 * A fixed table rather than the canned list's file-backed one, and that is the difference
 * between the two: a canned message is a sentence a user might want in their own words, while a
 * tapback is a glyph every other client on the mesh has to recognise on sight. Nothing here is
 * a client setting; the set is small on purpose, because a picker the cursor has to walk is not
 * a keyboard.
 *
 * The emoji is the payload - it goes on the air unchanged and is never translated - and the
 * label is what the row beside it says, because eight faces in a column at this glyph scale are
 * not eight distinguishable things.
 */
size_t mesh_ui_reaction_count(void);

/* The emoji itself, as UTF-8, or "" when the index is past the end. */
const char *mesh_ui_reaction_emoji(size_t index);

/* What that emoji means, as a catalog id. MESH_STR_NONE past the end. */
enum mesh_str_id mesh_ui_reaction_label(size_t index);

#ifdef __cplusplus
}
#endif
