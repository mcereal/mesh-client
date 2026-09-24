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

#include "inkcell/ui/widgets.h"

#include "fb_screens_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/channel_share.h"
#include "mesh/ui/contact_share.h"
#include "mesh/ui/focus.h"
#include "mesh/ui/help.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/trust.h"

#include "inkwell/base/text.h"

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
 * The one body in this client that is *positioned* rather than windowed.
 *
 * A row-index window is the right model for a column of rows that are all the same height, and
 * it is what every other list here uses. A note's height is a property of its *words*, so this
 * screen was the one that had to hand the model an array of them - and a window over items of
 * mixed heights still moves a whole item at a time, which on a paragraph six lines long is the
 * body jumping half a screen. A body measured in pixels has no such array, rests between two
 * paragraphs, gives at its ends, and - the part a window could not do at all - gives the
 * heading over it something continuous to collapse against.
 *
 * The cursor is still a note, and the nav is untouched: which paragraph is being read is a
 * press's business, and inkcell_scroll_reveal() is what turns that into a position. What the
 * pixels buy is how the body gets there.
 */
void fb_render_help(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                    struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    struct mesh_ui_help_topic topic;
    if (!mesh_ui_help_topic(&snapshot->settings,
                            snapshot->handshake_valid ? &snapshot->handshake : NULL, nav, &topic)) {
        /* Reachable only if the section emptied under an open help screen - a disconnect
           between the press and this frame. Saying so beats drawing an empty list. */
        inkcell_fb_draw_app_bar(
            state, layout,
            &(const struct inkcell_fb_app_bar){.title = inkcell_str(MESH_STR_HELP_TITLE)});
        inkcell_fb_draw_empty(state, layout, INKCELL_ICON_ABOUT, inkcell_str(MESH_STR_HELP_TITLE));
        fb_scroll_report(state, false);
        return;
    }

    const char *headings[MESH_UI_HELP_ENTRIES_MAX];
    const char *bodies[MESH_UI_HELP_ENTRIES_MAX];
    /* In pixels, because that is what the body is measured in now. The steps are still the
       component's answer - a note's height is how far its words wrap - and a step is a line. */
    int tops[MESH_UI_HELP_ENTRIES_MAX];
    int heights[MESH_UI_HELP_ENTRIES_MAX];
    /* And the same measure in the unit the component lays out in, which is still steps: what
       the pixels are for is where the *body* sits, not how a note is built. */
    uint8_t steps[MESH_UI_HELP_ENTRIES_MAX];
    int content_h = 0;
    for (uint32_t i = 0; i < topic.count; ++i) {
        /* The opening paragraph is about the whole screen and names no row, so it gets the one
           heading this screen writes rather than a field's label. */
        headings[i] = topic.entries[i].label != INKCELL_STR_NONE
                          ? inkcell_str(topic.entries[i].label)
                          : inkcell_str(MESH_STR_HELP_OVERVIEW);
        bodies[i] = inkcell_str(topic.entries[i].body);
        const uint32_t rows = inkcell_fb_list_note_steps(state, headings[i], bodies[i]);
        steps[i] = rows > UINT8_MAX ? UINT8_MAX : (uint8_t)rows;
        tops[i] = content_h;
        heights[i] = (int)steps[i] * layout->line;
        content_h += heights[i];
    }

    /*
     * Where the body has got to. A frame with no memo behind it draws it settled rather than
     * not at all: a local scroll is one that never remembers anything, which is exactly what a
     * screen with no history should look like.
     */
    struct inkcell_scroll local;
    memset(&local, 0, sizeof local);
    struct inkcell_scroll *const scroll =
        fb_scroll(state, FB_SCROLL_HELP) != NULL ? fb_scroll(state, FB_SCROLL_HELP) : &local;
    const uint32_t cursor = nav->help_cursor < topic.count ? nav->help_cursor : 0U;
    if (topic.count > 0U) {
        /* The paragraph being read, brought inside the window - which is the whole of what the
           press means here. A pad of one line so a note never arrives flush against an edge
           with the next one's heading cut off behind it. */
        (void)inkcell_scroll_reveal(scroll, tops[cursor], heights[cursor], layout->line,
                                    state->now_ms);
    }

    /*
     * The heading, drawn large at the top of the body and shrunk into the bar once the body has
     * moved - a continuous function of the offset, which is the one piece of chrome a row-index
     * window could not have had.
     *
     * The screen it explains goes in `detail` rather than on a trail. This is the one place a
     * trail earned a level the navigation bar was not already carrying: the strip says
     * "Settings" and this screen's own title says "Help", so without the section name between
     * them the frame never says *what* is being explained. It comes off the topic rather than
     * out of the nav, because reading nav->settings_section here was this renderer knowing that
     * help is about settings - which stopped being true the moment a tab acquired a topic.
     */
    const struct inkcell_fb_large_title bar = {
        .title = inkcell_str(MESH_STR_HELP_TITLE),
        .detail = topic.subject != INKCELL_STR_NONE ? inkcell_str(topic.subject) : NULL,
    };
    inkcell_fb_draw_large_title(state, layout, &bar, inkcell_scroll_offset(scroll, state->now_ms));

    const struct inkcell_fb_rect body = {
        .x = inkcell_fb_region(state).x,
        .y = layout->body_y,
        .w = inkcell_fb_region(state).w,
        .h = layout->footer_y - layout->body_y,
    };
    struct inkcell_fb_viewport view;
    if (inkcell_fb_viewport_begin(state, &view, body, scroll, content_h)) {
        /*
         * The notes at their own coordinates, from nothing to `content_h`, and the viewport
         * cuts what does not fit. A window would have been handed the count that fits and asked
         * to walk it; what is handed here is the whole of the content, in a layout whose only
         * job is to be tall enough to hold it - which is the shape the header calls "the one
         * thing this costs".
         */
        struct inkcell_fb_layout inner = fb_layout_in(
            layout, (struct inkcell_fb_rect){.x = 0, .y = 0, .w = body.w, .h = content_h});
        struct inkcell_fb_list list =
            inkcell_fb_list_begin_heights(&inner, topic.count, cursor, steps);
        uint32_t i;
        while (inkcell_fb_list_next(&list, &i)) {
            inkcell_fb_list_note(state, &list, i, headings[i], bodies[i]);
        }
        inkcell_fb_viewport_end(state, &view);
        /* Outside the viewport, because a rail is beside the window rather than in it - and
           after it, so the shortening thumb an overscroll draws is not clipped by the band it
           is reporting on. */
        inkcell_fb_draw_scroll_rail(state, &view, scroll);
    }
    fb_scroll_report(state, inkcell_scroll_active(scroll, state->now_ms));
}

/*
 * Writes what a dialog says into the slot it is remembered in.
 *
 * The strings are copied rather than pointed at: `headline` and `text` are composed into a
 * caller's buffer, and the two answers come out of the catalog, which is reloaded under a
 * change of language. A memo holding a pointer into either would be a panel drawing whatever
 * is at that address a frame later.
 */
static void fb_dialog_remember(struct fb_overlay_memo *memo,
                               const struct inkcell_fb_dialog *dialog) {
    if (memo == NULL || dialog == NULL) {
        return;
    }
    (void)inkwell_str_copy(memo->headline, sizeof memo->headline, dialog->headline);
    (void)inkwell_str_copy(memo->text, sizeof memo->text, dialog->text);
    (void)inkwell_str_copy(memo->accept, sizeof memo->accept, dialog->accept);
    (void)inkwell_str_copy(memo->cancel, sizeof memo->cancel, dialog->cancel);
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
static void fb_put_dialog(struct inkcell_draw_state *state, struct inkcell_fb_layout *layout,
                          enum fb_overlay_id id, bool up, const struct inkcell_fb_dialog *dialog) {
    struct fb_overlay_memo *const memo = fb_overlay_memo(state, id);
    if (dialog != NULL) {
        fb_dialog_remember(memo, dialog);
    }
    if (dialog == NULL && (memo == NULL || !memo->valid)) {
        /* Nothing to say and nothing remembered: a layer that never opened, or one drawn on a
           frame with no memo behind it. Not drawing is the only honest answer, and the slot it
           would have taken is released by never being asked for. */
        return;
    }
    const struct inkcell_fb_dialog remembered = {
        .icon = memo != NULL ? memo->icon : INKCELL_ICON_NONE,
        .headline = memo != NULL ? memo->headline : "",
        .text = memo != NULL ? memo->text : "",
        .accept = memo != NULL ? memo->accept : "",
        .cancel = memo != NULL ? memo->cancel : "",
        .cursor = memo != NULL ? memo->cursor : 0U,
        .destructive = memo != NULL && memo->destructive,
    };
    struct inkcell_fb_dialog put_copy = dialog != NULL ? *dialog : remembered;
    /*
     * What the d-pad calls the two answers: accept, and cancel one past it.
     *
     * Registered because the pair is laid out side by side or *stacked*, and inkcell decides
     * which from the words and the panel's width. Left-right on a stacked pair is not the press
     * that moves between them, and a nav toggling on every direction was right only because it
     * never had to know - which is the same thing as never being able to be wrong about
     * anything else either.
     */
    put_copy.action_focus_id = MESH_UI_FOCUS_DIALOG;
    const struct inkcell_fb_dialog *const put = &put_copy;
    if (!inkcell_fb_draw_dialog(state, layout, put, (uint32_t)id, up) && memo != NULL) {
        /* All the way out. What it asked is not the next question, and a memo kept past the
           travel it was for would be the words a fresh layer arrives holding. */
        memo->valid = false;
    }
}

/* "Save <section>?" for the sections whose write can cut this client off, and "Reboot the
   radio?" and its siblings for the Radio actions section. Which of the two it is standing in
   front of is nav->confirm_action; all three strings come from settings.c. */
void fb_render_confirm(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                       struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    /* Not over help, which takes every press - see the note at the tail of
       fb_render_snapshot(). */
    if (!nav->confirm_open || nav->help_open) {
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
    const struct inkcell_fb_dialog dialog = {
        .icon = INKCELL_ICON_WARNING,
        .headline = title,
        .text = text,
        .accept = mesh_ui_settings_confirm_accept(confirmed),
        .cancel = inkcell_str(MESH_STR_COMMON_CANCEL),
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
void fb_render_verify(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                      struct inkcell_fb_layout *layout) {
    struct mesh_ui_verify_sheet sheet;
    char headline[96];
    char text[256];
    /* `help_open` for the reason the confirm above has it, and this is the sheet that reason is
       about: a radio raises a verification at a moment of its own choosing, so it is the one
       that can arrive over an open help screen and be left unanswerable. */
    if (!snapshot->nav.verify_open || snapshot->nav.help_open ||
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

    const struct inkcell_fb_dialog dialog = {
        .icon = sheet.icon,
        .headline = headline,
        .text = text,
        .accept = inkcell_str(sheet.accept),
        .cancel = inkcell_str(sheet.cancel),
        .cursor = snapshot->nav.verify_cursor,
        .destructive = false,
    };
    fb_put_dialog(state, layout, FB_OVERLAY_VERIFY, true, &dialog);
}
