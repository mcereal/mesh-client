#define _POSIX_C_SOURCE 200809L

/*
 * The on-screen keyboard and the draft it edits.
 *
 * One grid of characters driven by the d-pad, in three layers. It is opened for four unrelated
 * jobs - typing a message, typing a settings field, naming a new waypoint and typing the network
 * radio's address - plus the BlueZ passkey prompt, which can arrive on top of any of them;
 * mesh_ui_nav_keyboard_close() is where "give the user back what they were doing" lives, and is
 * the reason that function is longer than it looks like it should be.
 */

#include "nav_internal.h"

#include "mesh/ui/channel_share.h"
#include "mesh/ui/settings.h"
#include "mesh/utils/array.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Keyboard layers, one row of MESH_UI_KB_COLS cells each.
 *
 * A fixed-width array rather than a row of pointers, and that is the invariant rather than a
 * formatting choice: the grid draws a key per column whatever the string holds, so a row one
 * character short is a blank keycap the cursor stops on and A does nothing to - the press that
 * does nothing this client refuses everywhere else. Declared this way the compiler rejects a
 * row too long for the grid, and a row too short is NUL-padded rather than read past its own
 * terminator; kb_layers_fill_the_grid is what catches the short one.
 */
static const char k_kb_layers[MESH_UI_KB_LAYER_COUNT][MESH_UI_KB_CHAR_ROWS][MESH_UI_KB_COLS + 1U] =
    {
        {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"},
        {"1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM!-:"},
        /* The symbols layer's quotes-and-slashes row ends in '=' because it was nine cells
           long and the tenth drew empty; '=' is the one piece of URL punctuation the rest of
           the row does not already carry, and it was otherwise a layer away on the row above. */
        {"!@#$%^&*()", "-_=+[]{}<>", ";:'\"/\\|`~=", ",.?!@#&%*+"},
};

char mesh_ui_kb_char(enum mesh_ui_kb_layer layer, unsigned row, unsigned col) {
    if (layer >= MESH_UI_KB_LAYER_COUNT || row >= MESH_UI_KB_CHAR_ROWS || col >= MESH_UI_KB_COLS) {
        return '\0';
    }
    return k_kb_layers[layer][row][col];
}

const char *mesh_ui_kb_action_label(const struct mesh_ui_nav *nav, enum mesh_ui_kb_action action) {
    switch (action) {
    case MESH_UI_KB_ACTION_LAYER:
        if (nav != NULL && nav->kb_layer == MESH_UI_KB_LOWER) {
            return mesh_str(MESH_STR_KEY_LAYER_UPPER);
        }
        if (nav != NULL && nav->kb_layer == MESH_UI_KB_UPPER) {
            return mesh_str(MESH_STR_KEY_LAYER_SYMBOLS);
        }
        return mesh_str(MESH_STR_KEY_LAYER_LOWER);
    case MESH_UI_KB_ACTION_SPACE:
        return mesh_str(MESH_STR_KEY_SPACE);
    case MESH_UI_KB_ACTION_DELETE:
        return mesh_str(MESH_STR_KEY_DELETE);
    case MESH_UI_KB_ACTION_SEND:
        /* "Send" only when something goes to a person. A settings field is finished, and so is
           a waypoint's name - the place is shared by the app afterwards, not by this key - and
           so is an address, which is a link brought up rather than anything put on the air. A
           security number is the sharpest case of the same rule: it goes to the radio in the
           user's hand, and a keycap saying "Send" over four digits the whole ceremony depends
           on staying off the mesh would be teaching exactly the wrong thing. */
        return mesh_str((nav != NULL && (nav->keyboard_field != MESH_UI_FIELD_NONE ||
                                         nav->keyboard_waypoint || nav->keyboard_network ||
                                         nav->keyboard_verify || nav->keyboard_channel_url))
                            ? MESH_STR_KEY_DONE
                            : MESH_STR_KEY_SEND);
    case MESH_UI_KB_ACTION_CANCEL:
        return mesh_str(MESH_STR_KEY_CANCEL);
    default:
        return "";
    }
}

/* ---- keyboard ----------------------------------------------------------------------------- */

size_t mesh_ui_nav_draft_cap(const struct mesh_ui_nav *nav) {
    if (nav == NULL) {
        return MESH_UI_DRAFT_MAX - 1U;
    }
    if (nav->keyboard_passkey) {
        /* A seventh digit could only ever be a passkey BlueZ rejects out of range. */
        return MESH_UI_PASSKEY_DIGITS;
    }
    if (nav->keyboard_verify) {
        /* The firmware generates exactly four; a fifth is a mistype, and refusing it where it
           is typed beats sending a number the radio will not match. */
        return MESH_UI_VERIFY_DIGITS_MAX;
    }
    if (nav->keyboard_field != MESH_UI_FIELD_NONE) {
        const uint32_t cap =
            mesh_ui_settings_text_max((enum mesh_ui_setting_field)nav->keyboard_field);
        return cap < MESH_UI_DRAFT_MAX - 1U ? cap : MESH_UI_DRAFT_MAX - 1U;
    }
    if (nav->keyboard_waypoint) {
        /* Upstream's own limit on Waypoint.name, which is its generated buffer less the NUL -
           see mesh/core/waypoint.h. A thirtieth character is one nanopb's own encoder drops, so
           it is refused where it is typed instead. */
        return MESH_UI_WAYPOINT_NAME_MAX - 1U;
    }
    if (nav->keyboard_network) {
        /* The transport's own limit on a target, which is a full bracketed v6 literal with a
           port on it. A longer one is refused by mesh_tcp_target_split() after the typing, so
           it is refused during the typing instead. */
        return MESH_UI_NETWORK_HOST_MAX - 1U;
    }
    /* A channel link has no cap of its own: what can be typed is the draft, and a link longer
       than that is one nobody was going to type. It is checked when the key is pressed rather
       than as it is typed, because a link is only ever right or wrong as a whole. */
    return MESH_UI_DRAFT_MAX - 1U;
}

static void mesh_ui_nav_draft_append(struct mesh_ui_nav *nav, char ch) {
    const size_t len = strlen(nav->draft);
    if (len + 1U >= sizeof nav->draft || len >= mesh_ui_nav_draft_cap(nav)) {
        return;
    }
    nav->draft[len] = ch;
    nav->draft[len + 1U] = '\0';
}

/* Removes one character. A name preloaded from the radio may hold UTF-8 the keyboard cannot
   type; deleting byte-wise would leave a broken sequence behind. */
static bool mesh_ui_nav_draft_delete(struct mesh_ui_nav *nav) {
    size_t len = strlen(nav->draft);
    if (len == 0U) {
        return false;
    }
    while (len > 1U && ((unsigned char)nav->draft[len - 1U] & 0xC0U) == 0x80U) {
        len--;
    }
    nav->draft[len - 1U] = '\0';
    return true;
}

/* Closing always parks the cursor at the top-left so the next message starts the same way.
   A keyboard opened for a setting returns to that section and restores the Compose draft; one
   opened for a message falls back to the compose overlay it was opened from. */
void mesh_ui_nav_keyboard_close(struct mesh_ui_nav *nav) {
    nav->keyboard_open = false;
    nav->kb_row = 0U;
    nav->kb_col = 0U;
    nav->kb_layer = MESH_UI_KB_LOWER;
    if (nav->keyboard_passkey || nav->keyboard_verify) {
        nav->keyboard_passkey = false;
        nav->keyboard_verify = false;
        nav->pairing_confirm = false;
        nav->pairing_label[0] = '\0';
        snprintf(nav->draft, sizeof nav->draft, "%s", nav->draft_saved);
        nav->draft_saved[0] = '\0';
        /* The prompt landed on an open keyboard: give it back rather than dropping the user
           out of what they were editing. A message keyboard counts - its field is NONE, which
           is why the flag rather than the field says whether there was one, and Y opens one
           with no compose overlay behind it to fall back on. */
        if (nav->keyboard_displaced) {
            nav->keyboard_field = nav->keyboard_field_displaced;
            nav->keyboard_displaced = false;
            nav->keyboard_field_displaced = MESH_UI_FIELD_NONE;
            nav->keyboard_open = true;
            /* Whichever of the three jobs it was doing. `keyboard_waypoint` survives the prompt
               untouched - the passkey branch above never sets it - so it is still true for a
               keyboard that was naming a place, and sending it back to the Messages tab would
               drop the user somewhere they were not. */
            if (nav->keyboard_field != MESH_UI_FIELD_NONE) {
                nav->screen = MESH_UI_SCREEN_SETTINGS;
            } else if (nav->keyboard_waypoint) {
                nav->screen = MESH_UI_SCREEN_WAYPOINTS;
            } else if (nav->keyboard_network) {
                /* Survives the prompt untouched, exactly as `keyboard_waypoint` does - and it
                   is the one flavour a passkey prompt can plausibly land on, since both are
                   raised from the Devices tab. */
                nav->screen = MESH_UI_SCREEN_DEVICES;
            } else {
                nav->screen = MESH_UI_SCREEN_MESSAGES;
            }
        }
        return;
    }
    if (nav->keyboard_field != MESH_UI_FIELD_NONE) {
        nav->keyboard_field = MESH_UI_FIELD_NONE;
        snprintf(nav->draft, sizeof nav->draft, "%s", nav->draft_saved);
        nav->draft_saved[0] = '\0';
        nav->screen = MESH_UI_SCREEN_SETTINGS;
        return;
    }
    if (nav->keyboard_network) {
        nav->keyboard_network = false;
        snprintf(nav->draft, sizeof nav->draft, "%s", nav->draft_saved);
        nav->draft_saved[0] = '\0';
        nav->screen = MESH_UI_SCREEN_DEVICES;
        return;
    }
    if (nav->keyboard_channel_url) {
        nav->keyboard_channel_url = false;
        snprintf(nav->draft, sizeof nav->draft, "%s", nav->draft_saved);
        nav->draft_saved[0] = '\0';
        /* Back to the Channels list the import row is on, which is where the section the sheet
           is about to write to is showing. */
        nav->screen = MESH_UI_SCREEN_SETTINGS;
        return;
    }
    if (nav->keyboard_waypoint) {
        nav->keyboard_waypoint = false;
        nav->waypoint_source_node = 0U;
        snprintf(nav->draft, sizeof nav->draft, "%s", nav->draft_saved);
        nav->draft_saved[0] = '\0';
        /* Back to the list the place was going to appear on, whichever screen raised the
           keyboard - the node detail's "Save this place" row opens it from the Nodes tab, and
           landing back there would leave the user looking for what they just made. */
        nav->screen = MESH_UI_SCREEN_WAYPOINTS;
    }
}

void mesh_ui_nav_open_network_keyboard(struct mesh_ui_nav *nav, const char *host) {
    if (nav == NULL) {
        return;
    }
    /* The Compose draft goes into the one parking slot, exactly as the other two flavours park
       it: there is only ever one keyboard open, and the text in front of the user is the one
       worth keeping. */
    snprintf(nav->draft_saved, sizeof nav->draft_saved, "%s", nav->draft);
    /* Preloaded rather than blank, because editing is the common case: an address is changed
       far more often than it is first written, and retyping fifteen characters on a d-pad to
       correct the last one is not editing. */
    snprintf(nav->draft, sizeof nav->draft, "%s", host != NULL ? host : "");
    nav->keyboard_network = true;
    nav->keyboard_waypoint = false;
    nav->keyboard_field = MESH_UI_FIELD_NONE;
    nav->keyboard_open = true;
    nav->compose_open = false;
    nav->kb_row = 0U;
    nav->kb_col = 0U;
    nav->kb_layer = MESH_UI_KB_LOWER;
    nav->screen = MESH_UI_SCREEN_DEVICES;
}

bool mesh_ui_nav_commit_network_host(struct mesh_ui_nav *nav, struct mesh_ui_action *action) {
    if (nav == NULL) {
        return false;
    }
    if (action != NULL) {
        /*
         * Connect, or forget. Whether the text is an address this client can reach is not this
         * layer's question - the nav has no resolver and no socket - so it goes to the app,
         * which asks the transport and puts the refusal in a toast. That is the same division
         * the rest of the tab follows: a row raises MESH_UI_ACTION_CONNECT and the reason a
         * connect could not happen comes back as words.
         */
        action->type = nav->draft[0] != '\0' ? MESH_UI_ACTION_CONNECT : MESH_UI_ACTION_FORGET;
        action->kind = (uint8_t)MESH_UI_DEVICE_TCP;
        /* mesh_str_copy rather than snprintf: the draft is the message buffer and the
           identifier is a target, so the compiler is right that one does not fit in the other
           - it is mesh_ui_nav_draft_cap() that keeps the two in step, and a bounded copy is
           what says so at the call rather than in a comment. */
        mesh_str_copy(action->identifier, sizeof action->identifier, nav->draft);
    }
    nav->draft[0] = '\0';
    mesh_ui_nav_keyboard_close(nav);
    /* Land on the list the address is about to appear on. */
    nav->screen = MESH_UI_SCREEN_DEVICES;
    return true;
}

/* Sends the draft as-is; empty drafts are ignored. */
static bool mesh_ui_nav_send_draft(struct mesh_ui_nav *nav, struct mesh_ui_action *action) {
    if (nav->draft[0] == '\0') {
        return false;
    }
    if (action != NULL) {
        action->type = MESH_UI_ACTION_SEND_TEXT;
        action->dest = nav->target_node;
        action->channel = nav->target_channel;
        /* Set when the keyboard was raised from the compose sheet A opened on a bubble; 0 when
           Y raised it, which is the difference between "reply" and "write". */
        action->reply_id = nav->reply_to;
        snprintf(action->text, sizeof action->text, "%s", nav->draft);
    }
    nav->draft[0] = '\0';
    nav->reply_to = 0U;
    mesh_ui_nav_keyboard_close(nav);
    /* Land back in the thread it went to, with the compose overlay out of the way. */
    nav->compose_open = false;
    nav->screen = MESH_UI_SCREEN_MESSAGES;
    return true;
}

/* Send on the PIN prompt. The digits go back to the pairing agent; anything the user typed
   that is not a digit is dropped rather than refused, because the prompt is blocking a bond
   and a second chance costs another 30 s of BlueZ. */
static bool mesh_ui_nav_submit_passkey(struct mesh_ui_nav *nav, struct mesh_ui_action *action) {
    char digits[MESH_UI_PASSKEY_DIGITS + 1U];
    size_t len = 0U;
    for (const char *c = nav->draft; *c != '\0' && len < MESH_UI_PASSKEY_DIGITS; ++c) {
        if (*c >= '0' && *c <= '9') {
            digits[len++] = *c;
        }
    }
    digits[len] = '\0';
    if (len == 0U) {
        return false; /* nothing to answer with; leave the prompt up */
    }
    if (action != NULL) {
        action->type = MESH_UI_ACTION_SUBMIT_PASSKEY;
        snprintf(action->text, sizeof action->text, "%s", digits);
    }
    nav->draft[0] = '\0';
    mesh_ui_nav_keyboard_close(nav);
    return true;
}

/*
 * Send on the security-number prompt. Digits only, for the passkey prompt's reason: the
 * keyboard has letters on it and nothing but the four digits means anything to the radio.
 *
 * Short of four is refused rather than sent. Unlike a pairing PIN - where a wrong answer costs
 * another thirty seconds of BlueZ and a second chance is expensive - a half-typed security
 * number costs the *ceremony*: the firmware answers a wrong one by failing the verification,
 * and the two people would have to start again from the beginning.
 */
static bool mesh_ui_nav_submit_verify_number(struct mesh_ui_nav *nav,
                                             struct mesh_ui_action *action) {
    char digits[MESH_UI_VERIFY_DIGITS_MAX + 1U];
    size_t len = 0U;
    for (const char *c = nav->draft; *c != '\0' && len < MESH_UI_VERIFY_DIGITS_MAX; ++c) {
        if (*c >= '0' && *c <= '9') {
            digits[len++] = *c;
        }
    }
    digits[len] = '\0';
    if (len < MESH_UI_VERIFY_DIGITS_MAX) {
        return false; /* not a number yet; leave the prompt up */
    }
    if (action != NULL) {
        action->type = MESH_UI_ACTION_VERIFY_NUMBER;
        snprintf(action->text, sizeof action->text, "%s", digits);
    }
    nav->draft[0] = '\0';
    mesh_ui_nav_keyboard_close(nav);
    return true;
}

/* Cancel on the security-number prompt, and B with nothing left to delete. It stands the
   ceremony down at this end, which is the honest reading of "I am not typing that": the other
   person is holding a number up waiting, and leaving the exchange open would leave them there
   until the radio's own timeout. */
static bool mesh_ui_nav_cancel_verify_number(struct mesh_ui_nav *nav,
                                             struct mesh_ui_action *action) {
    if (action != NULL) {
        action->type = MESH_UI_ACTION_VERIFY_ANSWER;
        action->number = 0U;
    }
    nav->draft[0] = '\0';
    mesh_ui_nav_keyboard_close(nav);
    return true;
}

/* Cancel on the PIN prompt, and B with nothing left to delete: the bond is abandoned. */
static bool mesh_ui_nav_cancel_passkey(struct mesh_ui_nav *nav, struct mesh_ui_action *action) {
    if (action != NULL) {
        action->type = MESH_UI_ACTION_CANCEL_PAIRING;
    }
    nav->draft[0] = '\0';
    mesh_ui_nav_keyboard_close(nav);
    return true;
}

void mesh_ui_nav_open_channel_url_keyboard(struct mesh_ui_nav *nav) {
    if (nav == NULL) {
        return;
    }
    /* The Compose draft into the one parking slot, as every other flavour parks it. Blank
       rather than preloaded: there is nothing to edit here - a link is somebody else's, read
       off a phone, and the last one typed is not a starting point for the next. */
    snprintf(nav->draft_saved, sizeof nav->draft_saved, "%s", nav->draft);
    nav->draft[0] = '\0';
    nav->keyboard_channel_url = true;
    nav->keyboard_network = false;
    nav->keyboard_waypoint = false;
    nav->keyboard_field = MESH_UI_FIELD_NONE;
    nav->keyboard_open = true;
    nav->compose_open = false;
    nav->kb_row = 0U;
    nav->kb_col = 0U;
    nav->kb_layer = MESH_UI_KB_LOWER;
    nav->screen = MESH_UI_SCREEN_SETTINGS;
}

/*
 * Done on the link keyboard: check it, then raise the sheet.
 *
 * The one commit here that can refuse. Every other flavour hands its text on and lets the app
 * decide - an address that does not resolve fails at the socket, a name too long is cut - but a
 * channel link is base64 typed a character at a time on a d-pad, and the honest answer to one
 * wrong character is to leave the user on the keyboard with what they typed rather than throw
 * two hundred characters away and make them start again. mesh_ui_nav_settings_commit_text()
 * refuses a mistyped key the same way and for the same reason.
 */
bool mesh_ui_nav_commit_channel_url(struct mesh_ui_nav *nav) {
    if (nav == NULL) {
        return false;
    }
    if (!mesh_ui_channel_link_valid(nav->draft)) {
        mesh_ui_nav_raise_toast(nav, mesh_str(MESH_STR_TOAST_IMPORT_NOT_A_LINK));
        return true; /* stay on the keyboard so it can be fixed */
    }
    snprintf(nav->channel_url, sizeof nav->channel_url, "%s", nav->draft);
    mesh_ui_nav_keyboard_close(nav);
    /* The sheet, which is where the radio first hears about any of this. Cancel by default, so
       a repeated press on Done cannot join a mesh. */
    nav->confirm_open = true;
    nav->confirm_cursor = 1U;
    nav->confirm_action = (uint8_t)MESH_UI_SETTINGS_ACTION_IMPORT_CHANNELS;
    return true;
}

bool mesh_ui_nav_keyboard_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                              enum mesh_ui_key key, struct mesh_ui_action *action) {
    const bool for_passkey = nav->keyboard_passkey;
    const bool for_verify = (!for_passkey && nav->keyboard_verify);
    const bool for_setting =
        (!for_passkey && !for_verify && nav->keyboard_field != MESH_UI_FIELD_NONE);
    const bool for_waypoint =
        (!for_passkey && !for_verify && !for_setting && nav->keyboard_waypoint);
    const bool for_network =
        (!for_passkey && !for_verify && !for_setting && !for_waypoint && nav->keyboard_network);
    const bool for_link = (!for_passkey && !for_verify && !for_setting && !for_waypoint &&
                           !for_network && nav->keyboard_channel_url);
    switch (key) {
    case MESH_UI_KEY_UP:
    case MESH_UI_KEY_DOWN: {
        const bool was_actions = (nav->kb_row == MESH_UI_KB_CHAR_ROWS);
        if (key == MESH_UI_KEY_UP) {
            nav->kb_row =
                (nav->kb_row == 0U) ? (uint8_t)(MESH_UI_KB_ROWS - 1U) : (uint8_t)(nav->kb_row - 1U);
        } else {
            nav->kb_row = (uint8_t)((nav->kb_row + 1U) % MESH_UI_KB_ROWS);
        }
        /* The action row has five wide keys under ten narrow ones; keep the cursor under
           roughly the same spot when crossing between them. */
        const bool is_actions = (nav->kb_row == MESH_UI_KB_CHAR_ROWS);
        if (!was_actions && is_actions) {
            nav->kb_col = (uint8_t)(nav->kb_col * MESH_UI_KB_ACTIONS / MESH_UI_KB_COLS);
        } else if (was_actions && !is_actions) {
            nav->kb_col = (uint8_t)(nav->kb_col * MESH_UI_KB_COLS / MESH_UI_KB_ACTIONS);
        }
        return true;
    }
    case MESH_UI_KEY_LEFT: {
        const unsigned cols =
            (nav->kb_row == MESH_UI_KB_CHAR_ROWS) ? MESH_UI_KB_ACTIONS : MESH_UI_KB_COLS;
        nav->kb_col = (nav->kb_col == 0U) ? (uint8_t)(cols - 1U) : (uint8_t)(nav->kb_col - 1U);
        return true;
    }
    case MESH_UI_KEY_RIGHT: {
        const unsigned cols =
            (nav->kb_row == MESH_UI_KB_CHAR_ROWS) ? MESH_UI_KB_ACTIONS : MESH_UI_KB_COLS;
        nav->kb_col = (uint8_t)((nav->kb_col + 1U) % cols);
        return true;
    }
    case MESH_UI_KEY_A:
        if (nav->kb_row < MESH_UI_KB_CHAR_ROWS) {
            const char ch =
                mesh_ui_kb_char((enum mesh_ui_kb_layer)nav->kb_layer, nav->kb_row, nav->kb_col);
            if (ch != '\0') {
                mesh_ui_nav_draft_append(nav, ch);
                /* One capital, then back to lower case, like a phone keyboard. */
                if (nav->kb_layer == MESH_UI_KB_UPPER) {
                    nav->kb_layer = MESH_UI_KB_LOWER;
                }
            }
            return true;
        }
        switch ((enum mesh_ui_kb_action)nav->kb_col) {
        case MESH_UI_KB_ACTION_LAYER:
            nav->kb_layer = (uint8_t)((nav->kb_layer + 1U) % MESH_UI_KB_LAYER_COUNT);
            return true;
        case MESH_UI_KB_ACTION_SPACE:
            mesh_ui_nav_draft_append(nav, ' ');
            return true;
        case MESH_UI_KB_ACTION_DELETE:
            return mesh_ui_nav_draft_delete(nav);
        case MESH_UI_KB_ACTION_SEND:
            if (for_passkey) {
                return mesh_ui_nav_submit_passkey(nav, action);
            }
            if (for_verify) {
                return mesh_ui_nav_submit_verify_number(nav, action);
            }
            if (for_setting) {
                return mesh_ui_nav_settings_commit_text(nav, store);
            }
            if (for_network) {
                return mesh_ui_nav_commit_network_host(nav, action);
            }
            if (for_link) {
                return mesh_ui_nav_commit_channel_url(nav);
            }
            return for_waypoint ? mesh_ui_nav_commit_waypoint(nav, action)
                                : mesh_ui_nav_send_draft(nav, action);
        case MESH_UI_KB_ACTION_CANCEL:
            if (for_passkey) {
                return mesh_ui_nav_cancel_passkey(nav, action);
            }
            if (for_verify) {
                return mesh_ui_nav_cancel_verify_number(nav, action);
            }
            nav->draft[0] = '\0';
            mesh_ui_nav_keyboard_close(nav);
            return true;
        default:
            return false;
        }
    case MESH_UI_KEY_B:
    case MESH_UI_KEY_L1:
        /* Backspace; with nothing left to delete, B closes the keyboard and keeps nothing. */
        if (mesh_ui_nav_draft_delete(nav)) {
            return true;
        }
        if (key == MESH_UI_KEY_B) {
            if (for_passkey) {
                return mesh_ui_nav_cancel_passkey(nav, action);
            }
            if (for_verify) {
                return mesh_ui_nav_cancel_verify_number(nav, action);
            }
            mesh_ui_nav_keyboard_close(nav);
            return true;
        }
        return false;
    case MESH_UI_KEY_X:
        nav->kb_layer = (uint8_t)((nav->kb_layer + 1U) % MESH_UI_KB_LAYER_COUNT);
        return true;
    case MESH_UI_KEY_Y:
    case MESH_UI_KEY_R1:
        mesh_ui_nav_draft_append(nav, ' ');
        return true;
    case MESH_UI_KEY_START:
        if (for_passkey) {
            return mesh_ui_nav_submit_passkey(nav, action);
        }
        if (for_verify) {
            return mesh_ui_nav_submit_verify_number(nav, action);
        }
        if (for_setting) {
            return mesh_ui_nav_settings_commit_text(nav, store);
        }
        if (for_network) {
            return mesh_ui_nav_commit_network_host(nav, action);
        }
        if (for_link) {
            return mesh_ui_nav_commit_channel_url(nav);
        }
        return for_waypoint ? mesh_ui_nav_commit_waypoint(nav, action)
                            : mesh_ui_nav_send_draft(nav, action);
    case MESH_UI_KEY_SELECT:
    case MESH_UI_KEY_NONE:
    default:
        return false;
    }
}
