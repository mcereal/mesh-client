#ifndef MESH_SHIM_UI_ACTIONS_H
#define MESH_SHIM_UI_ACTIONS_H

/* Moved to inkcell (third_party/inkcell). This is the old path, kept so the layers above did
   not all have to change in the commit that moved the file. Include the inkcell header
   directly in new code. */

#include "inkcell/ui/actions.h"

struct mesh_ui_snapshot;

/*
 * What the buttons do, for the state this snapshot is in.
 *
 * inkcell says what a bar is made of - a keycap, a verb, the order they are offered in - and
 * stops there: which press means what on a given screen is a fact about *this* client's
 * navigation. So the table is here, in src/ui/tables/actions.c, and this is the one entry point
 * into it.
 *
 * The overlays win over the screen beneath them, in the order they stack: a confirmation over a
 * picker over the keyboard over the compose sheet, then the tab's own screen. That is the same
 * chain fb_render_snapshot() walks to decide what to draw, and it is the same chain because a
 * bar describing a screen the user cannot reach is worse than no bar.
 *
 * `out` is fully overwritten; a NULL snapshot yields an empty bar rather than a default one,
 * because there is no state to be describing.
 */
void mesh_ui_actions_for(const struct mesh_ui_snapshot *snapshot, struct inkcell_action_bar *out);

#endif /* MESH_SHIM_UI_ACTIONS_H */
