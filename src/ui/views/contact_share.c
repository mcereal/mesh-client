#define _POSIX_C_SOURCE 200809L

/* See mesh/ui/contact_share.h. Reads a contact link and answers in catalog text. */

#include "mesh/ui/contact_share.h"

#include "inkwell/base/text.h"

#include "mesh/i18n/strings.h"
#include "mesh/proto/contact_url.h"
#include "mesh/proto/meshcore_url.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/store_settings.h"

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
_Static_assert(MESH_UI_CONTACT_URL_MAX >= MESH_MESHCORE_CONTACT_URL_MAX,
               "the store's contact_url is too small for the longest MeshCore contact link");

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
        inkwell_str_copy(out, out_len, contact->user.long_name);
        return;
    }
    if (contact->has_user && contact->user.short_name[0] != '\0') {
        inkwell_str_copy(out, out_len, contact->user.short_name);
        return;
    }
    inkwell_str_copy(out, out_len, inkcell_str(MESH_STR_CONTACT_NO_NAME));
}

/*
 * What a link names, in either protocol's spelling: the Meshtastic app's `meshtastic.org/v/#`
 * or the MeshCore app's `meshcore://contact/add` - or its signed card, `meshcore://<hex>`. The two
 * cannot be mistaken for each other - one is a fragment of base64, the other a query string - so
 * reading is trying each.
 */
/* An id as either app prints one: Meshtastic's `!%08x`, or MeshCore's key head in 12 hex. */
#define CONTACT_ID_MAX 16U
_Static_assert(CONTACT_ID_MAX >= sizeof(((meshtastic_User *)0)->id) &&
                   CONTACT_ID_MAX >= sizeof("0123456789ab"),
               "a contact id holds either app's spelling of one");

/* The keyboard, the parked link and the action that carries it all hold a draft: the longest
   card has to fit, or it is cut before it is read and the radio is handed half an advert. */
_Static_assert(MESH_UI_DRAFT_MAX >=
                   sizeof(MESH_MESHCORE_URL_CARD_PREFIX) + 2U * MESH_MESHCORE_CARD_MAX,
               "a draft is too short for the longest MeshCore contact card");

struct contact_reading {
    bool meshcore;
    char name[sizeof(((meshtastic_User *)0)->long_name)];
    char id[CONTACT_ID_MAX];
};

static void meshcore_reading(const char *name, const uint8_t *key, struct contact_reading *out) {
    out->meshcore = true;
    inkwell_str_copy(out->name, sizeof out->name,
                     name[0] != '\0' ? name : inkcell_str(MESH_STR_CONTACT_NO_NAME));
    /* The key's first six bytes, which is how MeshCore's own apps print a node. */
    snprintf(out->id, sizeof out->id, "%02x%02x%02x%02x%02x%02x", key[0], key[1], key[2], key[3],
             key[4], key[5]);
}

static bool read_link(const char *text, struct contact_reading *out) {
    memset(out, 0, sizeof *out);
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    meshtastic_SharedContact contact;
    if (mesh_contact_url_decode(text, &contact)) {
        contact_name(&contact, out->name, sizeof out->name);
        contact_id(&contact, out->id, sizeof out->id);
        return true;
    }
    struct mesh_meshcore_contact_link link;
    if (mesh_meshcore_contact_url_decode(text, &link)) {
        meshcore_reading(link.name, link.public_key, out);
        return true;
    }
    /* The signed card the MeshCore app shares: the node's advert, read only for what to show. */
    static struct mesh_meshcore_card card;
    if (mesh_meshcore_card_decode(text, &card)) {
        meshcore_reading(card.name, card.public_key, out);
        return true;
    }
    return false;
}

bool mesh_ui_contact_share_summary(const char *url, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';
    struct contact_reading reading;
    if (!read_link(url, &reading)) {
        return false;
    }
    inkcell_str_format(out, out_len,
                       reading.meshcore ? MESH_STR_CONTACT_SUMMARY_MESHCORE
                                        : MESH_STR_CONTACT_SUMMARY,
                       reading.name, reading.id);
    return true;
}

bool mesh_ui_contact_link_valid(const char *text) {
    struct contact_reading reading;
    return read_link(text, &reading);
}

bool mesh_ui_contact_link_name(const char *text, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';
    struct contact_reading reading;
    if (!read_link(text, &reading)) {
        return false;
    }
    inkwell_str_copy(out, out_len, reading.name);
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
    struct contact_reading reading;
    if (!read_link(text, &reading)) {
        return false;
    }
    if (headline != NULL) {
        inkcell_str_format(headline, headline_len, MESH_STR_CONFIRM_TITLE_ADD_CONTACT,
                           reading.name);
    }
    if (body != NULL) {
        /* The paragraph names the *number* where the headline named the name. A link is a
           stranger's claim about both, and the number is the half that decides which entry
           this write lands on - so it is the half worth reading before saying yes. */
        inkcell_str_format(body, body_len,
                           reading.meshcore ? MESH_STR_CONFIRM_TEXT_ADD_CONTACT_MESHCORE
                                            : MESH_STR_CONFIRM_TEXT_ADD_CONTACT,
                           reading.id);
    }
    return true;
}
