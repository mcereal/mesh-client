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
 * **The cursor is a verb, not a position, and that is a change from what shipped here.** It was
 * an index into this list, and an index made the list append-only: a verb appearing *ahead* of
 * the cursor changed what the next A press did without the cursor moving, so a client holding a
 * cached configuration and offering refresh alone would have had disconnect slide in underneath
 * a cursor still sitting on index 0. Every entry was therefore gated on the entries before it,
 * which cost the table two conditions that were not the honest ones and cost the *screen* its
 * reading order - Mesh is the middle card and its verb had to go last, so Down walked Link,
 * Radio, Mesh and the reader watched the highlight jump to the bottom of the screen and then
 * back up it.
 *
 * `nav->status_verb` names the verb instead. What the cursor is on survives a verb appearing or
 * disappearing anywhere in the list, which buys three things at once: the table is written in
 * the order the cards draw, a verb states its own condition rather than restating its
 * neighbours', and a link that drops and comes back leaves the reader on the verb they were on.
 * mesh_ui_status_verb_resolve() is the whole of what makes it work.
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
 * The first two are presses that already exist elsewhere, which was deliberate: the step that
 * gave a card somewhere to put a verb would have been arguing two things at once if it had also
 * invented one. Disconnect is X on the Devices tab and Refresh is X on Settings.
 *
 * Trend is the first that exists nowhere else, and it is a verb rather than a row because the
 * Status screen has no rows to press: the cursor here walks card buttons, so "open the airtime
 * readings as a chart" had nowhere else to be. It is also the only one that is not a request
 * over the air - what it opens is what this client has already watched - and it is now offered
 * on exactly that condition, which it could not be while the cursor was an index.
 *
 * The order here is the enum's own and means nothing. What the cursor walks is the order the
 * table in status.c is written in, which is the order the cards draw.
 */
enum mesh_ui_status_verb {
    MESH_UI_STATUS_VERB_DISCONNECT = 0, /* drop the link that is up */
    MESH_UI_STATUS_VERB_REFRESH,        /* re-read the radio's configuration */
    MESH_UI_STATUS_VERB_TREND,          /* open the airtime history as a chart */
    MESH_UI_STATUS_VERB_COUNT,          /* and "no verb", which is what an empty screen holds */
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
 * `connected` is whether a radio is attached, `synced` whether it has answered the config
 * handshake, and `has_trend` whether there are two airtime readings to draw a line between.
 * All three are facts the store and the snapshot each hold under the same names, which is why
 * they arrive as booleans rather than as one of the two structs: this is called from nav.c with
 * a store, from actions.c with a snapshot, and from the renderer with a snapshot.
 *
 * Each verb states its own condition and nothing else's. Two of the three are requests over the
 * air and have no meaning with no radio attached; the third is not - a trend is what this client
 * watched, and it survives the link dropping - so it is offered whenever there is a line to
 * draw. That sentence was not writable while the cursor was an index; see the header comment.
 */
void mesh_ui_status_actions(struct mesh_ui_status_actions *out, bool connected, bool synced,
                            bool has_trend);

/*
 * The entry for `verb`, or NULL when the screen is not offering it.
 *
 * The one lookup the three readers share. actions.c names the press with the label off this
 * entry, nav.c runs the verb it finds and does nothing when it finds none, and the renderer
 * asks whether the button it is about to draw is the one the cursor is on - each of them asking
 * about a *verb*, so none of them can be handed a position that means something else than it
 * did on the frame it was read from.
 */
const struct mesh_ui_status_action *
mesh_ui_status_find(const struct mesh_ui_status_actions *actions, uint8_t verb);

/*
 * Where a cursor remembering `verb` stands now.
 *
 * The verb itself while it is still offered, which is the ordinary answer and the point of the
 * whole exercise. When it has gone the cursor takes the nearest verb *above* it - the last one
 * on offer that the table lists before it - and the first on offer when nothing does, because a
 * cursor that fell off the bottom of a shrinking list should not reappear at the top of it.
 *
 * With nothing on offer the remembered verb is handed back unchanged. That is deliberate rather
 * than a missing case: a screen with no verbs draws no buttons, so there is nothing for the
 * cursor to be wrong about, and holding the reader's place across a link that dropped and came
 * back is the behaviour the old index could not have.
 */
uint8_t mesh_ui_status_verb_resolve(const struct mesh_ui_status_actions *actions, uint8_t verb);

/*
 * The verb one step from `verb`, or `verb` itself at either end of the list.
 *
 * `delta` is -1 for Up and +1 for Down. Returning the same verb rather than a flag is what lets
 * nav.c say whether the press moved anything by comparing, the way every other cursor on this
 * client reports a press that hit the end of a list.
 */
uint8_t mesh_ui_status_verb_step(const struct mesh_ui_status_actions *actions, uint8_t verb,
                                 int delta);

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
