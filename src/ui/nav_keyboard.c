#define _POSIX_C_SOURCE 200809L

/*
 * The on-screen keyboard and the draft it edits.
 *
 * One grid of characters driven by the d-pad, in three layers. It is opened for two unrelated
 * jobs - typing a message and typing a settings field - plus the BlueZ passkey prompt, which
 * can arrive on top of either; mesh_ui_nav_keyboard_close() is where "give the user back what
 * they were doing" lives, and is the reason that function is longer than it looks like it
 * should be.
 */

#include "nav_internal.h"

#include "mesh/ui/settings.h"
#include "mesh/utils/array.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keyboard layers. Each string is one row of MESH_UI_KB_COLS cells. */
static const char *const k_kb_layers[MESH_UI_KB_LAYER_COUNT][MESH_UI_KB_CHAR_ROWS] = {
    {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM!-:"},
    {"!@#$%^&*()", "-_=+[]{}<>", ";:'\"/\\|`~", ",.?!@#&%*+"},
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
            return "ABC";
        }
        if (nav != NULL && nav->kb_layer == MESH_UI_KB_UPPER) {
            return "#+=";
        }
        return "abc";
    case MESH_UI_KB_ACTION_SPACE:
        return "space";
    case MESH_UI_KB_ACTION_DELETE:
        return "del";
    case MESH_UI_KB_ACTION_SEND:
        return (nav != NULL && nav->keyboard_field != MESH_UI_FIELD_NONE) ? "done" : "send";
    case MESH_UI_KB_ACTION_CANCEL:
        return "cancel";
    default:
        return "";
    }
}

/* ---- keyboard ----------------------------------------------------------------------------- */

/* The most bytes the draft may hold: the message limit, or the field's cap when the keyboard
   is editing a setting. */
static size_t mesh_ui_nav_draft_cap(const struct mesh_ui_nav *nav) {
    if (nav->keyboard_passkey) {
        /* A seventh digit could only ever be a passkey BlueZ rejects out of range. */
        return MESH_UI_PASSKEY_DIGITS;
    }
    if (nav->keyboard_field != MESH_UI_FIELD_NONE) {
        const uint32_t cap =
            mesh_ui_settings_text_max((enum mesh_ui_setting_field)nav->keyboard_field);
        return cap < MESH_UI_DRAFT_MAX - 1U ? cap : MESH_UI_DRAFT_MAX - 1U;
    }
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
    if (nav->keyboard_passkey) {
        nav->keyboard_passkey = false;
        nav->pairing_confirm = false;
        nav->pairing_label[0] = '\0';
        snprintf(nav->draft, sizeof nav->draft, "%s", nav->draft_saved);
        nav->draft_saved[0] = '\0';
        /* The prompt landed on an open keyboard: give it back rather than dropping the user
           out of what they were editing. */
        if (nav->keyboard_field_displaced != MESH_UI_FIELD_NONE) {
            nav->keyboard_field = nav->keyboard_field_displaced;
            nav->keyboard_field_displaced = MESH_UI_FIELD_NONE;
            nav->keyboard_open = true;
            nav->screen = MESH_UI_SCREEN_SETTINGS;
        }
        return;
    }
    if (nav->keyboard_field != MESH_UI_FIELD_NONE) {
        nav->keyboard_field = MESH_UI_FIELD_NONE;
        snprintf(nav->draft, sizeof nav->draft, "%s", nav->draft_saved);
        nav->draft_saved[0] = '\0';
        nav->screen = MESH_UI_SCREEN_SETTINGS;
    }
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
        snprintf(action->text, sizeof action->text, "%s", nav->draft);
    }
    nav->draft[0] = '\0';
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

/* Cancel on the PIN prompt, and B with nothing left to delete: the bond is abandoned. */
static bool mesh_ui_nav_cancel_passkey(struct mesh_ui_nav *nav, struct mesh_ui_action *action) {
    if (action != NULL) {
        action->type = MESH_UI_ACTION_CANCEL_PAIRING;
    }
    nav->draft[0] = '\0';
    mesh_ui_nav_keyboard_close(nav);
    return true;
}

bool mesh_ui_nav_keyboard_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                              enum mesh_ui_key key, struct mesh_ui_action *action) {
    const bool for_passkey = nav->keyboard_passkey;
    const bool for_setting = (!for_passkey && nav->keyboard_field != MESH_UI_FIELD_NONE);
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
            return for_setting ? mesh_ui_nav_settings_commit_text(nav, store)
                               : mesh_ui_nav_send_draft(nav, action);
        case MESH_UI_KB_ACTION_CANCEL:
            if (for_passkey) {
                return mesh_ui_nav_cancel_passkey(nav, action);
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
        return for_setting ? mesh_ui_nav_settings_commit_text(nav, store)
                           : mesh_ui_nav_send_draft(nav, action);
    case MESH_UI_KEY_SELECT:
    case MESH_UI_KEY_NONE:
    default:
        return false;
    }
}
