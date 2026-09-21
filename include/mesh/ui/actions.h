#ifndef MESH_UI_ACTIONS_H
#define MESH_UI_ACTIONS_H

/*
 * Which verbs the open screen offers, as (button, string id) pairs.
 *
 * The bar itself is inkcell's (inkcell/ui/actions.h) - a row of keycaps and labels is not about
 * Meshtastic. What is here is the one question only this client can answer: given a snapshot,
 * which verbs are on offer. The table is src/ui/tables/actions.c.
 */

#include "inkcell/ui/actions.h"

#include <stdbool.h>

struct mesh_ui_snapshot;

/* Fills `out` with the bar for `snapshot`. The back arrow in the heading is derived from what
   this writes, never declared separately - see docs/ui.md. */
void mesh_ui_actions_for(const struct mesh_ui_snapshot *snapshot, struct inkcell_action_bar *out);

/*
 * Whether `bar` offers a way out - what the top app bar's leading arrow is derived from, and the
 * one question about a bar that something other than the bar asks.
 *
 * Answered from the table rather than by the screen renderers because the table is already the
 * place that decides it, and a second opinion is how a screen that grows a press ends up with
 * two places to remember it. Reading it off the bar also makes the arrow exactly as conditional
 * as the press is, which a flag on a screen would not be: a settings section with edits pending
 * offers B as *discard*, not as back, and it is right that no arrow appears there.
 *
 * It lives here rather than in inkcell because the answer is "does this bar carry
 * MESH_STR_ACTION_BACK", and which verb means leave is a row in this client's catalog.
 */
bool mesh_ui_action_bar_goes_back(const struct inkcell_action_bar *bar);

#endif /* MESH_UI_ACTIONS_H */
