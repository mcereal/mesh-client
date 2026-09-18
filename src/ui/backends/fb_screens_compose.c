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

#include "fb_widgets.h"

#include "fb_screens_internal.h"

#include "mesh/core/message.h"
#include "mesh/i18n/strings.h"
#include "mesh/ui/layout.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/reactions.h"
#include "mesh/ui/settings.h"

#include <stdio.h>
#include <string.h>

/*
 * The bubble sheet: the fixed emoji set one per row, then the delete, aimed at the message X
 * was pressed on.
 *
 * The message it is about is the heading rather than a row, for the reason the compose sheet's
 * destination is: nothing on this screen chooses it, so a row that looked pressable would be
 * offering a choice that is already made. It is also what makes the delete safe to put here -
 * the thing about to be thrown away is quoted at the top of the screen it is thrown away from.
 */
void fb_render_reactions(struct mesh_ui_backend_fb_state *state,
                         const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    char target[96] = {0};
    fb_thread_quote(mesh_ui_snapshot_message_view(snapshot), nav->reply_to, target, sizeof target);
    char title[160];
    mesh_str_format(title, sizeof title, MESH_STR_MESSAGE_ACTIONS, target);
    fb_draw_app_bar(state, layout, &(const struct fb_app_bar){.title = title});

    const uint32_t count = mesh_ui_nav_reaction_row_count();
    struct fb_list list = fb_list_begin(layout, count, nav->reaction_cursor);
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        /* The delete takes an icon rather than an emoji, and the danger role rather than the
           neutral fill: it is the one row here that destroys something, and the two are told
           apart by what they look like before they are read. */
        if (mesh_ui_nav_reaction_row_is_delete(i)) {
            const struct fb_list_item row = {
                .leading = {.kind = FB_LEADING_ICON,
                            .icon = MESH_UI_ICON_DELETE,
                            .role = MESH_UI_COLOR_ERROR},
                .text = mesh_str(nav->message_delete_armed ? MESH_STR_MESSAGE_DELETE_CONFIRM
                                                           : MESH_STR_MESSAGE_DELETE),
                .divider = true,
            };
            fb_list_item(state, &list, i, &row);
            continue;
        }
        /* The glyph in the leading slot and the word beside it: eight faces in a column at
           this scale are not eight distinguishable things, and a text backend has no sprites
           for any of them. */
        const struct fb_list_item row = {
            .leading = {.kind = FB_LEADING_AVATAR,
                        .label = mesh_ui_reaction_emoji(i),
                        /* A stated neutral fill rather than a tint: an avatar's colour is how
                           the eye tells one node from another, and here the glyph inside it is
                           already the whole of what the row is. */
                        .role = MESH_UI_COLOR_SURFACE_SEL},
            .text = mesh_str(mesh_ui_reaction_label(i)),
            .divider = true,
        };
        fb_list_item(state, &list, i, &row);
    }
}

/* Compose overlay: it writes to the open thread, so the destination is a heading rather than
   an editable row. */
void fb_render_compose(struct mesh_ui_backend_fb_state *state,
                       const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    char title[96];
    mesh_str_format(title, sizeof title, MESH_STR_COMPOSE_TO_KIND, nav->target_name,
                    mesh_str(nav->target_node == MESH_MESSAGE_BROADCAST_ADDR
                                 ? MESH_STR_COMPOSE_SUFFIX_CHANNEL
                                 : MESH_STR_COMPOSE_SUFFIX_DIRECT));
    /* The badge is the one thing on the sheet that says this answers a message rather than the
       conversation - which is the whole difference between the press that opened it and Y. */
    fb_draw_app_bar(
        state, layout,
        &(const struct fb_app_bar){
            .title = title,
            .badge = nav->reply_to != 0U ? mesh_str(MESH_STR_COMPOSE_BADGE_REPLY) : NULL,
            .badge_family = MESH_UI_FAMILY_TERTIARY,
        });

    struct fb_list list =
        fb_list_begin(layout, mesh_ui_nav_compose_row_count(), nav->compose_cursor);
    struct mesh_ui_line line;
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        const bool is_draft = (i == MESH_UI_COMPOSE_ROW_DRAFT);
        mesh_ui_line_reset(&line);
        /* The draft is the row that opens the keyboard and the rest are texts to send as they
           stand, which is a difference in kind rather than in indentation - so it is the
           accent tone and the accent edge that say so, and the two spaces the canned rows used
           to be pushed over by are gone. */
        if (is_draft) {
            if (nav->draft[0] != '\0') {
                mesh_ui_line_str(&line, MESH_STR_COMPOSE_DRAFT, nav->draft);
            } else {
                mesh_ui_line_printf(&line, "%s", mesh_str(MESH_STR_COMPOSE_DRAFT_EMPTY));
            }
        } else {
            mesh_ui_line_printf(&line, "%s", mesh_ui_canned_text(i - MESH_UI_COMPOSE_FIRST_CANNED));
        }
        const struct fb_list_item row = {
            .text = mesh_ui_line_text(&line),
            /* A started-but-unsent message is in flight, which is the tertiary - and the
               edge bar follows the tone, so the row is marked in the colour of the reason it
               is marked. */
            .tone = is_draft ? MESH_UI_TONE_TERTIARY : MESH_UI_TONE_NORMAL,
            .accent_edge = is_draft,
            .divider = true,
        };
        fb_list_item(state, &list, i, &row);
    }
}

/* "Send to" list: channels, then nodes, cursor on the current target. Mutable state, like
   every fb_list_item() caller. */
void fb_render_picker(struct mesh_ui_backend_fb_state *state,
                      const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    struct mesh_ui_store view;
    mesh_ui_store_view(snapshot, &view);

    const uint32_t count = mesh_ui_nav_picker_count(&view);
    char title[96];
    fb_title_count(title, sizeof title, mesh_str(MESH_STR_PICKER_TITLE), count, 0U);
    fb_draw_app_bar(state, layout, &(const struct fb_app_bar){.title = title});
    if (count == 0U) {
        fb_draw_empty(state, layout, MESH_UI_ICON_MESSAGES, mesh_str(MESH_STR_PICKER_EMPTY));
        return;
    }

    struct fb_list list = fb_list_begin(layout, count, nav->picker_cursor);
    char name[96];
    char initials[MESH_UI_CONVERSATION_INITIALS_MAX];
    uint32_t i;
    while (fb_list_next(&list, &i)) {
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
        struct fb_selection sel = {
            .id = 0x04000000U | i,
            .on = current,
        };
        const struct fb_list_item row = {
            .leading =
                {
                    .kind = FB_LEADING_AVATAR,
                    .label = initials,
                    /* A channel's disc carries the tag rather than the '#' the nav layer hands
                       back, which is what the conversation list puts in the same disc. */
                    .icon = is_channel ? MESH_UI_ICON_CHANNEL : MESH_UI_ICON_NONE,
                    .tint = tint,
                    .role = MESH_UI_COLOR_COUNT,
                },
            .text = name,
            .tone = is_channel ? MESH_UI_TONE_PRIMARY : MESH_UI_TONE_NORMAL,
            .trailing = {.kind = FB_TRAILING_RADIO, .sel = &sel},
            .divider = true,
        };
        fb_list_item(state, &list, i, &row);
    }
}

/* The on-screen keyboard takes the whole body: target, the draft so far, then the grid. */
void fb_render_keyboard(const struct mesh_ui_backend_fb_state *state,
                        const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
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
        mesh_str_format(title, sizeof title,
                        nav->pairing_confirm ? MESH_STR_PAIRING_CONFIRM : MESH_STR_PAIRING_SHOWN_ON,
                        nav->pairing_label[0] != '\0' ? nav->pairing_label
                                                      : mesh_str(MESH_STR_PAIRING_NODE_FALLBACK));
    } else if (for_verify) {
        /* The pairing prompt's problem exactly: the digits are on somebody else's screen and
           reach this one by voice, so the heading has to say whose. The name comes out of the
           snapshot rather than off the nav - it is the name the far radio used, which is the
           one the other person is looking at while they read the number out. */
        mesh_str_format(title, sizeof title, MESH_STR_VERIFY_NUMBER_PROMPT,
                        snapshot->verification.remote_name[0] != '\0'
                            ? snapshot->verification.remote_name
                            : mesh_str(MESH_STR_COMMON_UNKNOWN));
    } else if (for_setting) {
        snprintf(title, sizeof title, "%s",
                 mesh_ui_settings_field_label((enum mesh_ui_setting_field)nav->keyboard_field));
    } else if (nav->keyboard_network) {
        /* Each of the other two jobs names what it is editing, and these two fell through to
           the compose heading - so naming an address stood under "To: #LongFast", which is the
           destination of a message nobody is writing. A heading says what this keyboard is for
           or it is worse than none. */
        snprintf(title, sizeof title, "%s", mesh_str(MESH_STR_DEVICES_NETWORK_ROW));
    } else if (nav->keyboard_waypoint) {
        snprintf(title, sizeof title, "%s", mesh_str(MESH_STR_WAYPOINTS_NEW));
    } else if (nav->keyboard_channel_url) {
        /* The network address's problem exactly, and it had it: a link keyboard fell through to
           the compose heading, so two hundred characters of base64 were typed under "To:
           #LongFast" - the destination of a message nobody is writing. */
        snprintf(title, sizeof title, "%s", mesh_str(MESH_STR_IMPORT_PROMPT));
    } else if (nav->keyboard_contact_url) {
        snprintf(title, sizeof title, "%s", mesh_str(MESH_STR_CONTACT_IMPORT_PROMPT));
    } else {
        mesh_str_format(title, sizeof title, MESH_STR_COMPOSE_TO, nav->target_name);
    }
    /* The same badge the compose sheet carries, for the same reason: this keyboard was raised
       over a bubble, and the destination in the title is not what says so. A setting's keyboard
       and the pairing prompt never carry one - `reply_to` belongs to the thread. */
    const bool replying = (!for_passkey && !for_verify && !for_setting && !nav->keyboard_network &&
                           !nav->keyboard_waypoint && !nav->keyboard_channel_url &&
                           !nav->keyboard_contact_url && nav->reply_to != 0U);
    fb_draw_app_bar(state, layout,
                    &(const struct fb_app_bar){
                        .title = title,
                        .badge = replying ? mesh_str(MESH_STR_COMPOSE_BADGE_REPLY) : NULL,
                        .badge_family = MESH_UI_FAMILY_TERTIARY,
                    });

    const int scale = state->scale;
    const int line = layout->line;
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
    const struct fb_text_field field = {
        .value = nav->draft,
        .caret = true,
        .lines = 2U,
        .counter = meter,
    };
    fb_draw_text_field(state, layout, &y, &field);

    /* The character grid and the action row are the same button, sized differently. */
    const int margin = fb_margin(state);
    const int grid_w = (int)state->var.xres - 2 * margin;
    const int cell_w = grid_w / (int)MESH_UI_KB_COLS;

    /*
     * A key is as tall as the body has room for, not as tall as one line of text.
     *
     * The five rows used to cost a text line each and stop, which left a third of the panel
     * blank under a keyboard whose keys were a tenth of the panel wide and a twentieth of it
     * tall. Nothing on this screen wanted that space - the draft above is two lines by design -
     * and the keys are the one thing on the client aimed at with a d-pad rather than read, so
     * the space is theirs. The cap is the key's own width: past square a keycap stops looking
     * like a key, and the row of characters starts reading as a column of bars.
     *
     * The floor is what the row cost before, so a shorter body - a smaller panel, a larger
     * glyph scale, the passkey prompt's two-line heading - lays the grid out exactly as it
     * always did rather than crushing it.
     */
    const int kb_rows = (int)MESH_UI_KB_CHAR_ROWS + 1; /* the action row is the fifth */
    const int floor_h = line + fb_space(state, MESH_UI_SPACE_MD);
    int cell_h = (layout->footer_y - y) / kb_rows;
    if (cell_h > cell_w) {
        cell_h = cell_w;
    }
    if (cell_h < floor_h) {
        cell_h = floor_h;
    }

    /* Whatever the cap leaves over goes above the grid rather than below it: a keyboard sits at
       the bottom of what it is given, here as on every other device, and the slack under the
       draft reads as the gap over a keyboard rather than as a keyboard that stopped short. When
       there is no slack - and when the floor means the grid is taller than the room, which is a
       small panel at a large glyph scale - it starts where the draft left off, as before. */
    if (layout->footer_y - kb_rows * cell_h > y) {
        y = layout->footer_y - kb_rows * cell_h;
    }

    /*
     * And the keycap's text grows with it, up to what the glyph scale can express.
     *
     * A letter is legible at the body scale, so this is not the emoji layer's problem over
     * again - but a 24 px "q" adrift in a 99 px key reads as a key with nothing on it, and the
     * scale a keycap wants is simply the tallest line the key has room for. Derived from what
     * one step costs rather than stepped up in a loop.
     *
     * Clamped at three points: never below the body scale, never past the largest multiplier
     * the glyph cache is sized for, and never more than twice the text it is typing - a reader
     * who has asked for small text has asked for it, and a keycap three times the draft under
     * it would be the screen arguing with them. At the device's own scale it is
     * MESH_UI_SCALE_MAX that bites rather than the multiplier.
     */
    const int step = fb_line_adv(state, 1);
    int key_scale = step > 0 ? (cell_h - 2 * fb_space(state, MESH_UI_SPACE_MD)) / step : scale;
    const int key_scale_max = scale * 2 < MESH_UI_SCALE_MAX ? scale * 2 : MESH_UI_SCALE_MAX;
    if (key_scale < scale) {
        key_scale = scale;
    }
    if (key_scale > key_scale_max) {
        key_scale = key_scale_max;
    }
    for (unsigned row = 0; row < MESH_UI_KB_CHAR_ROWS; ++row) {
        for (unsigned col = 0; col < MESH_UI_KB_COLS; ++col) {
            /* The cell rather than the character: three of the four layers are one ASCII byte
               and the fourth is an emoji, which is four of them and one keycap. What the button
               does with either is its own business - see `emoji_face` below. */
            char scratch[MESH_UI_KB_CELL_MAX];
            const char *const key = mesh_ui_kb_cell(nav, row, col, scratch);
            const struct fb_button button = {
                .rect = {.x = margin + (int)col * cell_w,
                         .y = y,
                         .w = cell_w - scale,
                         .h = cell_h - scale},
                .label = key,
                .selected = (nav->kb_row == row && nav->kb_col == col),
                .variant = FB_BUTTON_TEXT,
                .shape = MESH_UI_SHAPE_SM,
                .idle_tone = MESH_UI_TONE_NORMAL,
                .scale = key_scale,
                /* Only the emoji layer's cells are sprites, and the button ignores this for
                   the three that are not - so the grid is described once for all four. */
                .emoji_face = true,
            };
            fb_draw_button(state, &button);
        }
        y += cell_h;
    }

    const int action_w = grid_w / (int)MESH_UI_KB_ACTIONS;
    for (unsigned col = 0; col < MESH_UI_KB_ACTIONS; ++col) {
        const enum mesh_ui_kb_action action = (enum mesh_ui_kb_action)col;
        /*
         * Four of the five keys are symbols, which is what a soft keyboard's action row is on
         * every platform: the words for them ("space", "delete", "send") are among the longest
         * strings in the catalog and these are the narrowest boxes on the screen. The layer key
         * keeps its label, because what it says - "ABC", "abc", "#!" - is the layer it switches
         * to, and no symbol carries that.
         */
        const enum mesh_ui_icon icon =
            action == MESH_UI_KB_ACTION_SPACE    ? MESH_UI_ICON_SPACE
            : action == MESH_UI_KB_ACTION_DELETE ? MESH_UI_ICON_BACKSPACE
            : action == MESH_UI_KB_ACTION_CANCEL ? MESH_UI_ICON_CLOSE
            : action == MESH_UI_KB_ACTION_SEND
                ? ((nav->keyboard_field != MESH_UI_FIELD_NONE || nav->keyboard_waypoint ||
                    nav->keyboard_network || nav->keyboard_channel_url || nav->keyboard_contact_url)
                       ? MESH_UI_ICON_CHECK
                       : MESH_UI_ICON_SEND)
                : MESH_UI_ICON_NONE;
        const struct fb_button button = {
            .rect = {.x = margin + (int)col * action_w,
                     .y = y,
                     .w = action_w - scale,
                     .h = cell_h - scale},
            .icon = icon,
            .label = icon == MESH_UI_ICON_NONE ? mesh_ui_kb_action_label(nav, action) : "",
            .selected = (nav->kb_row == MESH_UI_KB_CHAR_ROWS && nav->kb_col == col),
            .variant = FB_BUTTON_FILLED,
            .shape = MESH_UI_SHAPE_SM,
            .idle_tone = MESH_UI_TONE_NORMAL,
            .scale = key_scale,
        };
        fb_draw_button(state, &button);
    }
}
