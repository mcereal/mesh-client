#ifndef MESH_UI_MENU_H
#define MESH_UI_MENU_H

/*
 * The menu bar a window puts up, and the chords that go with it.
 *
 * inkcell builds the bar on a Mac and matches the chords everywhere else (inkcell/ui/sdl.h);
 * this is what is in it. An item's number is one of two things, which never overlap:
 *
 *   - below MESH_UI_COMMAND_COUNT, a command (mesh/ui/commands.h), run only when the last frame
 *     offers it - so Save is greyed out on a list exactly as the action bar leaves it off;
 *   - otherwise a click target (mesh/ui/focus.h) - a tab, the Nodes list's search chip - run as
 *     the click on it would be, and offered where that click would be answered.
 *
 * Neither is a new way to do anything: a menu item is a name for a command or a click the
 * frame already has, which is what keeps the menu bar from growing a second nav.
 */

#include "inkcell/ui/sdl.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

const struct inkcell_sdl_menu_item *mesh_ui_menu_items(size_t *count);

/* The title of each menu, by `enum inkcell_sdl_menu`. */
const inkcell_str_id *mesh_ui_menu_titles(void);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_MENU_H */
