#define _POSIX_C_SOURCE 200809L

/*
 * The four sheets that put a message together: the reactions, the compose sheet, the "send to"
 * picker and the on-screen keyboard.
 *
 * Drawn over whatever tab is up rather than as one of them, which is why they are not in
 * fb_screens_messages.c beside the transcript they mostly write to. The keyboard is the clearest
 * case: it is the client's only text entry and the Devices tab, the waypoint editor and the
 * settings all raise it, so filing it under Messages would be filing it under one of its
 * callers.
 */

#include "inkcell/ui/layout.h"
#include "inkcell/ui/widgets.h"

#include "fb_screens_internal.h"

#include "mesh/core/message.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/chrome.h"
#include "mesh/ui/focus.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/nodes.h"
#include "mesh/ui/reactions.h"
#include "mesh/ui/settings.h"

#include <stdio.h>
#include <string.h>

/* The most lines a window's text field grows to. The longest draft any keyboard here takes is a
   channel link of a few hundred characters, which wraps to about this many on a desktop-width
   field; past it a tall window would only be drawing an empty box. */
#define MESH_UI_KB_FIELD_LINES_MAX 8U

/*
 * The bubble sheet: the fixed emoji set one per row, then the delete, aimed at the message X
 * was pressed on.
 *
 * The message it is about is the heading rather than a row, for the reason the compose sheet's
 * destination is: nothing on this screen chooses it, so a row that looked pressable would be
 * offering a choice that is already made. It is also what makes the delete safe to put here -
 * the thing about to be thrown away is quoted at the top of the screen it is thrown away from.
 */
void fb_render_reactions(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                         struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    /*
     * Which message is being answered, which the sheet keeps for as long as it is on the panel.
     *
     * The rows are the same nine whatever the message is - the faces come out of
     * src/ui/tables/reactions.c and the delete out of the catalog - so all that has to outlive
     * the press is the packet id the quote is drawn from.
     */
    /* Not over help, which takes every press - see the note at the tail of
       fb_render_snapshot(). */
    const bool up = nav->reaction_open && !nav->help_open;
    const uint32_t reply_to = fb_overlay_subject(state, FB_OVERLAY_REACTIONS, up, nav->reply_to);
    if (reply_to == 0U) {
        return;
    }

    char target[96] = {0};
    fb_thread_quote(mesh_ui_snapshot_message_view(snapshot), reply_to, target, sizeof target);
    char title[160];
    inkcell_str_format(title, sizeof title, MESH_STR_MESSAGE_ACTIONS, target);

    /*
     * A sheet over the transcript rather than a screen in place of it.
     *
     * The message being answered was quoted into the app bar because the transcript it came
     * out of had been taken away - the heading was standing in for a bubble that was still
     * two hundred milliseconds ago. It comes up over that bubble now, and the quote is the
     * sheet's own title for the same reason it was the app bar's: the reader picked this
     * message out of the thread, and a sheet naming a different one is a sheet about some
     * other message.
     */
    const uint32_t count = mesh_ui_nav_reaction_row_count();
    const struct inkcell_fb_sheet sheet = {.title = title};
    struct inkcell_overlay_frame frame;
    struct inkcell_fb_layout inner;
    if (!fb_sheet_begin(state, layout, FB_OVERLAY_REACTIONS, up, &sheet, (int)count * layout->line,
                        &frame, &inner)) {
        return;
    }

    struct inkcell_fb_list list = inkcell_fb_list_begin(&inner, count, nav->reaction_cursor);
    inkcell_fb_list_glide(state, &list, FB_LIST_REACTIONS);
    inkcell_fb_list_focus(&list, (uint32_t)MESH_UI_FOCUS_SHEET_ROWS);
    uint32_t i;
    while (inkcell_fb_list_next(&list, &i)) {
        /* The delete takes an icon rather than an emoji, and the danger role rather than the
           neutral fill: it is the one row here that destroys something, and the two are told
           apart by what they look like before they are read. */
        if (mesh_ui_nav_reaction_row_is_delete(i)) {
            const struct inkcell_fb_list_item row = {
                .leading = {.kind = INKCELL_FB_LEADING_ICON,
                            .icon = INKCELL_ICON_DELETE,
                            .role = INKCELL_COLOR_ERROR},
                .text = inkcell_str(nav->message_delete_armed ? MESH_STR_MESSAGE_DELETE_CONFIRM
                                                              : MESH_STR_MESSAGE_DELETE),
                .divider = true,
            };
            inkcell_fb_list_item(state, &list, i, &row);
            continue;
        }
        /* The glyph in the leading slot and the word beside it: eight faces in a column at
           this scale are not eight distinguishable things, and a text backend has no sprites
           for any of them. */
        const struct inkcell_fb_list_item row = {
            .leading = {.kind = INKCELL_FB_LEADING_AVATAR,
                        .label = mesh_ui_reaction_emoji(i),
                        /* A stated neutral fill rather than a tint: an avatar's colour is how
                           the eye tells one node from another, and here the glyph inside it is
                           already the whole of what the row is. */
                        .role = INKCELL_COLOR_SURFACE_SEL},
            .text = inkcell_str(mesh_ui_reaction_label(i)),
            .divider = true,
        };
        inkcell_fb_list_item(state, &list, i, &row);
    }
    fb_sheet_end(state, &frame);
}

/* Compose overlay: it writes to the open thread, so the destination is a heading rather than
   an editable row. */
void fb_render_compose(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                       struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    char title[96];
    inkcell_str_format(title, sizeof title, MESH_STR_COMPOSE_TO_KIND, nav->target_name,
                       inkcell_str(nav->target_node == MESH_MESSAGE_BROADCAST_ADDR
                                       ? MESH_STR_COMPOSE_SUFFIX_CHANNEL
                                       : MESH_STR_COMPOSE_SUFFIX_DIRECT));
    /* The badge is the one thing on the sheet that says this answers a message rather than the
       conversation - which is the whole difference between the press that opened it and Y. */
    fb_draw_app_bar(
        state, layout,
        &(const struct inkcell_fb_app_bar){
            .title = title,
            .badge = nav->reply_to != 0U ? inkcell_str(MESH_STR_COMPOSE_BADGE_REPLY) : NULL,
            .badge_family = INKCELL_FAMILY_TERTIARY,
        });

    struct inkcell_fb_list list =
        inkcell_fb_list_begin(layout, mesh_ui_nav_compose_row_count(), nav->compose_cursor);
    inkcell_fb_list_focus(&list, (uint32_t)MESH_UI_FOCUS_ROWS);
    struct inkcell_line line;
    uint32_t i;
    while (inkcell_fb_list_next(&list, &i)) {
        const bool is_draft = (i == MESH_UI_COMPOSE_ROW_DRAFT);
        inkcell_line_reset(&line);
        /* The draft is the row that opens the keyboard and the rest are texts to send as they
           stand, which is a difference in kind rather than in indentation - so it is the
           accent tone and the accent edge that say so, and the two spaces the canned rows used
           to be pushed over by are gone. */
        if (is_draft) {
            if (nav->draft[0] != '\0') {
                inkcell_line_str(&line, MESH_STR_COMPOSE_DRAFT, nav->draft);
            } else {
                inkcell_line_printf(&line, "%s", inkcell_str(MESH_STR_COMPOSE_DRAFT_EMPTY));
            }
        } else {
            inkcell_line_printf(&line, "%s", mesh_ui_canned_text(i - MESH_UI_COMPOSE_FIRST_CANNED));
        }
        const struct inkcell_fb_list_item row = {
            .text = inkcell_line_text(&line),
            /* A started-but-unsent message is in flight, which is the tertiary - and the
               edge bar follows the tone, so the row is marked in the colour of the reason it
               is marked. */
            .tone = is_draft ? INKCELL_TONE_TERTIARY : INKCELL_TONE_NORMAL,
            .accent_edge = is_draft,
            .divider = true,
        };
        inkcell_fb_list_item(state, &list, i, &row);
    }
}

/* "Send to" list: channels, then nodes, cursor on the current target. Mutable state, like
   every inkcell_fb_list_item() caller. */
void fb_render_picker(struct inkcell_draw_state *state, const struct mesh_ui_snapshot *snapshot,
                      struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    struct mesh_ui_store view;
    mesh_ui_store_view(snapshot, &view);

    const uint32_t count = mesh_ui_nav_picker_count(&view);
    char title[96];
    mesh_ui_chrome_list_title(title, sizeof title, inkcell_str(MESH_STR_PICKER_TITLE), count, count,
                              0U);
    fb_draw_app_bar(state, layout, &(const struct inkcell_fb_app_bar){.title = title});
    if (count == 0U) {
        inkcell_fb_draw_empty(state, layout, INKCELL_ICON_MESSAGES,
                              inkcell_str(MESH_STR_PICKER_EMPTY));
        return;
    }

    /* The channels, and every node but our own. */
    uint8_t steps[MESH_UI_MAX_CHANNELS + MESH_UI_MAX_HANDSHAKE_NODES];
    struct inkcell_fb_list list = fb_list_begin_steps(state, layout, count, nav->picker_cursor, 1U,
                                                      steps, sizeof steps, FB_LIST_ROLE_MENU);
    inkcell_fb_list_glide(state, &list, FB_LIST_PICKER);
    inkcell_fb_list_focus(&list, (uint32_t)MESH_UI_FOCUS_ROWS);
    char name[96];
    char initials[MESH_UI_CONVERSATION_INITIALS_MAX];
    uint32_t i;
    while (inkcell_fb_list_next(&list, &i)) {
        uint32_t node = 0U;
        uint8_t channel = 0U;
        if (!mesh_ui_nav_picker_row(&view, i, &node, &channel, name, sizeof name)) {
            break;
        }
        const bool is_channel = (node == MESH_MESSAGE_BROADCAST_ADDR);
        const bool current =
            (node == nav->target_node) && (!is_channel || channel == nav->target_channel);

        /*
         * The same discs the conversation list draws, for the same rows. The nav layer answers
         * both the cells and the tint (mesh_ui_nav_target_avatar), which is what keeps a node
         * the same two letters and the same colour in both lists - this row's *name* is
         * "BRVO  Bravo Creek", and initials taken from that would read "BB".
         *
         * The disc also carries the "(channel)" suffix the row used to spell out, as the tag.
         *
         * What it no longer carries is which row is the current target. That was a stated
         * accent fill, and the fill is exactly the thing the disc is *for*: a node is the same
         * two letters and the same colour everywhere in this client, and marking the target
         * overwrote the second of those on precisely the row the eye was looking for. It is a
         * radio in the trailing slot now - identity at the leading edge, selection at the
         * trailing one, which is the division every list of choices makes and the reason the
         * two were fighting over one disc.
         */
        uint32_t tint = 0U;
        mesh_ui_nav_target_avatar(&view, node, channel, initials, sizeof initials, &tint);
        /* Keyed on the row, above everything the settings fields reach. The picker closes on the
           press that changes the answer, so this never animates in place - it is the same
           control the rest of the UI uses, drawn at rest. */
        struct inkcell_fb_selection sel = {
            .id = 0x04000000U | i,
            .on = current,
        };
        const struct inkcell_fb_list_item row = {
            .leading =
                {
                    .kind = INKCELL_FB_LEADING_AVATAR,
                    .label = initials,
                    /* A channel's disc carries the tag rather than the '#' the nav layer hands
                       back, which is what the conversation list puts in the same disc. */
                    .icon = is_channel ? INKCELL_ICON_CHANNEL : INKCELL_ICON_NONE,
                    .tint = tint,
                    .role = INKCELL_COLOR_COUNT,
                },
            .text = name,
            .tone = is_channel ? INKCELL_TONE_PRIMARY : INKCELL_TONE_NORMAL,
            .trailing = {.kind = INKCELL_FB_TRAILING_RADIO, .sel = &sel},
            .divider = true,
        };
        inkcell_fb_list_item(state, &list, i, &row);
    }
}

/*
 * The grid, which is inkcell's: the keys, how tall they are when the body is short, which way the
 * slack goes, and how big the letter on a key that size is set. What is left for this screen is
 * the two things only it knows - which panel the cursor is on, which arrived in the snapshot, and
 * what the submit key is *for*.
 *
 * The symbol on that key and the word under it now come off one predicate rather than two copies
 * of the same list. They disagreed: the copy here left the key-verification prompt out, so six
 * digits that must never reach the mesh were typed under a send arrow.
 */
static void fb_draw_keyboard_grid(const struct inkcell_draw_state *state,
                                  const struct mesh_ui_nav *nav, struct inkcell_fb_layout *layout,
                                  int *y) {
    const struct inkcell_keyboard_layout kb_layout = mesh_ui_nav_kb_layout(nav);
    const struct inkcell_fb_keyboard grid = {
        .keyboard = &nav->kb,
        .layout = &kb_layout,
        .submit_icon = mesh_ui_nav_kb_submit_finishes(nav) ? INKCELL_ICON_CHECK : INKCELL_ICON_SEND,
    };
    inkcell_fb_draw_keyboard(state, layout, y, &grid);
}

/*
 * The Find keyboard: the query is typed into the heading rather than into a box under it.
 *
 * Every other keyboard here is writing something that goes somewhere - a packet, a field on the
 * radio - and its box says so, with a counter against the limit at the other end. A find goes
 * nowhere; it narrows a list, and a search field in the bar is the shape every platform gives
 * that. The box's two lines are the grid's to use where the body is short of room for its keys -
 * a large glyph scale, a small panel - and slack above it where it is not.
 *
 * The capsule beside the field is what the typing has done so far: how many nodes it keeps,
 * through the filter the list is on, so the number is the one the list will show once Done is
 * pressed. The list itself is under the keys and cannot be seen, so without it the reader typed
 * blind and found out on the way back whether any of it had matched. Nothing is counted for an
 * empty field, which is not a search yet.
 */
static void fb_render_node_search(const struct inkcell_draw_state *state,
                                  const struct mesh_ui_snapshot *snapshot,
                                  struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const bool typed = nav->draft[0] != '\0';
    const uint32_t matches =
        typed ? mesh_ui_node_query_count(&snapshot->handshake,
                                         (enum mesh_ui_node_filter)nav->node_filter, nav->draft)
              : 0U;
    char badge[32] = "";
    if (typed) {
        inkcell_str_format_plural(badge, sizeof badge, MESH_STR_NODES_FIND_MATCHES_ONE, matches,
                                  matches);
    }
    fb_draw_app_bar(
        state, layout,
        &(const struct inkcell_fb_app_bar){
            .mode = INKCELL_FB_APP_BAR_SEARCH,
            .query = nav->draft,
            .placeholder = inkcell_str(MESH_STR_NODES_FIND_PROMPT),
            .editing = true,
            .caret_back = nav->kb.caret_back,
            .badge = typed ? badge : NULL,
            /* Nothing kept is the one count worth colouring: it is the reader's cue
               to take a letter back before Done empties the list. */
            .badge_family = typed && matches == 0U ? INKCELL_FAMILY_ERROR : INKCELL_FAMILY_PRIMARY,
        });

    int y = layout->body_y;
    /* A window types on its own keyboard and draws no grid (see fb_render_keyboard()), so the
       body is free - and says so in words when the typing has matched nothing. */
    if (state->pointer) {
        if (typed && matches == 0U) {
            inkcell_fb_draw_empty(state, layout, INKCELL_ICON_SEARCH,
                                  inkcell_str(MESH_STR_NODES_FIND_NONE));
        }
        return;
    }
    fb_draw_keyboard_grid(state, nav, layout, &y);
}

/* The on-screen keyboard takes the whole body: target, the draft so far, then the grid. */
void fb_render_keyboard(const struct inkcell_draw_state *state,
                        const struct mesh_ui_snapshot *snapshot, struct inkcell_fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    if (mesh_ui_nav_kb_node_search(nav)) {
        fb_render_node_search(state, snapshot, layout);
        return;
    }
    const bool for_passkey = nav->keyboard_passkey;
    const bool for_verify = (!for_passkey && nav->keyboard_verify);
    const bool for_setting =
        (!for_passkey && !for_verify && nav->keyboard_field != MESH_UI_FIELD_NONE);
    /* Asked rather than worked out again. The nav caps what the append will actually take, and
       this counter is the only thing on the frame that says a cap exists - so a copy of the
       rule here is a promise the typing does not keep. It was one: a place's name and a network
       address are cut at 29 and 63 bytes under a counter that said 233. */
    const size_t draft_cap = mesh_ui_nav_draft_cap(nav);
    char title[96];
    if (for_passkey) {
        /* The one prompt the user cannot act on without being told what to look at: the digits
           are on the node's own screen, not anywhere on this one. */
        inkcell_str_format(
            title, sizeof title,
            nav->pairing_confirm ? MESH_STR_PAIRING_CONFIRM : MESH_STR_PAIRING_SHOWN_ON,
            nav->pairing_label[0] != '\0' ? nav->pairing_label
                                          : inkcell_str(MESH_STR_PAIRING_NODE_FALLBACK));
    } else if (for_verify) {
        /* The pairing prompt's problem exactly: the digits are on somebody else's screen and
           reach this one by voice, so the heading has to say whose. The name comes out of the
           snapshot rather than off the nav - it is the name the far radio used, which is the
           one the other person is looking at while they read the number out. */
        inkcell_str_format(title, sizeof title, MESH_STR_VERIFY_NUMBER_PROMPT,
                           snapshot->verification.remote_name[0] != '\0'
                               ? snapshot->verification.remote_name
                               : inkcell_str(MESH_STR_COMMON_UNKNOWN));
    } else if (for_setting) {
        snprintf(title, sizeof title, "%s",
                 mesh_ui_settings_field_label((enum mesh_ui_setting_field)nav->keyboard_field));
    } else if (nav->keyboard_network) {
        /* Each of the other two jobs names what it is editing, and these two fell through to
           the compose heading - so naming an address stood under "To: #LongFast", which is the
           destination of a message nobody is writing. A heading says what this keyboard is for
           or it is worse than none. */
        snprintf(title, sizeof title, "%s", inkcell_str(MESH_STR_DEVICES_NETWORK_ROW));
    } else if (nav->keyboard_waypoint) {
        snprintf(title, sizeof title, "%s", inkcell_str(MESH_STR_WAYPOINTS_NEW));
    } else if (nav->keyboard_channel_url) {
        /* The network address's problem exactly, and it had it: a link keyboard fell through to
           the compose heading, so two hundred characters of base64 were typed under "To:
           #LongFast" - the destination of a message nobody is writing. */
        snprintf(title, sizeof title, "%s", inkcell_str(MESH_STR_IMPORT_PROMPT));
    } else if (nav->keyboard_contact_url) {
        snprintf(title, sizeof title, "%s", inkcell_str(MESH_STR_CONTACT_IMPORT_PROMPT));
    } else {
        inkcell_str_format(title, sizeof title, MESH_STR_COMPOSE_TO, nav->target_name);
    }
    /* The same badge the compose sheet carries, for the same reason: this keyboard was raised
       over a bubble, and the destination in the title is not what says so. A setting's keyboard
       and the pairing prompt never carry one - `reply_to` belongs to the thread. */
    const bool replying = (!for_passkey && !for_verify && !for_setting && !nav->keyboard_network &&
                           !nav->keyboard_waypoint && !nav->keyboard_channel_url &&
                           !nav->keyboard_contact_url && nav->reply_to != 0U);
    fb_draw_app_bar(state, layout,
                    &(const struct inkcell_fb_app_bar){
                        .title = title,
                        .badge = replying ? inkcell_str(MESH_STR_COMPOSE_BADGE_REPLY) : NULL,
                        .badge_family = INKCELL_FAMILY_TERTIARY,
                    });

    int y = layout->body_y;

    /*
     * What is being typed, in the one text field this UI has. Everything about how it is drawn
     * - the raised tier that lifts it off the keyboard, the panel shape, the edge, which tail
     * of an overlong draft to show and where the counter sits - belongs to the component; what
     * is left here is the two facts only this screen knows, which is what it holds and how full
     * it is.
     */
    char meter[32];
    snprintf(meter, sizeof meter, "%zu/%zu", strlen(nav->draft), draft_cap);
    struct inkcell_fb_text_field field = {
        .value = nav->draft,
        .caret = true,
        .caret_back = nav->kb.caret_back,
        .lines = 2U,
        .counter = meter,
    };

    /*
     * A window has a keyboard of its own. The grid below is how a d-pad types, and on a desktop
     * it is a picture of the keyboard the reader's hands are already on - so a frame drawn for a
     * pointer (the SDL window, the only backend that sets it) leaves the grid out and gives its
     * room to the field. The characters arrive as text from the window (inkcell/ui/sdl.h), with
     * Return, Backspace and Escape as the grid's submit, delete and back; an emoji comes from
     * the system's own picker. The field grows by whole lines so its height still does not
     * change as somebody types.
     */
    if (state->pointer) {
        const int two = inkcell_fb_text_field_height(state, layout, &field);
        field.lines = 3U;
        const int line = inkcell_fb_text_field_height(state, layout, &field) - two;
        field.lines = 2U;
        const int room = layout->footer_y - y;
        if (line > 0 && room > two) {
            const uint32_t fits = 2U + (uint32_t)((room - two) / line);
            field.lines = fits < MESH_UI_KB_FIELD_LINES_MAX ? fits : MESH_UI_KB_FIELD_LINES_MAX;
        }
        inkcell_fb_draw_text_field(state, layout, &y, &field);
        return;
    }
    inkcell_fb_draw_text_field(state, layout, &y, &field);
    fb_draw_keyboard_grid(state, nav, layout, &y);
}
