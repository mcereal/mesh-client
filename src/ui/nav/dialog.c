/*
 * A two-answer question over the screen - see mesh/ui/dialog.h.
 */

#include "mesh/ui/dialog.h"

#include <stddef.h>

void mesh_ui_dialog_open(struct mesh_ui_dialog *dialog, uint16_t subject) {
    if (dialog == NULL) {
        return;
    }
    dialog->open = true;
    dialog->cursor = MESH_UI_DIALOG_CANCEL; /* so a repeated press changes nothing */
    dialog->subject = subject;
}

void mesh_ui_dialog_close(struct mesh_ui_dialog *dialog) {
    if (dialog == NULL) {
        return;
    }
    dialog->open = false;
    dialog->subject = 0U;
}

uint8_t mesh_ui_dialog_answer(const struct inkcell_focus_map *map, uint32_t focus_base,
                              enum inkcell_key key, uint8_t cursor) {
    const uint8_t here = (uint8_t)(cursor == 0U ? 0U : 1U);
    const uint8_t other = (uint8_t)(here == 0U ? 1U : 0U);
    enum inkcell_focus_dir dir;
    const uint32_t here_id = focus_base + here;
    /*
     * A backend can already have handed over a perfectly good map from the frame under a dialog
     * which has only just opened.  That is not the same thing as a map of the
     * dialog: neither answer is in it yet.  Treat it like the no-map case and keep the old
     * toggle fallback, otherwise the safe default (Cancel) becomes a trap until another frame
     * happens to replace the map.
     */
    if (map == NULL || !inkcell_focus_has(map, here_id) || !inkcell_focus_dir_for_key(key, &dir)) {
        return other;
    }
    /*
     * Resolve inside the modal, not against every box dimmed behind it. A wide row behind it can
     * cross the same horizontal band as these buttons; the general finder then quite
     * reasonably chooses that nearer rectangle, and the range guard below quite reasonably
     * refuses to let a modal cursor land there. Together those two correct local decisions
     * trap the cursor on Cancel. A two-item map states the missing fact: while this question is
     * up, its two answers are the whole focus world.
     */
    struct inkcell_focus_item items[2];
    struct inkcell_focus_map dialog;
    inkcell_focus_begin(&dialog, items, 2U);
    for (uint8_t answer = 0U; answer < 2U; ++answer) {
        const uint32_t id = focus_base + answer;
        struct inkcell_focus_rect rect;
        if (inkcell_focus_rect_of(map, id, &rect)) {
            (void)inkcell_focus_add(&dialog, id, rect.x, rect.y, rect.w, rect.h);
        }
    }
    const uint32_t to = inkcell_focus_find(&dialog, here_id, dir);
    if (to == INKCELL_FOCUS_NONE) {
        /*
         * Nothing that way. On a dialog that is the edge of the panel and the press goes spare,
         * which is the right answer and not the old one: Left from the leading button used to
         * land on the trailing one, so a reader holding Left saw the cursor shuttling between
         * two answers rather than resting on the one they had reached.
         */
        return here;
    }
    if (to < focus_base || to > focus_base + 1U) {
        /* Something else on the frame. A dialog is modal, so the cursor does not leave it. */
        return here;
    }
    return (uint8_t)(to - focus_base);
}

enum mesh_ui_dialog_press mesh_ui_dialog_key(struct mesh_ui_dialog *dialog,
                                             const struct inkcell_focus_map *map,
                                             uint32_t focus_base, enum inkcell_key key,
                                             uint16_t *out_subject) {
    if (dialog == NULL || !dialog->open) {
        return MESH_UI_DIALOG_IGNORED;
    }
    switch (key) {
    case INKCELL_KEY_UP:
    case INKCELL_KEY_DOWN:
    case INKCELL_KEY_LEFT:
    case INKCELL_KEY_RIGHT: {
        const uint8_t to = mesh_ui_dialog_answer(map, focus_base, key, dialog->cursor);
        if (to == dialog->cursor) {
            return MESH_UI_DIALOG_IGNORED;
        }
        dialog->cursor = to;
        return MESH_UI_DIALOG_MOVED;
    }
    case INKCELL_KEY_A:
    case INKCELL_KEY_START: {
        const bool accepted = dialog->cursor == MESH_UI_DIALOG_ACCEPT;
        if (out_subject != NULL) {
            *out_subject = dialog->subject;
        }
        mesh_ui_dialog_close(dialog);
        return accepted ? MESH_UI_DIALOG_ACCEPTED : MESH_UI_DIALOG_CANCELLED;
    }
    case INKCELL_KEY_B:
        if (out_subject != NULL) {
            *out_subject = dialog->subject;
        }
        mesh_ui_dialog_close(dialog);
        return MESH_UI_DIALOG_CANCELLED;
    default:
        return MESH_UI_DIALOG_IGNORED;
    }
}
