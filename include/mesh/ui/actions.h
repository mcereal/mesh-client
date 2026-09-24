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
#include "inkcell/ui/icon.h"

#include "mesh/ui/commands.h"

#include <stdbool.h>
#include <stddef.h>

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

/*
 * Takes "L/R tabs" out of `bar`, for a frame drawn for a pointer: the tabs are clicked there, and
 * the entry is the one on every bar that says nothing about this screen. The shoulders' other
 * jobs - the keyboard's panels, the picker's jump - stay, since nothing else on those screens
 * does them.
 */
void mesh_ui_actions_drop_tabs(struct inkcell_action_bar *bar);

/*
 * The verbs a pointer finds in the heading rather than as keycaps at the foot.
 *
 * A window with a mouse has no use for "X pin" printed under the panel: a keycap is what a d-pad
 * reader is told, and a pointer reader wants the verb itself where every desktop app keeps it.
 * So the same command set the bar is projected from is projected a second time, into the app
 * bar's actions - one answer, two presentations, and the two cannot offer different things.
 *
 * Not every command belongs there, and the rule is the symbol. A command with no entry in the
 * icon table below stays off the heading, which is what keeps out the ones a pointer already has
 * somewhere better: back (the arrow), the tabs (clicked), a row's own A (the row is clicked), the
 * paired moves (the wheel, the controls themselves), and the keyboard's keys (typed). Help is
 * always last and never the emphasized one - it is about the screen, not what the screen is for.
 */
struct mesh_ui_heading_action {
    enum mesh_ui_command_id id;
    inkcell_str_id label;
    enum inkcell_icon icon;
    /* A verb that throws something away, drawn in the error family when it is the one
       emphasized. */
    bool destructive;
    /* A verb a screen can be *for* - one that makes, sends or keeps something. The first of
       these on a heading is its tonal pill; a heading with none has no pill, which is better than
       dressing "pin" up as the reason a node's detail was opened. */
    bool primary;
};

#define MESH_UI_HEADING_ACTIONS_MAX 6U

/* The symbol `id` is drawn with in the heading, or INKCELL_ICON_NONE for a command that is not
   offered there. The controller asks the same question before it acts on a click, so a verb the
   heading could not have drawn cannot be reached through it. */
enum inkcell_icon mesh_ui_command_icon(enum mesh_ui_command_id id);

/* Fills `out` with at most `max` heading verbs for `snapshot`, in the command set's priority
   order with help moved to the end, and returns how many. */
size_t mesh_ui_actions_heading(const struct mesh_ui_snapshot *snapshot,
                               struct mesh_ui_heading_action *out, size_t max);

#endif /* MESH_UI_ACTIONS_H */
