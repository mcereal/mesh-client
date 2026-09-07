#ifndef MESH_UI_ACTIONS_H
#define MESH_UI_ACTIONS_H

/*
 * What the buttons do here, as data.
 *
 * The footer used to say it as a sentence - one catalog entry per screen, "A open node  X pin
 * Y write  L/R tabs" - and a sentence is the right shape for exactly one renderer: a single
 * line of text. It is the wrong shape for everything that reads like a handheld OS rather than
 * like a terminal, because the moment the buttons are drawn as *keycaps* the renderer needs the
 * letters and the verbs apart, and the only place they were apart was inside a translation.
 * Reading a translation back to find them is the one thing src/i18n exists to prevent.
 *
 * So the answer is the same one the colours, the icons and the shapes got: a screen names a
 * *token*, and a table answers. Here the token is a (button, verb) pair, the table is
 * mesh_ui_actions_for(), and the drawing - keycaps, elision, where the bar sits - belongs to
 * whichever backend is up. See fb_draw_action_bar() in src/ui/backends/fb_widgets.h.
 *
 * This lives in include/mesh/ui/ rather than beside the framebuffer backend because it is not
 * a drawing concern at all: which buttons mean something in a given state is a fact about the
 * nav, and a second backend would want the same answer. It is also, unlike the sentence it
 * replaces, something a test can assert about - see tests/suites/ui_actions.c.
 */

#include "mesh/i18n/strings.h"

#include <stddef.h>

struct mesh_ui_snapshot;

/*
 * A keycap, not a key code.
 *
 * These are the labels printed on the Brick's case, plus the two directional pairs, plus the
 * one that leaves - and each is a *thing a finger presses*, which is why the shoulders are one
 * entry rather than two. L1 and R1 never do different jobs; a bar that drew them separately
 * would spend two keycaps saying one thing.
 *
 * The face buttons deliberately do not carry their evdev codes here. Those are in
 * src/ui/input.c and they are not by position (see the note in CLAUDE.md); this enum is about
 * what is written on the plastic.
 */
enum mesh_ui_button {
    MESH_UI_BUTTON_A = 0,
    MESH_UI_BUTTON_B,
    MESH_UI_BUTTON_X,
    MESH_UI_BUTTON_Y,
    MESH_UI_BUTTON_START,
    /* L1 and R1 together: "the shoulders", which only ever move between things. */
    MESH_UI_BUTTON_SHOULDERS,
    MESH_UI_BUTTON_UP_DOWN,
    MESH_UI_BUTTON_LEFT_RIGHT,
    /*
     * Whatever leaves the pak. Its cap is the one that is not a constant: MENU by default, and
     * a bare key code when MESHCLIENT_QUIT_KEYS has rebound it to something whose name we do
     * not know. mesh_ui_input_quit_cap() answers.
     */
    MESH_UI_BUTTON_QUIT,
    MESH_UI_BUTTON_COUNT
};

/*
 * The cap's text: "A", "L/R", the arrow pair.
 *
 * Not a catalog id, and not an oversight. A face button's cap is what is silkscreened next to
 * it, so it reads the same in every language for the same reason a region code does - and the
 * arrows are drawn rather than read. The *verb* beside the cap is the translated half.
 *
 * Never NULL, including for a button outside the enum.
 */
const char *mesh_ui_button_cap(enum mesh_ui_button button);

/*
 * The most a screen offers at once.
 *
 * Six is one above the densest set here (the node detail's five), so reaching the cap means a
 * screen has grown a sixth thing to press rather than that the bar ran out of room - which is a
 * different problem, and the bar's own elision is what answers it.
 */
#define MESH_UI_ACTIONS_MAX 6U

/*
 * One press and what it does. Named for the button rather than for the bar because `struct
 * mesh_ui_action` is already taken, and by a different sense of the word: nav.h's is a request
 * the UI raises for the app to carry out, and this is a *label on a key*.
 */
struct mesh_ui_button_action {
    enum mesh_ui_button button;
    enum mesh_str_id label;
};

/*
 * The bar's contents, in the order they are offered.
 *
 * Order is priority, not layout: a bar that does not fit drops from the *end*, so the first
 * entry is the one that survives a narrow panel and a long translation. Each table below is
 * written with the press the screen is for at the front and "L/R tabs" - true everywhere, and
 * therefore the least worth the room - at the back.
 */
struct mesh_ui_action_bar {
    struct mesh_ui_button_action items[MESH_UI_ACTIONS_MAX];
    size_t count;
};

/*
 * What the buttons do, for the state this snapshot is in.
 *
 * The overlays win over the screen beneath them, in the order they stack: a confirmation over a
 * picker over the keyboard over the compose sheet, then the tab's own screen. That is the same
 * chain fb_render_snapshot() walks to decide what to draw, and it is the same chain because a
 * bar describing a screen the user cannot reach is worse than no bar.
 *
 * `out` is fully overwritten; a NULL snapshot yields an empty bar rather than a default one,
 * because there is no state to be describing.
 */
void mesh_ui_actions_for(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_action_bar *out);

#endif /* MESH_UI_ACTIONS_H */
