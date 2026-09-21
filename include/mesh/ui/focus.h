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
    /* The rows of whichever list this frame drew, `base + index`. One block, because one list
       is drawn per frame and an index means nothing outside the list it belongs to. */
    MESH_UI_FOCUS_ROWS = 0x1000,
};

/* How many boxes one frame may register. A list registers only the rows in its window, so this
   is a panel's worth of rows and the chrome around them rather than a count of anything the
   client holds. */
#define MESH_UI_FOCUS_MAX 128U

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_FOCUS_H */
