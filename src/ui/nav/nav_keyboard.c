#define _POSIX_C_SOURCE 200809L

/*
 * What this client's on-screen keyboard is for.
 *
 * The grid itself is inkcell's - where the cursor is, which panel is showing, what a press does
 * to a buffer - and none of that was ever about Meshtastic. What is here is the half that is:
 * the emoji this client puts on the air, the job each keyboard was opened for, and what the
 * text is worth when it is finished.
 *
 * It is opened for six unrelated jobs - typing a message, typing a settings field, naming a new
 * waypoint, typing the network radio's address, and pasting a channel or contact link - plus
 * the BlueZ passkey prompt and the key-verification prompt, either of which can arrive on top
 * of any of them. mesh_ui_nav_keyboard_close() is where "give the user back what they were
 * doing" lives, and is the reason that function is longer than it looks like it should be.
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
 * The emoji layer, three pages of forty, in the row-major order inkcell_keyboard_cell() indexes.
 *
 * inkcell carries none of these deliberately: a set chosen for a radio on a hillside - tents,
 * ambulances, SOS - is the wrong set for a music player, so the pages are the application's and
 * this is where this client's are. Meshtastic names are written by people and a good share of a
 * real roster is emoji already - `User.short_name` is `char[5]`, which is exactly one - so the
 * client could draw them long before it could type one. These are what it can type.
 *
 * Written as `\U` escapes rather than pasted in, for the reason src/ui/tables/reactions.c states
 * about its own eight: an editor, a terminal or a patch tool that mangles non-ASCII cannot
 * quietly change what this client puts on the air. At a hundred and twenty cells the escape also
 * has to be *readable*, which the raw UTF-8 bytes are not - a code point names itself, and
 * kb_emoji_cells_are_drawable is what checks that each one is a glyph this build actually has a
 * sprite for rather than a box.
 *
 * Every cell is a single code point with no variation selector and no joiner, which is the same
 * rule the tapbacks follow: a glyph spelled two ways is two different glyphs to anything
 * counting them, and the one on the air should be the one the other client recognises.
 *
 * clang-format off over the table, because this is a *grid* ten cells wide and which cell sits
 * under which is the only thing about it worth reading. Reflowed to a string a line it is a
 * hundred and twenty lines of escapes nothing can be found in.
 */
#define MESH_UI_KB_EMOJI_PAGES 3U

/* clang-format off */
static const char *const k_kb_emoji[MESH_UI_KB_EMOJI_PAGES * INKCELL_KB_EMOJI_PAGE_CELLS] = {
    /* Faces, hands, and what lives out there. */
    /* grinning through smiling */
    "\U0001F600", "\U0001F603", "\U0001F604", "\U0001F601", "\U0001F606",
    "\U0001F605", "\U0001F602", "\U0001F642", "\U0001F609", "\U0001F60A",
    /* fond, thinking, tired, upset */
    "\U0001F60D", "\U0001F618", "\U0001F61C", "\U0001F914", "\U0001F610",
    "\U0001F634", "\U0001F60E", "\U0001F62D", "\U0001F622", "\U0001F631",
    /* cross, celebrating, unwell, then the hands */
    "\U0001F621", "\U0001F633", "\U0001F973", "\U0001F912", "\U0001F91D",
    "\U0001F44D", "\U0001F44E", "\U0001F44B", "\U0001F44C", "\U0000270C",
    /* please, strength, watching, people and what lives out there */
    "\U0001F64F", "\U0001F4AA", "\U0001F440", "\U0001F9E0", "\U0001F464",
    "\U0001F46A", "\U0001F415", "\U0001F43B", "\U0001F98C", "\U0001F40D",
    /* Marks, symbols and status. */
    /* affection and occasions */
    "\U00002764", "\U0001F494", "\U0001F4AF", "\U00002728", "\U00002B50",
    "\U0001F525", "\U0001F389", "\U0001F382", "\U0001F381", "\U0000262E",
    /* yes, no, careful, asking, and time */
    "\U00002705", "\U0000274C", "\U000026A0", "\U00002753", "\U00002757",
    "\U0000203C", "\U00002795", "\U00002796", "\U000023F0", "\U0000231B",
    /* attention, places, and writing */
    "\U0001F514", "\U0001F515", "\U0001F4CC", "\U0001F4CD", "\U0001F4CE",
    "\U0001F4DD", "\U0001F4D6", "\U0001F4AC", "\U0001F4E3", "\U0001F517",
    /* locks, eyes, power, and the two arrows */
    "\U0001F512", "\U0001F513", "\U0001F511", "\U0001F441", "\U0001F4A4",
    "\U0000267B", "\U000026A1", "\U00002622", "\U00002B06", "\U00002B07",
    /* Outdoors, weather, travel and kit. */
    /* the sky */
    "\U00002600", "\U000026C5", "\U00002601", "\U000026C8", "\U00002744",
    "\U0001F30A", "\U0001F308", "\U0001F319", "\U0001F31E", "\U0001F30D",
    /* the ground, and finding your way over it */
    "\U000026F0", "\U0001F332", "\U0001F335", "\U0001F3DD", "\U0001F3D5",
    "\U0001F5FA", "\U0001F9ED", "\U0001F6A9", "\U0001F6F6", "\U0001F3A3",
    /* getting there, and who comes when it goes wrong */
    "\U0001F697", "\U0001F68C", "\U0001F6B2", "\U0001F6FB", "\U0001F681",
    "\U0001F691", "\U0001F692", "\U0001F46E", "\U0001F3E0", "\U0001F3E5",
    /* the kit, the call for help, and patching it up */
    "\U0001F4E1", "\U0001F50B", "\U0001F526", "\U0001F6E0", "\U0001F4FB",
    "\U0001F198", "\U0001F6A8", "\U0001F50C", "\U0001F4DE", "\U0001FA79",
};
/* clang-format on */

bool mesh_ui_nav_kb_submit_finishes(const struct mesh_ui_nav *nav) {
    if (nav == NULL) {
        return false;
    }
    return nav->keyboard_field != MESH_UI_FIELD_NONE || nav->keyboard_waypoint ||
           nav->keyboard_network || nav->keyboard_verify || nav->keyboard_channel_url ||
           nav->keyboard_contact_url;
}

struct inkcell_keyboard_layout mesh_ui_nav_kb_layout(const struct mesh_ui_nav *nav) {
    return (struct inkcell_keyboard_layout){
        .emoji = k_kb_emoji,
        .pages = (uint8_t)MESH_UI_KB_EMOJI_PAGES,
        .submit_label = mesh_ui_nav_kb_submit_finishes(nav)
                            ? (enum inkcell_str_id)MESH_STR_KEY_DONE
                            : (enum inkcell_str_id)MESH_STR_KEY_SEND,
        /* The cap the append is held to. The nav owns it because it is a fact about the job -
           six digits for a passkey, a field's own limit for a setting - and the counter the
           renderer draws asks the same function, so the promise and the typing cannot drift. */
        .cap = mesh_ui_nav_draft_cap(nav),
    };
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

/* Closing always parks the cursor at the top-left so the next message starts the same way.
   A keyboard opened for a setting returns to that section and restores the Compose draft; one
   opened for a message falls back to the compose overlay it was opened from. */
void mesh_ui_nav_keyboard_close(struct mesh_ui_nav *nav) {
    nav->keyboard_open = false;
    inkcell_keyboard_reset(&nav->kb);
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
    inkcell_keyboard_reset(&nav->kb);
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
        /* inkcell_str_copy rather than snprintf: the draft is the message buffer and the
           identifier is a target, so the compiler is right that one does not fit in the other
           - it is mesh_ui_nav_draft_cap() that keeps the two in step, and a bounded copy is
           what says so at the call rather than in a comment. */
        inkcell_str_copy(action->identifier, sizeof action->identifier, nav->draft);
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
    inkcell_keyboard_reset(&nav->kb);
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
        mesh_ui_nav_raise_toast(nav, inkcell_str(MESH_STR_TOAST_IMPORT_NOT_A_LINK));
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
    inkcell_keyboard_reset(&nav->kb);
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
        mesh_ui_nav_raise_toast(nav, inkcell_str(MESH_STR_TOAST_CONTACT_LINK_INVALID));
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

/*
 * The text is finished: whichever of the eight jobs this keyboard was opened for decides what
 * that means.
 *
 * One function for the submit key and for START, because they are one press to the user and
 * were two copies of this ladder - which is the shape a bug takes here: a flavour added to one
 * copy and not the other is a keyboard whose Start key does something its Send key does not.
 */
static bool mesh_ui_nav_keyboard_submit(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                                        struct mesh_ui_action *action) {
    if (nav->keyboard_passkey) {
        return mesh_ui_nav_submit_passkey(nav, action);
    }
    if (nav->keyboard_verify) {
        return mesh_ui_nav_submit_verify_number(nav, action);
    }
    if (nav->keyboard_field != MESH_UI_FIELD_NONE) {
        return mesh_ui_nav_settings_commit_text(nav, store);
    }
    if (nav->keyboard_network) {
        return mesh_ui_nav_commit_network_host(nav, action);
    }
    if (nav->keyboard_channel_url) {
        return mesh_ui_nav_commit_channel_url(nav);
    }
    if (nav->keyboard_contact_url) {
        return mesh_ui_nav_commit_contact_url(nav);
    }
    return nav->keyboard_waypoint ? mesh_ui_nav_commit_waypoint(nav, action)
                                  : mesh_ui_nav_send_draft(nav, action);
}

/*
 * The grid's own cancel key: the text is thrown away.
 *
 * Not the same press as B - see INKCELL_KEYBOARD_DISMISS below - and the two prompts the radio
 * raises are the reason the difference is worth keeping. Abandoning a bond and standing a
 * verification down are things somebody at the other end is waiting on, and they are what
 * "throw this away" means on those two screens.
 */
static bool mesh_ui_nav_keyboard_cancel(struct mesh_ui_nav *nav, struct mesh_ui_action *action) {
    if (nav->keyboard_passkey) {
        return mesh_ui_nav_cancel_passkey(nav, action);
    }
    if (nav->keyboard_verify) {
        return mesh_ui_nav_cancel_verify_number(nav, action);
    }
    nav->draft[0] = '\0';
    mesh_ui_nav_keyboard_close(nav);
    return true;
}

bool mesh_ui_nav_keyboard_key(struct mesh_ui_nav *nav, const struct mesh_ui_store *store,
                              enum inkcell_key key, struct mesh_ui_action *action) {
    if (nav == NULL) {
        return false;
    }
    /*
     * The grid first, this client's meaning after. inkcell moves the cursor, walks the panels
     * and edits the draft; what none of that can decide is what the text was for, which is
     * every line below.
     */
    const struct inkcell_keyboard_layout layout = mesh_ui_nav_kb_layout(nav);
    switch (inkcell_keyboard_key(&nav->kb, &layout, key, nav->draft, sizeof nav->draft)) {
    case INKCELL_KEYBOARD_CONSUMED:
        return true;
    case INKCELL_KEYBOARD_SUBMIT:
        return mesh_ui_nav_keyboard_submit(nav, store, action);
    case INKCELL_KEYBOARD_CANCEL:
        return mesh_ui_nav_keyboard_cancel(nav, action);
    case INKCELL_KEYBOARD_DISMISS:
        /*
         * B: out of the keyboard with the draft intact. A message keyboard hands it to the
         * compose sheet's draft row; every other flavour is closing over a Compose draft that
         * was parked when it opened, and giving that back is the same promise.
         *
         * Except on the two prompts the radio raised, where backing out *is* the answer:
         * nobody is on the other end of a settings field, and somebody is on the other end of
         * both of these. A PIN prompt left quietly open holds the bond until BlueZ times it
         * out; a verification left open leaves the other person holding a number up.
         */
        if (nav->keyboard_passkey) {
            return mesh_ui_nav_cancel_passkey(nav, action);
        }
        if (nav->keyboard_verify) {
            return mesh_ui_nav_cancel_verify_number(nav, action);
        }
        mesh_ui_nav_keyboard_close(nav);
        return true;
    case INKCELL_KEYBOARD_IGNORED:
    default:
        return false;
    }
}
