#define _POSIX_C_SOURCE 200809L

/* See mesh/core/contact_share.h. The half of contact sharing that knows what a radio is. */

#include "mesh/core/contact_share.h"

#include "mesh/proto/contact_url.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * Our node number, read back out of the owner record's own `id`.
 *
 * Taken from there rather than passed in from the handshake's `my_info`, and the reason is that
 * the two fields of a contact have to *agree*: `SharedContact.node_num` and `user.id` name the
 * same node, and a reader that cross-checks them - or a firmware that indexes its NodeDB by one
 * and displays the other - must not be handed a pair assembled from two sources that were read
 * a second apart. The owner reply carries both halves, so both halves come from it.
 *
 * The format is upstream's `!` followed by eight lower-case hex digits, which is what every
 * client writes and what this one writes in a dozen places of its own. Anything else is
 * refused rather than guessed at: a partial parse here would put a contact on the mesh under a
 * node number that is not the sender's.
 */
static bool owner_node_num(const meshtastic_User *owner, uint32_t *out) {
    if (owner->id[0] != '!') {
        return false;
    }
    const char *hex = &owner->id[1];
    for (size_t i = 0; i < 8U; ++i) {
        const char c = hex[i];
        const bool digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!digit) {
            return false;
        }
    }
    if (hex[8] != '\0') {
        return false;
    }
    *out = (uint32_t)strtoul(hex, NULL, 16);
    return *out != 0U;
}

/*
 * The public key to put in our own contact.
 *
 * Two places hold it and they are meant to be the same bytes: the owner record the radio
 * broadcasts as NodeInfo, and `SecurityConfig.public_key`, which is where the key pair actually
 * lives. The owner's is preferred because it is the one the mesh sees - a contact should carry
 * what a reader would have got by hearing this node speak - and SecurityConfig is the fallback
 * because firmware old enough to leave the owner's copy empty still fills that one.
 *
 * Returns 0 when neither is set, which is a radio with PKC off. There is nothing to share then,
 * and that is the honest answer rather than a code carrying a name and no key.
 */
static size_t own_public_key(const struct mesh_radio_settings *settings, const uint8_t **out) {
    if (settings->has_owner && settings->owner.public_key.size > 0U) {
        *out = settings->owner.public_key.bytes;
        return settings->owner.public_key.size;
    }
    if (settings->has_security && settings->security.public_key.size > 0U) {
        *out = settings->security.public_key.bytes;
        return settings->security.public_key.size;
    }
    return 0U;
}

bool mesh_contact_share_settled(const struct mesh_radio_settings *settings) {
    if (settings == NULL || !settings->has_owner) {
        return false;
    }
    uint32_t node_num = 0U;
    const uint8_t *key = NULL;
    return owner_node_num(&settings->owner, &node_num) && own_public_key(settings, &key) > 0U;
}

bool mesh_contact_share_build(const struct mesh_radio_settings *settings,
                              meshtastic_SharedContact *out) {
    if (settings == NULL || out == NULL || !settings->has_owner) {
        return false;
    }
    uint32_t node_num = 0U;
    if (!owner_node_num(&settings->owner, &node_num)) {
        return false;
    }
    const uint8_t *key = NULL;
    const size_t key_len = own_public_key(settings, &key);
    /*
     * Both checks before anything is written, so a false return leaves the caller's contact
     * untouched - which is what every caller assumes and what keeps a refused build from
     * looking like a half-filled one.
     *
     * The upper bound is unreachable with today's protobuf, where `User.public_key` and
     * `SecurityConfig.public_key` are both 32 bytes. It is here for the day one of them grows:
     * the two are copied between, and a silent truncation would be a contact carrying most of
     * a key, which decodes and encrypts to nobody.
     */
    if (key_len == 0U || key_len > sizeof out->user.public_key.bytes) {
        return false;
    }

    *out = (meshtastic_SharedContact)meshtastic_SharedContact_init_zero;
    out->node_num = node_num;
    out->has_user = true;
    /* The owner record verbatim: it is exactly what this radio broadcasts as NodeInfo, so a
       contact built from it says the same thing about us that hearing us would. */
    out->user = settings->owner;
    out->user.public_key.size = (pb_size_t)key_len;
    memcpy(out->user.public_key.bytes, key, key_len);
    /* The two instructions to the reader's radio, left off. See the header for why neither is
       ours to send. */
    out->should_ignore = false;
    out->manually_verified = false;
    return true;
}

size_t mesh_contact_share_url(const struct mesh_radio_settings *settings, char *out,
                              size_t out_len) {
    meshtastic_SharedContact contact;
    if (!mesh_contact_share_build(settings, &contact)) {
        if (out != NULL && out_len > 0U) {
            out[0] = '\0';
        }
        return 0U;
    }
    return mesh_contact_url_encode(&contact, out, out_len);
}

int mesh_contact_share_queue_import(struct mesh_radio_settings *settings,
                                    const meshtastic_SharedContact *contact) {
    if (settings == NULL || contact == NULL) {
        return -EINVAL;
    }
    /*
     * A copy, so the two fields below can be cleared without editing the caller's contact - the
     * caller is holding what the link said, and what the link said is worth keeping intact for
     * anything that wants to describe it.
     */
    meshtastic_SharedContact sanitised = *contact;
    sanitised.should_ignore = false;
    sanitised.manually_verified = false;
    return mesh_radio_settings_queue_contact(settings, &sanitised);
}
