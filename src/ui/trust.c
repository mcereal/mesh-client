#include "mesh/ui/trust.h"

#include "mesh/utils/text.h"

#include <stddef.h>

/*
 * The key-trust vocabulary, and the verification sheet's words. See the header for why both
 * live in one table: the three screens that draw trust have to agree, and the sheet is the
 * fourth screen in the same story.
 */

/*
 * The mark per state.
 *
 * A padlock and a shield, which are two shapes rather than one shape in two colours - a colour
 * would be a claim the theme's contrast contract has to carry, and at 28 px on a bubble's
 * trailing run the difference between two hues of the same glyph is not a difference.
 *
 * Neither is a dedicated glyph. MESH_UI_ICON_SECURITY is the Settings tab's Security section,
 * and it is doing a second job here, which include/mesh/ui/icons.def's rules say should be a
 * second id. It is not one because the sprite table is generated from a font that has moved
 * upstream since it was last rasterised, so adding a row would rewrite all 54 sprites in the
 * same commit as this feature - see the note in the pull request. The id to add when that is
 * done is VERIFIED, and this table is the only place that would change.
 */
static const enum mesh_ui_icon k_trust_icons[] = {
    [MESH_UI_KEY_TRUST_NONE] = MESH_UI_ICON_NONE,
    [MESH_UI_KEY_TRUST_UNVERIFIED] = MESH_UI_ICON_ENCRYPTED,
    [MESH_UI_KEY_TRUST_VERIFIED] = MESH_UI_ICON_SECURITY,
};

static const enum mesh_str_id k_trust_labels[] = {
    [MESH_UI_KEY_TRUST_NONE] = MESH_STR_TRUST_NONE,
    [MESH_UI_KEY_TRUST_UNVERIFIED] = MESH_STR_TRUST_UNVERIFIED,
    [MESH_UI_KEY_TRUST_VERIFIED] = MESH_STR_TRUST_VERIFIED,
};

static const enum mesh_ui_tone k_trust_tones[] = {
    [MESH_UI_KEY_TRUST_NONE] = MESH_UI_TONE_NORMAL,
    [MESH_UI_KEY_TRUST_UNVERIFIED] = MESH_UI_TONE_NORMAL,
    [MESH_UI_KEY_TRUST_VERIFIED] = MESH_UI_TONE_SUCCESS,
};

static bool trust_in_range(enum mesh_ui_key_trust trust) {
    return (size_t)trust < sizeof k_trust_labels / sizeof k_trust_labels[0];
}

enum mesh_ui_key_trust mesh_ui_key_trust_of(const struct mesh_ui_node_summary *node) {
    if (node == NULL || node->public_key_len == 0U) {
        return MESH_UI_KEY_TRUST_NONE;
    }
    /* The verified bit only ever means anything alongside a key, which is why it is read here
       and not on its own: a record carrying the bit and no key is a roster that has lost half
       of something, and the honest answer for it is that there is nothing to encrypt to. */
    return node->key_verified ? MESH_UI_KEY_TRUST_VERIFIED : MESH_UI_KEY_TRUST_UNVERIFIED;
}

enum mesh_ui_icon mesh_ui_key_trust_icon(enum mesh_ui_key_trust trust) {
    return trust_in_range(trust) ? k_trust_icons[trust] : MESH_UI_ICON_NONE;
}

enum mesh_str_id mesh_ui_key_trust_label(enum mesh_ui_key_trust trust) {
    return trust_in_range(trust) ? k_trust_labels[trust] : MESH_STR_NONE;
}

enum mesh_ui_tone mesh_ui_key_trust_tone(enum mesh_ui_key_trust trust) {
    return trust_in_range(trust) ? k_trust_tones[trust] : MESH_UI_TONE_NORMAL;
}

/* The other person, as the sheet names them: the name the *radio* used, falling back to the
   node number when the notification carried none. Never a name resolved out of the roster -
   see struct mesh_ui_verification. */
static void verify_peer(const struct mesh_ui_verification *verification, char *out,
                        size_t out_len) {
    if (verification->remote_name[0] != '\0') {
        (void)mesh_str_copy(out, out_len, verification->remote_name);
        return;
    }
    mesh_str_format(out, out_len, MESH_STR_NODE_VAL_USER_ID_HEX, verification->remote_node);
}

bool mesh_ui_verify_sheet_of(const struct mesh_ui_verification *verification,
                             struct mesh_ui_verify_sheet *out, char *headline, size_t headline_len,
                             char *text, size_t text_len) {
    if (verification == NULL || out == NULL || headline == NULL || headline_len == 0U ||
        text == NULL || text_len == 0U) {
        return false;
    }
    headline[0] = '\0';
    text[0] = '\0';

    char peer[MESH_UI_VERIFY_NAME_MAX];
    verify_peer(verification, peer, sizeof peer);

    switch ((enum mesh_ui_verify_stage)verification->stage) {
    case MESH_UI_VERIFY_WAITING:
        out->icon = MESH_UI_ICON_SENDING;
        mesh_str_format(headline, headline_len, MESH_STR_VERIFY_HEAD_WAITING, peer);
        (void)mesh_str_copy(text, text_len, mesh_str(MESH_STR_VERIFY_BODY_WAITING));
        /* "Later" gets out of the way without ending anything: the sheet comes back when the
           radio asks for something, and a user who has to go and find the other person should
           not have to keep a dialog open while they do it. */
        out->accept = MESH_STR_VERIFY_ANSWER_LATER;
        out->cancel = MESH_STR_VERIFY_ANSWER_STOP;
        return true;
    case MESH_UI_VERIFY_SHOW_NUMBER:
        out->icon = MESH_UI_ICON_SECURITY;
        /* The digits are the headline, drawn at the dialog's largest: they are the whole of
           what this screen is for, and the paragraph under them is the instructions. */
        mesh_str_format(headline, headline_len, MESH_STR_VERIFY_VAL_NUMBER,
                        (unsigned)verification->security_number);
        mesh_str_format(text, text_len, MESH_STR_VERIFY_BODY_NUMBER, peer);
        out->accept = MESH_STR_VERIFY_ANSWER_READING;
        out->cancel = MESH_STR_VERIFY_ANSWER_STOP;
        return true;
    case MESH_UI_VERIFY_COMPARE:
        out->icon = MESH_UI_ICON_SECURITY;
        /* The code as the radio sent it, as the headline, for the reason the digits above are
           one - and unformatted, because it is not a sentence: it is the thing being compared,
           and a specifier around it would be a translator's chance to put something between
           two people reading characters to each other. */
        (void)mesh_str_copy(headline, headline_len, verification->characters);
        mesh_str_format(text, text_len, MESH_STR_VERIFY_BODY_COMPARE, peer);
        out->accept = MESH_STR_VERIFY_ANSWER_MATCH;
        out->cancel = MESH_STR_VERIFY_ANSWER_DIFFER;
        return true;
    case MESH_UI_VERIFY_ENTER_NUMBER:
        /*
         * Deliberately not a sheet. This is the one stage that collects something, and the
         * client already has a way to be handed a number by a radio in the middle of whatever
         * the user was doing: the keyboard, retargeted, the way the BLE pairing PIN prompt
         * retargets it. A dialog here would be a dialog whose only answer opens a keyboard.
         */
        return false;
    case MESH_UI_VERIFY_IDLE:
    default:
        return false;
    }
}
