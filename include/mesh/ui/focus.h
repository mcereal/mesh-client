#ifndef MESH_UI_FOCUS_H
#define MESH_UI_FOCUS_H

/*
 * What this client's d-pad calls the things a frame draws.
 *
 * inkcell/ui/focus.h is the model - a map of the rectangles a frame registered, and which one
 * lies in a given direction. This is the vocabulary over it, and it is here rather than beside
 * the renderer because both halves of a press need it: the backend registers a box under an id
 * while it draws, and the nav asks what that id's neighbour is when the button arrives. An id
 * known to only one of them would be a press answered against a name nobody else uses.
 *
 * A focus id names a *pressable box*, which is not the space a route level or a layer id lives
 * in: those name something that persists, and these name what came out on the panel this frame.
 * The map is rebuilt from nothing every frame, which is the whole of what makes it honest - a
 * verb a card had no room for is registered nowhere, so the cursor cannot land on it.
 *
 * The blocks are a base plus an index wherever a component has more than one of something,
 * which is how inkcell_fb_list_focus() numbers a list's rows and how a dialog numbers its two
 * answers.
 */

#include "inkcell/ui/focus.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mesh_ui_focus_id {
    /* A dialog's answers, in the dialog's own numbering: 0 is accept and 1 is cancel, which is
       what nav.confirm_cursor and nav.verify_cursor already carry. */
    MESH_UI_FOCUS_DIALOG = 1,
    /* A right-click menu's verbs, `base + enum inkcell_button` - the button, not the row, so a
       click on one names the press it stands for without the nav keeping the menu's contents. */
    MESH_UI_FOCUS_MENU = 0x0400,
    /* Everything under an open menu, as one target: a click that lands off the menu puts it
       down rather than reaching whatever is drawn beneath. */
    MESH_UI_FOCUS_MENU_DISMISS = 0x04FF,
    /* The tab strip, `base + enum mesh_ui_screen`. */
    MESH_UI_FOCUS_TABS = 0x0800,
    /* The rows of the list the screen drew, `base + index` - the index the list's own cursor
       holds. One block, because one screen list is drawn per frame and an index means nothing
       outside the list it belongs to. */
    MESH_UI_FOCUS_ROWS = 0x1000,
    /* The rows of a sheet drawn over it - the tapbacks, a node's verbs. A block of their own,
       because the list under a sheet is still on the frame and still registered, and a click
       has to say which of the two it landed on. */
    MESH_UI_FOCUS_SHEET_ROWS = 0x2000,
};

/* How many ids one block holds: a list longer than this registers only its first rows. */
#define MESH_UI_FOCUS_BLOCK 0x1000U

/* How many boxes one frame may register. A list registers only the rows in its window, so this
   is a panel's worth of rows and the chrome around them rather than a count of anything the
   client holds. */
#define MESH_UI_FOCUS_MAX 128U

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_FOCUS_H */
