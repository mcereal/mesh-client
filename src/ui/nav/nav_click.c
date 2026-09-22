#define _POSIX_C_SOURCE 200809L

/*
 * A click, answered as the presses it stands for.
 *
 * The nav is a model of presses, and every rule in it - which overlay takes a key first, what
 * stands an armed delete down, which row A opens - is written against a button. A click that
 * reached into the model by a second route would need every one of those rules written twice,
 * and would be the route that forgot one. So a click is *translated*: it puts a cursor where the
 * reader pointed, which is the one thing a pointer knows that a d-pad does not, and then presses
 * the key a reader with a d-pad would have pressed on that spot.
 *
 *   - **A tab** is the shoulder pressed until that tab is up. Going through the press rather
 *     than writing `nav->screen` is what keeps the guards: a settings section with unsaved edits
 *     answers a shoulder with its question, the walk stops there, and the click has asked it.
 *   - **A row** is the cursor on it and A - one click opens, which is what a desktop list of
 *     conversations or devices does. A bubble in a thread is the exception and only selects:
 *     A there answers the message, and a reader clicking to see which message they are on has
 *     not asked to write anything.
 *   - **A dialog's answer** is the dialog's cursor on it and A.
 *
 * A click on anything that is not the top of the frame goes nowhere. The screen's rows are
 * still registered under a sheet, because the sheet was drawn over them rather than instead of
 * them; a click that reached one would be a press on a list the reader has been told is not
 * the thing in front of them.
 */

#include "mesh/ui/focus.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store.h"

#include "nav_internal.h"

#include <stddef.h>
#include <string.h>

/* Whether something is drawn over the tab's own screen that takes every press. */
static bool mesh_ui_nav_click_modal(const struct mesh_ui_nav *nav) {
    return nav->help_open || nav->verify_open || nav->confirm_open || nav->picker_open ||
           nav->keyboard_open || nav->compose_open || nav->reaction_open || nav->share_open ||
           nav->contact_open;
}

/* A shoulder, pressed until `screen` is up or a press stops moving - whichever way round the
   strip is shorter, since it wraps. */
static bool mesh_ui_nav_click_tab(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                  enum mesh_ui_screen screen, struct mesh_ui_action *out_action) {
    if (mesh_ui_nav_click_modal(nav) || nav->screen == screen) {
        return false;
    }
    const int count = (int)MESH_UI_SCREEN_COUNT;
    const int ahead = ((int)screen - (int)nav->screen + count) % count;
    const enum inkcell_key key = ahead <= count - ahead ? INKCELL_KEY_R1 : INKCELL_KEY_L1;
    bool changed = false;
    for (int i = 0; i < count && nav->screen != screen; ++i) {
        const enum mesh_ui_screen was = nav->screen;
        mesh_ui_nav_clamp(nav, store);
        changed = mesh_ui_nav_handle_key(nav, store, key, out_action) || changed;
        if (nav->screen == was) {
            break;
        }
    }
    return changed;
}

/*
 * The cursor a click on `block`'s row moves, or NULL where a click on it goes nowhere.
 * `activate` is whether the click goes on to press A.
 *
 * In the order mesh_ui_nav_handle_key() hands out presses, because the list a click may reach is
 * the list a key would have: the top one.
 */
static uint32_t *mesh_ui_nav_click_cursor(struct mesh_ui_nav *nav, uint32_t block, bool *activate) {
    *activate = true;
    if (nav->help_open || nav->verify_open || nav->confirm_open) {
        return NULL;
    }
    if (block == (uint32_t)MESH_UI_FOCUS_SHEET_ROWS) {
        if (nav->picker_open || nav->keyboard_open || nav->compose_open) {
            return NULL;
        }
        if (nav->reaction_open) {
            return &nav->reaction_cursor;
        }
        if (nav->share_open || nav->contact_open) {
            return NULL;
        }
        if (nav->node_actions_open && nav->screen == MESH_UI_SCREEN_NODES) {
            return &nav->node_actions_cursor;
        }
        return NULL;
    }
    /* The screen's own list, which the two full-screen overlays replace rather than cover. */
    if (nav->picker_open) {
        return &nav->picker_cursor;
    }
    if (nav->keyboard_open) {
        return NULL;
    }
    if (nav->compose_open) {
        return &nav->compose_cursor;
    }
    if (nav->reaction_open || nav->share_open || nav->contact_open) {
        return NULL;
    }
    if (nav->screen == MESH_UI_SCREEN_NODES && nav->node_actions_open) {
        return NULL;
    }
    /* Status walks verbs rather than rows, and the node detail walks cards: neither has an index
       a click could name, so neither registers rows. */
    if (nav->screen == MESH_UI_SCREEN_STATUS ||
        (nav->screen == MESH_UI_SCREEN_NODES && nav->node_detail_open)) {
        return NULL;
    }
    *activate = !(nav->screen == MESH_UI_SCREEN_MESSAGES && nav->thread_open);
    return &nav->cursor[nav->screen];
}

static bool mesh_ui_nav_click_row(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                  uint32_t block, uint32_t index,
                                  struct mesh_ui_action *out_action) {
    bool activate = true;
    uint32_t *const cursor = mesh_ui_nav_click_cursor(nav, block, &activate);
    if (cursor == NULL) {
        return false;
    }
    const bool screen_list =
        block == (uint32_t)MESH_UI_FOCUS_ROWS && !nav->picker_open && !nav->compose_open;
    if (screen_list && mesh_ui_nav_row_is_heading(nav, store, index)) {
        return false;
    }
    const bool moved = *cursor != index;
    *cursor = index;
    /*
     * A cursor that moved has walked off whatever row a second press was armed on - which is
     * what a d-pad step says by being a step. The click's own press is A, and A on the detail
     * that armed a delete is the key that may confirm it, so the stand-downs in
     * mesh_ui_nav_handle_key() cannot see the move: it has to be said here, before A arrives.
     */
    if (moved) {
        nav->node_remove_armed = false;
        nav->waypoint_delete_armed = false;
        nav->message_delete_armed = false;
        nav->messages_delete_armed = false;
        nav->devices_forget_armed = false;
    }
    /* The frame drew the row, so it was in range then; the store may have moved on since. */
    mesh_ui_nav_clamp(nav, store);
    if (!activate) {
        return moved;
    }
    return mesh_ui_nav_handle_key(nav, store, INKCELL_KEY_A, out_action) || moved;
}

static bool mesh_ui_nav_click_dialog(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                     uint8_t answer, struct mesh_ui_action *out_action) {
    if (nav->help_open) {
        return false;
    }
    if (nav->verify_open) {
        nav->verify_cursor = answer;
    } else if (nav->confirm_open) {
        nav->confirm_cursor = answer;
    } else {
        /* A dialog still travelling out after its answer. */
        return false;
    }
    return mesh_ui_nav_handle_key(nav, store, INKCELL_KEY_A, out_action);
}

bool mesh_ui_nav_handle_click(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                              uint32_t target, struct mesh_ui_action *out_action) {
    if (out_action != NULL) {
        memset(out_action, 0, sizeof *out_action);
    }
    if (nav == NULL || store == NULL) {
        return false;
    }
    if (target == (uint32_t)MESH_UI_FOCUS_DIALOG || target == (uint32_t)MESH_UI_FOCUS_DIALOG + 1U) {
        return mesh_ui_nav_click_dialog(nav, store, (uint8_t)(target - MESH_UI_FOCUS_DIALOG),
                                        out_action);
    }
    if (target >= (uint32_t)MESH_UI_FOCUS_TABS &&
        target < (uint32_t)MESH_UI_FOCUS_TABS + (uint32_t)MESH_UI_SCREEN_COUNT) {
        return mesh_ui_nav_click_tab(
            nav, store, (enum mesh_ui_screen)(target - (uint32_t)MESH_UI_FOCUS_TABS), out_action);
    }
    const uint32_t blocks[] = {(uint32_t)MESH_UI_FOCUS_ROWS, (uint32_t)MESH_UI_FOCUS_SHEET_ROWS};
    for (size_t b = 0; b < sizeof blocks / sizeof blocks[0]; ++b) {
        if (target >= blocks[b] && target < blocks[b] + MESH_UI_FOCUS_BLOCK) {
            return mesh_ui_nav_click_row(nav, store, blocks[b], target - blocks[b], out_action);
        }
    }
    return false;
}
