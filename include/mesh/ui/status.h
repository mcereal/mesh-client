#ifndef MESH_UI_STATUS_H
#define MESH_UI_STATUS_H

#include "mesh/i18n/strings.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * What the Status tab offers, as data.
 *
 * Status was the one screen with nothing to select: three cards reporting on the link, the mesh
 * and the radio, and no way to act on any of it. Disconnecting meant walking to the Devices tab
 * and re-reading the radio's configuration meant walking to Settings - two verbs about the
 * subject already on screen, reachable only from somewhere else.
 *
 * A card can carry a verb now (fb_widgets.h), and this is the table of which card carries
 * which. It lives here rather than beside the framebuffer backend for the reason
 * `src/ui/actions.c` does: *what a screen offers in a given state* is a fact about the nav, and
 * three things have to agree about it - nav.c, which moves the cursor over the verbs and runs
 * the one it lands on; actions.c, which names the press in the action bar; and the renderer,
 * which draws the buttons. A second opinion about the list is how those three drift.
 *
 * The list is flat, and that is the whole of the interaction model: Up and Down walk *verbs*
 * rather than cards, and a card is focused because the cursor is on one of its buttons. Left
 * and Right could not be spent here - they are the tab switch on every screen, including this
 * one - so a per-card cursor would have needed a press the Brick does not have spare.
 *
 * Because the cursor is an *index* into that list, the list may only ever grow at its end. A
 * verb appearing ahead of the cursor would change what the next A press does without the
 * cursor moving, which is the one way a screen like this can act on something nobody asked
 * for. mesh_ui_status_actions() holds to it, and says how.
 */

/* The cards, in the order the screen draws them. */
enum mesh_ui_status_card {
    MESH_UI_STATUS_CARD_LINK = 0,
    MESH_UI_STATUS_CARD_MESH,
    MESH_UI_STATUS_CARD_RADIO,
    MESH_UI_STATUS_CARD_COUNT,
};

/*
 * The verbs.
 *
 * Both are presses that already exist elsewhere, which is deliberate: this step gives a card
 * somewhere to put a verb, and a verb invented for the occasion would have been arguing two
 * things at once. Disconnect is X on the Devices tab and Refresh is X on Settings.
 */
enum mesh_ui_status_verb {
    MESH_UI_STATUS_VERB_DISCONNECT = 0, /* drop the link that is up */
    MESH_UI_STATUS_VERB_REFRESH,        /* re-read the radio's configuration */
    MESH_UI_STATUS_VERB_COUNT,
};

struct mesh_ui_status_action {
    uint8_t card; /* enum mesh_ui_status_card - which card draws it */
    uint8_t verb; /* enum mesh_ui_status_verb - what nav.c raises for it */
    enum mesh_str_id label;
};

/* Above what the table can produce, so reaching it means the screen has grown a verb rather
   than that the list ran out. */
#define MESH_UI_STATUS_ACTIONS_MAX 4U

struct mesh_ui_status_actions {
    struct mesh_ui_status_action items[MESH_UI_STATUS_ACTIONS_MAX];
    uint32_t count;
};

/*
 * The verbs on offer, in the order the cursor walks them.
 *
 * `connected` is whether a radio is attached and `synced` whether it has answered the config
 * handshake. Both are facts the store and the snapshot each hold under the same names, which is
 * why they arrive as booleans rather than as one of the two structs: this is called from nav.c
 * with a store, from actions.c with a snapshot, and from the renderer with a snapshot.
 *
 * With no radio attached the answer is nothing at all: every verb here is a request over the
 * air, and the list is append-only precisely because both of them turn on the same fact.
 */
void mesh_ui_status_actions(struct mesh_ui_status_actions *out, bool connected, bool synced);

/*
 * The verbs `card` holds: how many, and where the first of them sits in the flat list.
 *
 * The pair is what a renderer needs to hang the screen cursor on the right button, and it is
 * here rather than worked out by the renderer so that the order the buttons draw in and the
 * order the cursor walks cannot disagree. `out_first` may be NULL.
 */
uint32_t mesh_ui_status_card_actions(const struct mesh_ui_status_actions *actions,
                                     enum mesh_ui_status_card card, uint32_t *out_first);

#endif /* MESH_UI_STATUS_H */
