#ifndef MESH_UI_DIALOG_H
#define MESH_UI_DIALOG_H

/*
 * A two-answer question put over the screen: go ahead, or don't.
 *
 * What the question is *about* - which save, which irreversible verb - is the application's, and
 * so is what going ahead does. This is the rest, which every such question has the same way:
 *
 *   - **It opens on Cancel.** The press that raised the question is usually the press that will
 *     be repeated by a reader who has not read it yet, and a repeated press must change nothing.
 *
 *   - **It is modal.** The cursor moves between its two answers and nowhere else, however close a
 *     box dimmed behind it sits to one of them.
 *
 *   - **An answer closes it.** Go ahead or cancel, the question is gone; what remains is what the
 *     caller does about the answer.
 *
 * The struct is a plain value, so a snapshot of whatever holds it is a copy. Nothing allocates.
 */

#include "inkcell/ui/focus.h"
#include "inkcell/ui/key.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The two answers, as the cursor holds them. The leading one goes ahead. */
#define MESH_UI_DIALOG_ACCEPT 0U
#define MESH_UI_DIALOG_CANCEL 1U

struct mesh_ui_dialog {
    bool open;
    /* MESH_UI_DIALOG_ACCEPT or MESH_UI_DIALOG_CANCEL. */
    uint8_t cursor;
    /* What the question is about, in the application's terms; 0 when it is about nothing more than
       the screen it was raised over. The dialog carries it and never reads it. */
    uint16_t subject;
};

/* What a press did to an open dialog. */
enum mesh_ui_dialog_press {
    MESH_UI_DIALOG_IGNORED = 0, /* not a key a dialog answers, or a move with nowhere to go */
    MESH_UI_DIALOG_MOVED,       /* the cursor went to the other answer */
    MESH_UI_DIALOG_ACCEPTED,    /* go ahead; the dialog has closed */
    MESH_UI_DIALOG_CANCELLED,   /* don't; the dialog has closed */
};

/* Puts the question up, about `subject`, with the cursor on Cancel. */
void mesh_ui_dialog_open(struct mesh_ui_dialog *dialog, uint16_t subject);

/* Takes it down and forgets its subject. */
void mesh_ui_dialog_close(struct mesh_ui_dialog *dialog);

/*
 * Where a direction key takes the cursor from `cursor`, over a frame's focus map in which the two
 * answers are `focus_base` and `focus_base + 1`.
 *
 * Resolved inside the dialog rather than against the whole frame: a box behind it can sit nearer
 * to an answer than the other answer does, and a general finder would choose it. The two answers
 * are the whole focus world while the question is up. With no map yet, or a map from the frame
 * before the dialog opened, any direction simply goes to the other answer. A direction with no
 * answer that way leaves the cursor where it is - the edge of the dialog - rather than wrapping.
 */
uint8_t mesh_ui_dialog_answer(const struct inkcell_focus_map *map, uint32_t focus_base,
                              enum inkcell_key key, uint8_t cursor);

/*
 * A key pressed over an open dialog. Direction keys move between the answers; A and START answer
 * with the one under the cursor; B cancels. An answer closes the dialog, and the subject it was
 * about is written to `out_subject` first - so the caller acts on ACCEPTED knowing what was
 * accepted.
 */
enum mesh_ui_dialog_press mesh_ui_dialog_key(struct mesh_ui_dialog *dialog,
                                             const struct inkcell_focus_map *map,
                                             uint32_t focus_base, enum inkcell_key key,
                                             uint16_t *out_subject);

#ifdef __cplusplus
}
#endif

#endif
