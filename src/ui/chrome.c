#include "mesh/ui/chrome.h"

#include "mesh/core/updater.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include <string.h>

/*
 * The two tables, written out per state rather than composed - the same shape as the action-bar
 * tables in src/ui/actions.c and the verb table in src/ui/status.c, and for the same reason:
 * what the client says about itself in a given state should be readable in one place.
 */

bool mesh_ui_chrome_busy(const struct mesh_ui_snapshot *snapshot) {
    if (snapshot == NULL) {
        return false;
    }
    const struct mesh_ui_handshake_state *handshake = &snapshot->handshake;
    const struct mesh_ui_settings *settings = &snapshot->settings;

    /*
     * The config handshake, from the request going out to the radio saying it is done.
     *
     * `config_complete` is checked against a *connected* radio rather than on its own, because
     * the roster deliberately outlives the connection: a client sitting with a cached roster
     * and no radio has an incomplete handshake for as long as it runs, and a bar that never
     * stopped would say nothing at all.
     */
    if (mesh_ui_snapshot_connected_device(snapshot) != NULL &&
        (handshake->request_in_flight || !handshake->config_complete)) {
        return true;
    }
    /* An admin read on its way back, and a write on its way out. Both are what the Settings
       rows are waiting for when they say "not loaded". */
    if (settings->admin_busy || settings->write_pending) {
        return true;
    }
    /* A check or a download. The About row draws the download's own length; this only says the
       client has work outstanding, which is the half that was invisible from any other tab. */
    return settings->client.update_busy;
}

/*
 * The overlays that take the body, in the order fb_render_snapshot() stacks them.
 *
 * Written out here rather than behind a predicate in nav.h because this is the only place that
 * wants the question in this shape - mesh_ui_actions_for() walks the same list but needs to
 * know *which* one, so it cannot share an answer that is only a boolean. The cost of that is
 * that a new overlay has to be added here too, which is what
 * ui_chrome_banner_yields_to_a_modal walks one at a time to catch.
 */
static bool mesh_ui_chrome_modal_open(const struct mesh_ui_nav *nav) {
    return nav->confirm_open || nav->picker_open || nav->keyboard_open || nav->compose_open ||
           nav->reaction_open;
}

/* Whether the screen up is the one that already reports the updater in full. A banner pointing
   at a section is noise on that section. */
static bool mesh_ui_chrome_on_about(const struct mesh_ui_nav *nav) {
    return nav->screen == MESH_UI_SCREEN_SETTINGS &&
           nav->settings_section == (uint8_t)MESH_UI_SETTINGS_ABOUT;
}

bool mesh_ui_chrome_banner(const struct mesh_ui_snapshot *snapshot, struct mesh_ui_banner *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->detail = "";
    if (snapshot == NULL) {
        return false;
    }
    if (mesh_ui_chrome_modal_open(&snapshot->nav) || mesh_ui_chrome_on_about(&snapshot->nav)) {
        return false;
    }

    const struct mesh_ui_client_info *client = &snapshot->settings.client;
    /*
     * Both entries are the updater's, and the icon is the one Material puts on an informational
     * banner - which is also the icon the About section wears, because "there is something you
     * should know" and "the section that knows about this client" are close enough to the same
     * sentence that a second sprite would be saying it twice.
     */
    if (client->update_state == (uint8_t)MESH_UPDATE_READY) {
        out->kind = (uint8_t)MESH_UI_BANNER_UPDATE_READY;
        out->icon = MESH_UI_ICON_ABOUT;
        out->text = MESH_STR_BANNER_UPDATE_READY;
        out->supporting = MESH_STR_BANNER_UPDATE_READY_HINT;
        out->detail = client->update_latest;
        /* Success rather than primary: this one is a job that finished, and the difference
           between "done" and "there is something to do" is worth a family. */
        out->family = MESH_UI_FAMILY_SUCCESS;
        return true;
    }
    /*
     * `update_can_install` is the resolvability gate rather than a nicety. A dev build compares
     * itself against GitHub and reports what it finds, but is not allowed to install it - so a
     * banner raised there would be a container nothing the user can do would ever clear, on
     * every screen, for the rest of the run.
     */
    if (client->update_state == (uint8_t)MESH_UPDATE_AVAILABLE && client->update_can_install) {
        out->kind = (uint8_t)MESH_UI_BANNER_UPDATE_AVAILABLE;
        out->icon = MESH_UI_ICON_ABOUT;
        out->text = MESH_STR_BANNER_UPDATE_AVAILABLE;
        out->supporting = MESH_STR_BANNER_UPDATE_AVAILABLE_HINT;
        out->detail = client->update_latest;
        out->family = MESH_UI_FAMILY_PRIMARY;
        return true;
    }

    return false;
}
