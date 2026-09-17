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
#include "mesh/ui/contact_share.h"
#include "mesh/ui/settings.h"
#include "mesh/utils/array.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The three ASCII layers, one row of MESH_UI_KB_COLS cells each.
 *
 * A fixed-width array rather than a row of pointers, and that is the invariant rather than a
 * formatting choice: the grid draws a key per column whatever the string holds, so a row one
 * character short is a blank keycap the cursor stops on and A does nothing to - the press that
 * does nothing this client refuses everywhere else. Declared this way the compiler rejects a
 * row too long for the grid, and a row too short is NUL-padded rather than read past its own
 * terminator; kb_layers_fill_the_grid is what catches the short one.
 *
 * The fourth layer is emoji and is a table of strings rather than of characters, below.
 */
static const char k_kb_layers[MESH_UI_KB_ASCII_LAYERS][MESH_UI_KB_CHAR_ROWS][MESH_UI_KB_COLS + 1U] =
    {
        {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"},
        {"1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM!-:"},
        /*
         * The symbols layer, arranged by *errand* rather than by code point, because finding
         * a character is the whole difficulty of a grid this size. The shifted number row first,
         * where a hand expects it; brackets and arithmetic next; then the sentence punctuation,
         * where `/` sits beside the comma and the full stop rather than between a backslash and a
         * pipe - a topic and a URL are the two things anybody types here that need it, and it was
         * the one character a user went looking for and gave up on.
         *
         * `'` and `"` are not on this layer and are not missing: they are the tenth cell of the
         * lower and upper layers' home row, where a hand already knows to find them, and the two
         * cells that buys are what `` ` `` and `~` are drawn in. Between the three layers every one
         * of the thirty-two ASCII punctuation marks is reachable, which is what
         * kb_reaches_every_printable_character holds.
         *
         * The digits repeat on the last row, the only repeat on the layer and the one that saves a
         * press rather than spending one: an address, a port and an MQTT topic are digits and
         * punctuation together, and every one of them used to cost a bounce through two layers per
         * character. The row it replaced was ",.?!@#&%*+" - ten cells restating characters already
         * one row above them, which is why the layer looked like it ended at the third.
         */
        {"!@#$%^&*()", "-_=+[]{}<>", ";:`~,.?/\\|", "1234567890"},
};

/*
 * The emoji layer, three pages of forty.
 *
 * Meshtastic names are written by people and a good share of a real roster is emoji already -
 * `User.short_name` is `char[5]`, which is exactly one - so the client could draw them long
 * before it could type one. These are what it can now type.
 *
 * Written as `\U` escapes rather than pasted in, for the reason src/ui/tables/reactions.c states
 * about its own eight: an editor, a terminal or a patch tool that mangles non-ASCII cannot quietly
 * change what this client puts on the air. At a hundred and twenty cells the escape also has to
 * be *readable*, which the raw UTF-8 bytes are not - a code point names itself, and
 * kb_emoji_cells_are_drawable is what checks that each one is a glyph this build actually has a
 * sprite for rather than a box.
 *
 * Every cell is a single code point with no variation selector and no joiner, which is the same
 * rule the tapbacks follow: a glyph spelled two ways is two different glyphs to anything
 * counting them, and the one on the air should be the one the other client recognises.
 */
static const char *const k_kb_emoji[MESH_UI_KB_EMOJI_PAGES][MESH_UI_KB_CHAR_ROWS][MESH_UI_KB_COLS] =
    {
        /* Faces, hands, and what lives out there. */
        {
            /* grinning through smiling */
            {"\U0001F600", "\U0001F603", "\U0001F604", "\U0001F601", "\U0001F606", "\U0001F605",
             "\U0001F602", "\U0001F642", "\U0001F609", "\U0001F60A"},
            /* fond, thinking, tired, upset */
            {"\U0001F60D", "\U0001F618", "\U0001F61C", "\U0001F914", "\U0001F610", "\U0001F634",
             "\U0001F60E", "\U0001F62D", "\U0001F622", "\U0001F631"},
            /* cross, celebrating, unwell, then the hands */
            {"\U0001F621", "\U0001F633", "\U0001F973", "\U0001F912", "\U0001F91D", "\U0001F44D",
             "\U0001F44E", "\U0001F44B", "\U0001F44C", "\U0000270C"},
            /* please, strength, watching, people and what lives out there */
            {"\U0001F64F", "\U0001F4AA", "\U0001F440", "\U0001F9E0", "\U0001F464", "\U0001F46A",
             "\U0001F415", "\U0001F43B", "\U0001F98C", "\U0001F40D"},
        },
        /* Marks, symbols and status. */
        {
            /* affection and occasions */
            {"\U00002764", "\U0001F494", "\U0001F4AF", "\U00002728", "\U00002B50", "\U0001F525",
             "\U0001F389", "\U0001F382", "\U0001F381", "\U0000262E"},
            /* yes, no, careful, asking, and time */
            {"\U00002705", "\U0000274C", "\U000026A0", "\U00002753", "\U00002757", "\U0000203C",
             "\U00002795", "\U00002796", "\U000023F0", "\U0000231B"},
            /* attention, places, and writing */
            {"\U0001F514", "\U0001F515", "\U0001F4CC", "\U0001F4CD", "\U0001F4CE", "\U0001F4DD",
             "\U0001F4D6", "\U0001F4AC", "\U0001F4E3", "\U0001F517"},
            /* locks, eyes, power, and the two arrows */
            {"\U0001F512", "\U0001F513", "\U0001F511", "\U0001F441", "\U0001F4A4", "\U0000267B",
             "\U000026A1", "\U00002622", "\U00002B06", "\U00002B07"},
        },
        /* Outdoors, weather, travel and kit. */
        {
            /* the sky */
            {"\U00002600", "\U000026C5", "\U00002601", "\U000026C8", "\U00002744", "\U0001F30A",
             "\U0001F308", "\U0001F319", "\U0001F31E", "\U0001F30D"},
            /* the ground, and finding your way over it */
            {"\U000026F0", "\U0001F332", "\U0001F335", "\U0001F3DD", "\U0001F3D5", "\U0001F5FA",
             "\U0001F9ED", "\U0001F6A9", "\U0001F6F6", "\U0001F3A3"},
            /* getting there, and who comes when it goes wrong */
            {"\U0001F697", "\U0001F68C", "\U0001F6B2", "\U0001F6FB", "\U0001F681", "\U0001F691",
             "\U0001F692", "\U0001F46E", "\U0001F3E0", "\U0001F3E5"},
            /* the kit, the call for help, and patching it up */
            {"\U0001F4E1", "\U0001F50B", "\U0001F526", "\U0001F6E0", "\U0001F4FB", "\U0001F198",
             "\U0001F6A8", "\U0001F50C", "\U0001F4DE", "\U0001FA79"},
        },
};

char mesh_ui_kb_char(enum mesh_ui_kb_layer layer, unsigned row, unsigned col) {
    if (layer >= MESH_UI_KB_ASCII_LAYERS || row >= MESH_UI_KB_CHAR_ROWS || col >= MESH_UI_KB_COLS) {
        return '\0';
    }
    return k_kb_layers[layer][row][col];
}

const char *mesh_ui_kb_cell(const struct mesh_ui_nav *nav, unsigned row, unsigned col,
                            char scratch[MESH_UI_KB_CELL_MAX]) {
    if (nav == NULL || scratch == NULL || row >= MESH_UI_KB_CHAR_ROWS || col >= MESH_UI_KB_COLS) {
        return "";
    }
    if (nav->kb_layer == MESH_UI_KB_EMOJI) {
        const unsigned page = nav->kb_emoji_page < MESH_UI_KB_EMOJI_PAGES ? nav->kb_emoji_page : 0U;
        return k_kb_emoji[page][row][col];
    }
    scratch[0] = mesh_ui_kb_char((enum mesh_ui_kb_layer)nav->kb_layer, row, col);
    scratch[1] = '\0';
    return scratch;
}

/*
 * Where the cursor is in the ring the layer key and the shoulders walk: the three ASCII layers
 * are panels 0..2 and each page of emoji is a panel after them.
 */
static unsigned kb_panel_index(const struct mesh_ui_nav *nav) {
    if (nav->kb_layer != MESH_UI_KB_EMOJI) {
        return nav->kb_layer;
    }
    const unsigned page = nav->kb_emoji_page < MESH_UI_KB_EMOJI_PAGES ? nav->kb_emoji_page : 0U;
    return (unsigned)MESH_UI_KB_EMOJI + page;
}

void mesh_ui_nav_kb_panel_step(struct mesh_ui_nav *nav, int delta) {
    if (nav == NULL) {
        return;
    }
    const int panels = (int)MESH_UI_KB_PANELS;
    /* Modulo on a negative left operand keeps the sign in C, so the step is made positive
       before it wraps rather than after: L1 from the first panel is the last one. */
    int next = ((int)kb_panel_index(nav) + delta) % panels;
    if (next < 0) {
        next += panels;
    }
    if (next < (int)MESH_UI_KB_EMOJI) {
        nav->kb_layer = (uint8_t)next;
        nav->kb_emoji_page = 0U;
        return;
    }
    nav->kb_layer = (uint8_t)MESH_UI_KB_EMOJI;
    nav->kb_emoji_page = (uint8_t)(next - (int)MESH_UI_KB_EMOJI);
}

void mesh_ui_nav_kb_shift(struct mesh_ui_nav *nav) {
    if (nav == NULL) {
        return;
    }
    nav->kb_emoji_page = 0U;
    nav->kb_layer =
        (uint8_t)(nav->kb_layer == MESH_UI_KB_UPPER ? MESH_UI_KB_LOWER : MESH_UI_KB_UPPER);
}

const char *mesh_ui_kb_action_label(const struct mesh_ui_nav *nav, enum mesh_ui_kb_action action) {
    switch (action) {
    case MESH_UI_KB_ACTION_LAYER:
        /* The key names where it *goes*, which is the only thing about a layer key worth
           drawing - and now that the ring has six panels the emoji pages have to name
           themselves apart, or three presses in a row land on a key that says the same
           thing. */
        if (nav == NULL) {
            return mesh_str(MESH_STR_KEY_LAYER_LOWER);
        }
        switch (nav->kb_layer) {
        case MESH_UI_KB_LOWER:
            return mesh_str(MESH_STR_KEY_LAYER_UPPER);
        case MESH_UI_KB_UPPER:
            return mesh_str(MESH_STR_KEY_LAYER_SYMBOLS);
        case MESH_UI_KB_SYMBOLS:
            return mesh_str(MESH_STR_KEY_LAYER_EMOJI);
        default:
            break;
        }
        return nav->kb_emoji_page + 1U < MESH_UI_KB_EMOJI_PAGES
                   ? mesh_str(MESH_STR_KEY_LAYER_EMOJI_MORE)
                   : mesh_str(MESH_STR_KEY_LAYER_LOWER);
    case MESH_UI_KB_ACTION_SPACE:
        return mesh_str(MESH_STR_KEY_SPACE);
    case MESH_UI_KB_ACTION_DELETE:
        return mesh_str(MESH_STR_KEY_DELETE);
    case MESH_UI_KB_ACTION_SEND:
        /* "Send" only when something goes to a person. A settings field is finished, and so is
           a waypoint's name - the place is shared by the app afterwards, not by this key - and
           so is an address, which is a link brought up rather than anything put on the air. A
           security number is the sharpest case of the same rule: it goes to the radio in the
           user's hand, and a keycap saying "Send" over six digits the whole ceremony depends
           on staying off the mesh would be teaching exactly the wrong thing. */
        return mesh_str(
            (nav != NULL && (nav->keyboard_field != MESH_UI_FIELD_NONE || nav->keyboard_waypoint ||
                             nav->keyboard_network || nav->keyboard_verify ||
                             nav->keyboard_channel_url || nav->keyboard_contact_url))
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
        /* The firmware reads out six, leading zeros included; a seventh is a mistype, and
           refusing it where it is typed beats sending a number the radio will not match. */
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

/*
 * The same, for a keycap that is several bytes: an emoji cell.
 *
 * All of it or none of it. The cap is a byte count and an emoji is four of them, so appending
 * as far as the cap would leave a truncated UTF-8 sequence in a field that is about to go to a
 * radio - the one outcome worse than the character not fitting.
 */
static void mesh_ui_nav_draft_append_text(struct mesh_ui_nav *nav, const char *text) {
    if (text == NULL || text[0] == '\0') {
        return;
    }
    const size_t len = strlen(nav->draft);
    const size_t add = strlen(text);
    if (len + add + 1U > sizeof nav->draft || len + add > mesh_ui_nav_draft_cap(nav)) {
        return;
    }
    memcpy(&nav->draft[len], text, add + 1U);
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
    nav->kb_emoji_page = 0U;
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
    if (nav->keyboard_contact_url) {
        nav->keyboard_contact_url = false;
        snprintf(nav->draft, sizeof nav->draft, "%s", nav->draft_saved);
        nav->draft_saved[0] = '\0';
        /* Back to the User list the add row is on, the way the channel link goes back to the
           Channels list. */
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
    nav->kb_emoji_page = 0U;
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
 * keyboard has letters on it and nothing but the six digits means anything to the radio.
 *
 * Short of six is refused rather than sent. Unlike a pairing PIN - where a wrong answer costs
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
    nav->kb_emoji_page = 0U;
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

void mesh_ui_nav_open_contact_url_keyboard(struct mesh_ui_nav *nav) {
    if (nav == NULL) {
        return;
    }
    /* The Compose draft into the one parking slot, and a blank line to type on: a contact link
       is somebody else's, read off their phone, and the last one typed is not a starting point
       for the next. */
    snprintf(nav->draft_saved, sizeof nav->draft_saved, "%s", nav->draft);
    nav->draft[0] = '\0';
    nav->keyboard_contact_url = true;
    nav->keyboard_channel_url = false;
    nav->keyboard_network = false;
    nav->keyboard_waypoint = false;
    nav->keyboard_field = MESH_UI_FIELD_NONE;
    nav->keyboard_open = true;
    nav->compose_open = false;
    nav->kb_row = 0U;
    nav->kb_col = 0U;
    nav->kb_layer = MESH_UI_KB_LOWER;
    nav->kb_emoji_page = 0U;
    nav->screen = MESH_UI_SCREEN_SETTINGS;
}

/* Done on the contact link keyboard: check it, then raise the sheet. The channel link's commit
   exactly, and it refuses for that one's reason - a link is base64 typed a character at a time
   on a d-pad, and the honest answer to one wrong character is to leave the user standing on the
   keyboard with what they typed. */
bool mesh_ui_nav_commit_contact_url(struct mesh_ui_nav *nav) {
    if (nav == NULL) {
        return false;
    }
    if (!mesh_ui_contact_link_valid(nav->draft)) {
        mesh_ui_nav_raise_toast(nav, mesh_str(MESH_STR_TOAST_CONTACT_LINK_INVALID));
        return true; /* stay on the keyboard so it can be fixed */
    }
    snprintf(nav->contact_url, sizeof nav->contact_url, "%s", nav->draft);
    mesh_ui_nav_keyboard_close(nav);
    /* Cancel by default, so a repeated press on Done cannot write a stranger's key to the
       radio. */
    nav->confirm_open = true;
    nav->confirm_cursor = 1U;
    nav->confirm_action = (uint8_t)MESH_UI_SETTINGS_ACTION_IMPORT_CONTACT;
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
    const bool for_contact = (!for_passkey && !for_verify && !for_setting && !for_waypoint &&
                              !for_network && !for_link && nav->keyboard_contact_url);
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
            char scratch[MESH_UI_KB_CELL_MAX];
            const char *const cell = mesh_ui_kb_cell(nav, nav->kb_row, nav->kb_col, scratch);
            if (cell[0] != '\0') {
                mesh_ui_nav_draft_append_text(nav, cell);
                /* One capital, then back to lower case, like a phone keyboard. The emoji layer
                   deliberately does not do the same: a run of them is the normal way to use it,
                   and a picker that closed itself after one would be a picker nobody uses
                   twice. */
                if (nav->kb_layer == MESH_UI_KB_UPPER) {
                    nav->kb_layer = MESH_UI_KB_LOWER;
                }
            }
            return true;
        }
        switch ((enum mesh_ui_kb_action)nav->kb_col) {
        case MESH_UI_KB_ACTION_LAYER:
            mesh_ui_nav_kb_panel_step(nav, 1);
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
            if (for_contact) {
                return mesh_ui_nav_commit_contact_url(nav);
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
        /*
         * Out of the keyboard, and that is all it does: B is back on every other screen in the
         * client and was a backspace on this one, which is the press people stumble over -
         * where every pad-driven keyboard they have used puts backspace on X and leaves B for
         * leaving.
         *
         * What was typed survives, because backing out is not the same press as throwing away.
         * A message keyboard hands its draft to the compose sheet's draft row; every other
         * flavour is closing over a Compose draft that was parked when it opened, and giving
         * that back is the same promise. The grid's own cancel key is what discards.
         */
        if (for_passkey) {
            return mesh_ui_nav_cancel_passkey(nav, action);
        }
        if (for_verify) {
            /* Leaving this one *is* standing the ceremony down: somebody at the other end is
               holding a number up, and a prompt closed quietly would leave them there. */
            return mesh_ui_nav_cancel_verify_number(nav, action);
        }
        mesh_ui_nav_keyboard_close(nav);
        return true;
    case MESH_UI_KEY_X:
        /* Backspace, where a pad-driven keyboard puts it. */
        return mesh_ui_nav_draft_delete(nav);
    case MESH_UI_KEY_Y:
        mesh_ui_nav_draft_append(nav, ' ');
        return true;
    case MESH_UI_KEY_L1:
    case MESH_UI_KEY_R1:
        /* The shoulders move between things everywhere else in the client, and a keyboard with
           six panels is a thing to move between. They replace a backspace and a space that
           duplicated B and Y, which is two buttons spent on presses the face already had. */
        mesh_ui_nav_kb_panel_step(nav, key == MESH_UI_KEY_L1 ? -1 : 1);
        return true;
    case MESH_UI_KEY_L2:
    case MESH_UI_KEY_R2:
        mesh_ui_nav_kb_shift(nav);
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
        if (for_contact) {
            return mesh_ui_nav_commit_contact_url(nav);
        }
        return for_waypoint ? mesh_ui_nav_commit_waypoint(nav, action)
                            : mesh_ui_nav_send_draft(nav, action);
    case MESH_UI_KEY_SELECT:
    case MESH_UI_KEY_NONE:
    default:
        return false;
    }
}
