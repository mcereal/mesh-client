#define _POSIX_C_SOURCE 200809L

/*
 * The three things drawn over a screen that are about the screen underneath: its help, a confirm
 * it raised, and the key-verification sheet a radio can raise at any moment.
 *
 * None of them says anything of its own. What the help paragraphs are is src/ui/tables/help.c,
 * what a confirm asks is src/ui/settings/settings.c, and what a verification stage says -
 * including which two answers it has - is src/ui/tables/trust.c. A renderer with an opinion
 * about cryptography is the thing this file exists to not be.
 */

#include "fb_widgets.h"

#include "fb_screens_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/channel_share.h"
#include "mesh/ui/contact_share.h"
#include "mesh/ui/help.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/trust.h"

#include <stdio.h>
#include <string.h>

/*
 * Help: what the screen underneath is for, and what its rows mean.
 *
 * A list of paragraphs rather than a dialog, because there is nothing here to answer - and a
 * list rather than a card for the same reason: a card note stops at three lines, and these are
 * the only content on the frame. Which paragraphs there are is src/ui/tables/help.c's answer, read
 * here and by the action bar and the key handler alike.
 *
 * The heights are measured before the list opens, which on this screen is not a formality: a
 * note's height is a property of its *words* rather than of its kind, so this is the one list in
 * the client where the screen genuinely cannot guess and the model genuinely has to be told.
 */
void fb_render_help(struct mesh_ui_backend_fb_state *state, const struct mesh_ui_snapshot *snapshot,
                    struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    struct mesh_ui_help_topic topic;
    if (!mesh_ui_help_topic(&snapshot->settings,
                            snapshot->handshake_valid ? &snapshot->handshake : NULL, nav, &topic)) {
        /* Reachable only if the section emptied under an open help screen - a disconnect
           between the press and this frame. Saying so beats drawing an empty list. */
        fb_draw_empty(state, layout, MESH_UI_ICON_ABOUT, mesh_str(MESH_STR_HELP_TITLE));
        return;
    }

    /*
     * The screen it explains, on the trail above the title.
     *
     * This is the one place a trail earns a level the navigation bar is not already carrying:
     * the strip says "Settings" and this screen's own title says "Help", so without the section
     * name between them the frame never says *what* is being explained.
     *
     * It comes off the topic rather than out of the nav. Reading nav->settings_section here was
     * this renderer knowing that help is about settings, which stopped being true the moment a
     * tab acquired a topic - and on a help screen over the Nodes tab it would have drawn
     * whichever section the user had last opened, confidently and wrongly.
     */
    struct fb_app_bar bar = {.title = mesh_str(MESH_STR_HELP_TITLE)};
    if (topic.subject != MESH_STR_NONE) {
        bar.trail[bar.trail_count++] = mesh_str(topic.subject);
    }
    fb_draw_app_bar(state, layout, &bar);

    const char *headings[MESH_UI_HELP_ENTRIES_MAX];
    const char *bodies[MESH_UI_HELP_ENTRIES_MAX];
    uint8_t heights[MESH_UI_HELP_ENTRIES_MAX];
    for (uint32_t i = 0; i < topic.count; ++i) {
        /* The opening paragraph is about the whole screen and names no row, so it gets the one
           heading this screen writes rather than a field's label. */
        headings[i] = topic.entries[i].label != MESH_STR_NONE ? mesh_str(topic.entries[i].label)
                                                              : mesh_str(MESH_STR_HELP_OVERVIEW);
        bodies[i] = mesh_str(topic.entries[i].body);
        const uint32_t steps = fb_list_note_steps(state, headings[i], bodies[i]);
        heights[i] = steps > UINT8_MAX ? UINT8_MAX : (uint8_t)steps;
    }

    struct fb_list list = fb_list_begin_heights(layout, topic.count, nav->help_cursor, heights);
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        fb_list_note(state, &list, i, headings[i], bodies[i]);
    }
}

/*
 * Writes what a dialog says into the slot it is remembered in.
 *
 * The strings are copied rather than pointed at: `headline` and `text` are composed into a
 * caller's buffer, and the two answers come out of the catalog, which is reloaded under a
 * change of language. A memo holding a pointer into either would be a panel drawing whatever
 * is at that address a frame later.
 */
static void fb_dialog_remember(struct fb_dialog_memo *memo, const struct fb_dialog *dialog) {
    if (memo == NULL || dialog == NULL) {
        return;
    }
    (void)mesh_str_copy(memo->headline, sizeof memo->headline, dialog->headline);
    (void)mesh_str_copy(memo->text, sizeof memo->text, dialog->text);
    (void)mesh_str_copy(memo->accept, sizeof memo->accept, dialog->accept);
    (void)mesh_str_copy(memo->cancel, sizeof memo->cancel, dialog->cancel);
    memo->icon = dialog->icon;
    memo->cursor = dialog->cursor;
    memo->destructive = dialog->destructive;
    memo->valid = true;
}

/*
 * Puts `dialog` on layer `id`, and keeps putting the last one while the layer walks out.
 *
 * `up` is whether the nav still wants the question, and `dialog` is NULL on a frame where it
 * cannot be described - which is most of the frames a layer spends leaving. The two questions
 * below are otherwise the same call, so it is written once: what differs between them is where
 * their words come from, which is the half of a screen that is worth reading.
 */
static void fb_put_dialog(struct mesh_ui_backend_fb_state *state, struct fb_layout *layout,
                          enum fb_overlay_id id, bool up, const struct fb_dialog *dialog) {
    struct fb_dialog_memo *const memo = fb_dialog_memo(state, id);
    if (dialog != NULL) {
        fb_dialog_remember(memo, dialog);
    }
    if (dialog == NULL && (memo == NULL || !memo->valid)) {
        /* Nothing to say and nothing remembered: a layer that never opened, or one drawn on a
           frame with no memo behind it. Not drawing is the only honest answer, and the slot it
           would have taken is released by never being asked for. */
        return;
    }
    const struct fb_dialog remembered = {
        .icon = memo != NULL ? memo->icon : MESH_UI_ICON_NONE,
        .headline = memo != NULL ? memo->headline : "",
        .text = memo != NULL ? memo->text : "",
        .accept = memo != NULL ? memo->accept : "",
        .cancel = memo != NULL ? memo->cancel : "",
        .cursor = memo != NULL ? memo->cursor : 0U,
        .destructive = memo != NULL && memo->destructive,
    };
    const struct fb_dialog *const put = dialog != NULL ? dialog : &remembered;
    if (!inkcell_fb_draw_dialog(state, layout, put, (uint32_t)id, up) && memo != NULL) {
        /* All the way out. What it asked is not the next question, and a memo kept past the
           travel it was for would be the words a fresh layer arrives holding. */
        memo->valid = false;
    }
}

/* "Save <section>?" for the sections whose write can cut this client off, and "Reboot the
   radio?" and its siblings for the Radio actions section. Which of the two it is standing in
   front of is nav->confirm_action; all three strings come from settings.c. */
void fb_render_confirm(struct mesh_ui_backend_fb_state *state,
                       const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    if (!nav->confirm_open) {
        fb_put_dialog(state, layout, FB_OVERLAY_CONFIRM, false, NULL);
        return;
    }
    const enum mesh_ui_settings_section section =
        (enum mesh_ui_settings_section)nav->settings_section;
    const enum mesh_ui_settings_action confirmed =
        (enum mesh_ui_settings_action)nav->confirm_action;
    char title[96];
    char text[256];
    /*
     * The import sheet's words come out of the *link*, not out of the tables: what the user is
     * agreeing to is joining a named mesh, and the name is in the characters they just typed.
     * Asked of src/ui/views/channel_share.c for the reason the verification sheet asks
     * src/ui/tables/trust.c
     * - parsing a link is not something a renderer may do - and the tables answer for every
     * other sheet exactly as before.
     */
    if (confirmed == MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS &&
        mesh_ui_channel_import_sheet(nav->channel_url, title, sizeof title, text, sizeof text)) {
        /* nothing more to do: the sheet filled both */
    } else if (confirmed == MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT &&
               mesh_ui_contact_import_sheet(nav->contact_url, title, sizeof title, text,
                                            sizeof text)) {
        /* likewise, out of the contact link rather than the channel one */
    } else {
        mesh_ui_settings_confirm_title(section, nav->settings_channel, confirmed, title,
                                       sizeof title);
        mesh_ui_settings_confirm_text(section, confirmed, text, sizeof text);
        /* And which radio, when it is not this one. The banner that has been saying so on every
           other frame is gone the moment this panel takes the body, so the sheet says it
           instead - in a sentence of settings.c's own, joined here rather than composed here. */
        mesh_ui_settings_confirm_add_subject(&snapshot->settings, confirmed, text, sizeof text);
    }

    /*
     * A dialog rather than a screen. There is no title bar and no list: the question is the
     * panel's own headline and the two answers are buttons on it, which is the shape that says
     * "this is being asked of you" rather than "here is another list to walk".
     */
    const struct fb_dialog dialog = {
        .icon = MESH_UI_ICON_WARNING,
        .headline = title,
        .text = text,
        .accept = mesh_ui_settings_confirm_accept(confirmed),
        .cancel = mesh_str(MESH_STR_COMMON_CANCEL),
        .cursor = nav->confirm_cursor,
        /* A radio action cannot be taken back - a reboot drops the link, a NodeDB reset empties
           the roster - while a section save is only the settings the user has just been
           editing. The two deserve different-coloured answers. */
        .destructive = confirmed != (uint8_t)MESH_UI_SETTINGS_ACTION_NONE,
    };
    fb_put_dialog(state, layout, FB_OVERLAY_CONFIRM, true, &dialog);
}

/*
 * The key-verification sheet: the radio's half of the ceremony, put to the user.
 *
 * The same dialog the settings confirm uses, and for the same reason - a question is a panel
 * with two answers on it rather than another list to walk - but everything it says comes out of
 * src/ui/tables/trust.c rather than from here, including which two answers this stage has. That is
 * the house rule doing real work: the difference between "they match" and "stop" is the difference
 * between a verification and a refusal, and a renderer choosing it would be a renderer with an
 * opinion about cryptography.
 *
 * Never destructive, which is worth saying because a settings confirm sometimes is. The
 * dangerous half of this dialog is not the button that acts - it is answering "they match"
 * without having compared anything, and no colour on a button prevents that. What does is the
 * paragraph under the headline, which is why the panel keeps one at every stage.
 */
void fb_render_verify(struct mesh_ui_backend_fb_state *state,
                      const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    struct mesh_ui_verify_sheet sheet;
    char headline[96];
    char text[256];
    if (!snapshot->nav.verify_open ||
        !mesh_ui_verify_sheet_of(&snapshot->verification, &sheet, headline, sizeof headline, text,
                                 sizeof text)) {
        /*
         * Either the sheet is not up, or the exchange ended between the press and this frame -
         * a link that dropped, or the other end standing down. Both are the same thing to draw:
         * the stage it was last at, walking out. What the panel underneath says about an
         * exchange that has gone is the screen's business rather than this layer's, which is
         * the difference a layer makes - this used to have to draw an empty body and say so,
         * because it had taken the body away from a screen that could.
         */
        fb_put_dialog(state, layout, FB_OVERLAY_VERIFY, false, NULL);
        return;
    }

    const struct fb_dialog dialog = {
        .icon = sheet.icon,
        .headline = headline,
        .text = text,
        .accept = mesh_str(sheet.accept),
        .cancel = mesh_str(sheet.cancel),
        .cursor = snapshot->nav.verify_cursor,
        .destructive = false,
    };
    fb_put_dialog(state, layout, FB_OVERLAY_VERIFY, true, &dialog);
}
