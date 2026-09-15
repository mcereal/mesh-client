#define _POSIX_C_SOURCE 200809L

/* See mesh/proto/contact_url.h. Bytes in, bytes out; nothing here knows what a radio is. */

#include "mesh/proto/contact_url.h"

#include "mesh/utils/base64.h"

#include "link_url.h"

#include <pb_decode.h>
#include <pb_encode.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/*
 * The bar both directions hold a contact to: a node number and a public key.
 *
 * Neither is a protobuf requirement - every field of `SharedContact` is optional on the wire,
 * and a payload of zero bytes decodes cleanly into an empty one - and that is exactly why it is
 * asked here. A contact with no node number names nobody, and one with no key is what the radio
 * would build for itself the moment the node transmitted, so writing either into a link or
 * reading either out of one is promising a reader something that is not there.
 *
 * It is the same bar mesh_radio_settings_queue_contact() holds the admin verb to, and it is
 * held here as well rather than only there so that a mistyped link is refused as a link -
 * where the user is still standing, with the text still on the keyboard - instead of parsing
 * cleanly and failing three layers down as a queue error with nothing to say.
 */
static bool contact_is_usable(const meshtastic_SharedContact *contact) {
    if (contact->node_num == 0U || !contact->has_user || contact->user.public_key.size == 0U) {
        return false;
    }
    /*
     * And the two halves have to name the *same node*.
     *
     * A `SharedContact` says who it is twice - `node_num`, which is what a radio files the entry
     * under, and `user.id`, which is what a screen shows - and nothing on the wire makes them
     * agree. A link whose id says one node while its number says another is a contact that reads
     * as one person and is written as another: approve adding `!aaaaaaaa` and a stranger's key
     * lands in `!bbbbbbbb`'s slot. Refusing here is what makes the two interchangeable for every
     * caller, so no screen has to know which of them the write will use.
     *
     * Compared against the canonical spelling rather than parsed, because that spelling is
     * upstream's own: the firmware fills the field with `snprintf("!%08x", node_num)` and every
     * client copies it from there. Case-insensitively, so a client that wrote its hex in capitals
     * is read rather than refused. An empty id claims nothing and is allowed - the node number
     * is the field that matters, and a link is welcome to leave the other out.
     */
    if (contact->user.id[0] == '\0') {
        return true;
    }
    char canonical[sizeof contact->user.id];
    (void)snprintf(canonical, sizeof canonical, "!%08x", (unsigned)contact->node_num);
    return strcasecmp(contact->user.id, canonical) == 0;
}

size_t mesh_contact_url_encode(const meshtastic_SharedContact *contact, char *out, size_t out_len) {
    if (contact == NULL || out == NULL || out_len == 0U) {
        return 0U;
    }
    out[0] = '\0';
    if (!contact_is_usable(contact)) {
        return 0U;
    }

    uint8_t encoded[meshtastic_SharedContact_size];
    pb_ostream_t stream = pb_ostream_from_buffer(encoded, sizeof encoded);
    if (!pb_encode(&stream, meshtastic_SharedContact_fields, contact)) {
        return 0U;
    }

    const size_t prefix_len = strlen(MESH_CONTACT_URL_PREFIX);
    if (prefix_len >= out_len) {
        return 0U;
    }
    memcpy(out, MESH_CONTACT_URL_PREFIX, prefix_len);
    const size_t payload = mesh_base64_encode(encoded, stream.bytes_written, true, out + prefix_len,
                                              out_len - prefix_len);
    if (payload == 0U) {
        out[0] = '\0';
        return 0U;
    }
    return prefix_len + payload;
}

bool mesh_contact_url_decode(const char *text, meshtastic_SharedContact *out) {
    if (text == NULL || out == NULL) {
        return false;
    }
    const char *payload = mesh_link_url_payload(text);
    if (payload == NULL || payload[0] == '\0') {
        return false;
    }

    /*
     * A query tail comes off before the base64, as it does for a channel link - but nothing
     * here reads one. `?add=true` is a claim about what the *reader's* channel table should do
     * with a set, and a contact has no second mode to be in: adding it is the only thing there
     * is to do with one. Cutting the tail anyway is what keeps a link that arrived with some
     * other site's tracking parameter on it from being refused over characters that were never
     * part of the payload.
     */
    const size_t chars = strcspn(payload, "?&");

    uint8_t decoded[meshtastic_SharedContact_size];
    size_t len = 0U;
    if (!mesh_base64_decode(payload, chars, MESH_BASE64_ANY, decoded, sizeof decoded, &len)) {
        return false;
    }

    *out = (meshtastic_SharedContact)meshtastic_SharedContact_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(decoded, len);
    if (!pb_decode(&stream, meshtastic_SharedContact_fields, out)) {
        return false;
    }
    return contact_is_usable(out);
}
