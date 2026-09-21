#define _POSIX_C_SOURCE 200809L

/* See mesh/ui/contact_share.h. Reads a contact link and answers in catalog text. */

#include "mesh/ui/contact_share.h"

#include "mesh/i18n/strings.h"
#include "mesh/proto/contact_url.h"
#include "mesh/ui/store_settings.h"
#include "mesh/utils/text.h"

#include <stdio.h>
#include <string.h>

/*
 * The store's buffer has to hold the longest contact link the wire format can make.
 *
 * store_settings.h restates the bound rather than including this header, for the reason it
 * gives there; this is the assertion that keeps the restatement honest. A protobuf bump that
 * widens `User` - a longer name, a second key - fails the build here rather than silently
 * cutting a contact code in half.
 */
_Static_assert(MESH_UI_CONTACT_URL_MAX >= MESH_CONTACT_URL_MAX,
               "the store's contact_url is too small for the longest contact link");

/*
 * The node number as text, always spelled from `node_num` and never copied out of `user.id`.
 *
 * The two are the same node - mesh_contact_url_decode() refuses a link where they are not - and
 * this is still the field to read, because `node_num` is the one the radio files the entry
 * under. A sheet has one job here: name the node this press will write to. Showing the other
 * field would mean a screen and a write agreeing only as long as the decoder's check holds,
 * which is a coupling nothing gains from.
 */
static void contact_id(const meshtastic_SharedContact *contact, char *out, size_t out_len) {
    inkcell_str_format(out, out_len, MESH_STR_NODE_VAL_USER_ID_HEX, contact->node_num);
}

/* The name to put in front of a person: what they call themselves, what they call themselves
   in four characters, or - for a link carrying neither - the catalog's stand-in. A contact with
   no name at all is legal on the wire and is still worth adding: what makes it useful is the
   key, and the name arrives with the node's first NodeInfo. */
static void contact_name(const meshtastic_SharedContact *contact, char *out, size_t out_len) {
    if (contact->has_user && contact->user.long_name[0] != '\0') {
        inkcell_str_copy(out, out_len, contact->user.long_name);
        return;
    }
    if (contact->has_user && contact->user.short_name[0] != '\0') {
        inkcell_str_copy(out, out_len, contact->user.short_name);
        return;
    }
    inkcell_str_copy(out, out_len, inkcell_str(MESH_STR_CONTACT_NO_NAME));
}

bool mesh_ui_contact_share_summary(const char *url, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';
    meshtastic_SharedContact contact;
    if (url == NULL || url[0] == '\0' || !mesh_contact_url_decode(url, &contact)) {
        return false;
    }
    char name[sizeof contact.user.long_name];
    char id[sizeof contact.user.id];
    contact_name(&contact, name, sizeof name);
    contact_id(&contact, id, sizeof id);
    inkcell_str_format(out, out_len, MESH_STR_CONTACT_SUMMARY, name, id);
    return true;
}

bool mesh_ui_contact_link_valid(const char *text) {
    meshtastic_SharedContact contact;
    return text != NULL && mesh_contact_url_decode(text, &contact);
}

bool mesh_ui_contact_link_name(const char *text, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';
    meshtastic_SharedContact contact;
    if (text == NULL || !mesh_contact_url_decode(text, &contact)) {
        return false;
    }
    contact_name(&contact, out, out_len);
    return true;
}

bool mesh_ui_contact_import_sheet(const char *text, char *headline, size_t headline_len, char *body,
                                  size_t body_len) {
    if (headline != NULL && headline_len > 0U) {
        headline[0] = '\0';
    }
    if (body != NULL && body_len > 0U) {
        body[0] = '\0';
    }
    meshtastic_SharedContact contact;
    if (text == NULL || !mesh_contact_url_decode(text, &contact)) {
        return false;
    }
    if (headline != NULL) {
        char name[sizeof contact.user.long_name];
        contact_name(&contact, name, sizeof name);
        inkcell_str_format(headline, headline_len, MESH_STR_CONFIRM_TITLE_ADD_CONTACT, name);
    }
    if (body != NULL) {
        /* The paragraph names the *number* where the headline named the name. A link is a
           stranger's claim about both, and the number is the half that decides which NodeDB
           entry this write lands on - so it is the half worth reading before saying yes. */
        char id[sizeof contact.user.id];
        contact_id(&contact, id, sizeof id);
        inkcell_str_format(body, body_len, MESH_STR_CONFIRM_TEXT_ADD_CONTACT, id);
    }
    return true;
}
