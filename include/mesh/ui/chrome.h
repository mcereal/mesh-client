#ifndef MESH_UI_CHROME_H
#define MESH_UI_CHROME_H

#include "mesh/i18n/strings.h"
#include "mesh/ui/icon.h"
#include "mesh/ui/theme.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mesh_ui_snapshot;

/*
 * What the frame says about the *client*, as opposed to what a screen says about itself.
 *
 * Every other statement on the panel belongs to something: a row describes a node, a card
 * describes the link, the app bar names the screen it heads. Two do not. "A newer release is
 * out there" and "something is in flight right now" are true of the whole client, they are true
 * on whichever tab you happen to be looking at, and until this file existed the first was
 * visible only in Settings > About and the second was not visible anywhere.
 *
 * So they are chrome, drawn once by fb_render_snapshot() around whichever screen is up, and
 * *what* they say is decided here rather than beside the framebuffer - for the same reason
 * src/ui/status.c decides which card carries which verb. A second backend gets the same two
 * answers, and a unit test can ask the questions without a panel.
 *
 * The two are deliberately a pair, and the split between them is the rule that keeps either
 * from being noise:
 *
 *   the bar     something is *moving*. It costs no row, it says nothing about what, and it
 *               goes away on its own when the work lands.
 *   the banner  something has *settled* and stays true until somebody or something resolves
 *               it. It costs rows, so it has to be worth them.
 *
 * A state that is one of them is never the other.
 */

/* ---- the progress bar ---------------------------------------------------------------------- */

/*
 * Whether the client is waiting on something it has already asked for.
 *
 * The gap this closes: open Settings before the radio has answered and eight sections say "not
 * loaded", which is indistinguishable from a radio that will never answer. The handshake, an
 * admin read, a queued write and an update check are all *requests already sent*, and none of
 * them had any presence on the frame at all.
 *
 * Deliberately one boolean rather than a kind. A screen-level indicator answers "is this thing
 * stuck or is it working", which is the same answer whatever the work is; naming the work would
 * mean the bar had to be read, and a bar that has to be read is a row of text. What each job is
 * doing is still said where that job lives - the About meter counts a download's bytes, the
 * Settings rows fill in as they arrive.
 *
 * And it is indeterminate for the same reason it is a boolean. Exactly one of these jobs has a
 * fraction (a download knows its asset's size) and that fraction is already drawn, on the About
 * row it belongs to. A screen-level bar reporting one job's progress as though it were the
 * client's would be a number that means something different depending on what happens to be in
 * flight, which is worse than no number.
 */
bool mesh_ui_chrome_busy(const struct mesh_ui_snapshot *snapshot);

/* ---- the banner ---------------------------------------------------------------------------- */

/*
 * Which persistent notice the frame carries, if any.
 *
 * At most one. Banners stack on a phone; on fifteen body rows a second one is a screen of
 * chrome with a list underneath, so this answers with the most urgent and the rest wait.
 */
enum mesh_ui_banner_kind {
    MESH_UI_BANNER_NONE = 0,
    /* A release has been downloaded, verified and installed; the binary on disk is newer than
       the one running. Resolved by restarting, which is the one thing that makes it untrue. */
    MESH_UI_BANNER_UPDATE_READY,
    /* A check found a newer release and this build is allowed to install it. Resolved by
       installing it, or by a later check finding this one is current. */
    MESH_UI_BANNER_UPDATE_AVAILABLE,
    MESH_UI_BANNER_KIND_COUNT,
};

/*
 * A banner, in the slots the component draws rather than as a sentence.
 *
 * `text` and `supporting` are string ids; `detail` is a runtime string that is *not* translated
 * - a version number, in the same category as a region code or a hardware model name (see
 * docs/i18n.md). Keeping the version out of the words is what lets the headline be one short
 * translatable phrase instead of a format string with a number glued into it, and it is the
 * same split the top app bar made when "Settings > %s%s%s" became a trail, a title and a badge.
 */
struct mesh_ui_banner {
    uint8_t kind; /* enum mesh_ui_banner_kind */
    enum mesh_ui_icon icon;
    enum mesh_str_id text;       /* the headline: what is true */
    enum mesh_str_id supporting; /* what to do about it; MESH_STR_NONE for nothing */
    /* Points into `snapshot`, so it lives exactly as long as the snapshot the call was made
       with - which is the frame being drawn. Never NULL; empty when there is nothing to show. */
    const char *detail;
    /* The container's fill, taken with its ink from mesh_ui_theme_paint(). A family rather than
       a tone because a banner fills something, and a fill and the words on it are a pair every
       theme was validated as a pair. */
    enum mesh_ui_family family;
};

/*
 * Fills `out` with the banner this frame carries and returns true, or returns false and leaves
 * `out` cleared.
 *
 * Three rules decide the table, and the first two are why it is shorter than §2.9 of the
 * component roadmap expected:
 *
 *   **A banner says only what nothing else on the frame says.** This is the overline's rule
 *   (see fb_draw_app_bar) arriving somewhere else. It is what refuses "radio disconnected",
 *   which the status line under the keycaps already reports on every frame, and it is why the
 *   update banner stands down inside Settings > About - the section it is pointing at states
 *   the same thing in more detail, so a banner over it would be the client telling you
 *   something while you are already reading it.
 *
 *   **A banner must resolve.** There is no dismissal here: dismissal needs somewhere to
 *   remember what was dismissed, a press to spend on it and a rule about when it comes back,
 *   which is a nav change with a component on the end of it. So nothing is raised that cannot
 *   go away on its own terms - which is what refuses the radio's own ERROR notice, kept until
 *   the link cycles, and what makes `update_can_install` part of the gate rather than a detail:
 *   an update a build is not allowed to install is a banner nothing the user does can clear.
 *
 *   **A modal owns the body.** Nothing is raised over the confirm dialog, the picker, the
 *   keyboard or the compose sheet. Those four take the body for a question, and a container
 *   that shortened the body while one was up would move the question as it was being answered.
 */
bool mesh_ui_chrome_banner(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_banner *out);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_CHROME_H */
