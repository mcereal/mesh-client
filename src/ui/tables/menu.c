/*
 * The menu bar's items. See include/mesh/ui/menu.h.
 *
 * Chords are a Mac's own where it has one - Command-comma for Settings, Command-[ for Back,
 * Command-1 to 5 for the tabs as a browser's go to its tabs - and inkcell turns Command into
 * Control off a Mac. The tab items name the tabs with the tab strip's own words.
 */

#include "mesh/ui/menu.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/commands.h"
#include "mesh/ui/focus.h"
#include "mesh/ui/nav.h"

#define MESH_UI_MENU_TAB(screen) ((uint32_t)MESH_UI_FOCUS_TABS + (uint32_t)(screen))

static const struct inkcell_sdl_menu_item k_menu_items[] = {
    {INKCELL_SDL_MENU_APP, MESH_STR_MENU_SETTINGS, ',', false,
     MESH_UI_MENU_TAB(MESH_UI_SCREEN_SETTINGS)},

    {INKCELL_SDL_MENU_FILE, MESH_STR_MENU_NEW, 'n', false, MESH_UI_COMMAND_NEW},
    {INKCELL_SDL_MENU_FILE, MESH_STR_MENU_SAVE, 's', false, MESH_UI_COMMAND_SAVE},

    {INKCELL_SDL_MENU_EDIT, MESH_STR_MENU_PASTE, 'v', false, INKCELL_SDL_MENU_PASTE},
    {INKCELL_SDL_MENU_EDIT, MESH_STR_MENU_FIND, 'f', true,
     (uint32_t)MESH_UI_FOCUS_NODE_CHIPS + (uint32_t)MESH_UI_NODES_CHIP_FIND},

    {INKCELL_SDL_MENU_VIEW, MESH_STR_TAB_MESSAGES, '1', false,
     MESH_UI_MENU_TAB(MESH_UI_SCREEN_MESSAGES)},
    {INKCELL_SDL_MENU_VIEW, MESH_STR_TAB_NODES, '2', false, MESH_UI_MENU_TAB(MESH_UI_SCREEN_NODES)},
    {INKCELL_SDL_MENU_VIEW, MESH_STR_TAB_MAP, '3', false, MESH_UI_MENU_TAB(MESH_UI_SCREEN_MAP)},
    {INKCELL_SDL_MENU_VIEW, MESH_STR_TAB_RADIO, '4', false, MESH_UI_MENU_TAB(MESH_UI_SCREEN_RADIO)},
    {INKCELL_SDL_MENU_VIEW, MESH_STR_TAB_SETTINGS, '5', false,
     MESH_UI_MENU_TAB(MESH_UI_SCREEN_SETTINGS)},
    {INKCELL_SDL_MENU_VIEW, MESH_STR_MENU_REFRESH, 'r', true, MESH_UI_COMMAND_REFRESH},
    {INKCELL_SDL_MENU_VIEW, MESH_STR_MENU_ZOOM_IN, '=', true, MESH_UI_COMMAND_ZOOM_IN},
    {INKCELL_SDL_MENU_VIEW, MESH_STR_MENU_ZOOM_OUT, '-', false, MESH_UI_COMMAND_ZOOM_OUT},
    {INKCELL_SDL_MENU_VIEW, MESH_STR_MENU_FIT, '0', false, MESH_UI_COMMAND_FIT},

    {INKCELL_SDL_MENU_GO, MESH_STR_MENU_BACK, '[', false, MESH_UI_COMMAND_BACK},

    {INKCELL_SDL_MENU_HELP, MESH_STR_MENU_HELP_ITEM, '\0', false, MESH_UI_COMMAND_HELP},
};

static const inkcell_str_id k_menu_titles[INKCELL_SDL_MENU_COUNT] = {
    /* INKCELL_SDL_MENU_APP is left out: AppKit titles the application's menu with its name. */
    [INKCELL_SDL_MENU_FILE] = MESH_STR_MENU_FILE, [INKCELL_SDL_MENU_EDIT] = MESH_STR_MENU_EDIT,
    [INKCELL_SDL_MENU_VIEW] = MESH_STR_MENU_VIEW, [INKCELL_SDL_MENU_GO] = MESH_STR_MENU_GO,
    [INKCELL_SDL_MENU_HELP] = MESH_STR_MENU_HELP,
};

const struct inkcell_sdl_menu_item *mesh_ui_menu_items(size_t *count) {
    if (count != NULL) {
        *count = sizeof k_menu_items / sizeof k_menu_items[0];
    }
    return k_menu_items;
}

const inkcell_str_id *mesh_ui_menu_titles(void) { return k_menu_titles; }
