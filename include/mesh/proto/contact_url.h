#ifndef MESH_PROTO_CONTACT_URL_H
#define MESH_PROTO_CONTACT_URL_H

/*
 * The Meshtastic contact URL: one node's identity as a link.
 *
 * `https://meshtastic.org/v/#<payload>`, where the payload is a `SharedContact` protobuf in
 * URL-safe base64 with the padding dropped - the same wrapper as the channel link one directory
 * over, around a different message. It is how the phone apps hand a node to each other, and it
 * answers the one question this client could not: *how do you get a contact for a node that has
 * never transmitted?* Until a node is heard from there is no NodeInfo and so no public key, and
 * without a key a direct message to it goes out in the clear or not at all. A contact link
 * carries the key ahead of the node.
 *
 * It is `src/proto/` rather than `src/core/` for mesh/proto/channel_url.h's reason and with the
 * same boundary: bytes in, bytes out, no radio, no queue, no store, and no opinion about
 * whether the contact in it should be trusted. What a radio does with one is
 * mesh/core/contact_share.h's question - and the answer to that includes two fields this module
 * carries faithfully and that one deliberately drops.
 *
 * `SharedContact` is `admin.proto`: unlike `ChannelSet` it is a message that also goes over the
 * wire, as the payload of the `add_contact` admin verb this client has queued since key trust
 * landed. The link is a second way to obtain one, not a second thing to do with it.
 */

#include "meshtastic/admin.pb.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The link this client writes, and the one it prefers to read.
 *
 * `/v/` rather than the channel link's `/e/`, which is the apps' own split: a different path
 * for a different message, because the payload of one will not decode as the other and a reader
 * that guessed would hand a channel set to the NodeDB.
 */
#define MESH_CONTACT_URL_PREFIX "https://meshtastic.org/v/#"

/*
 * The longest link a SharedContact can make, NUL included.
 *
 * From the protobuf's own bound the way MESH_CHANNEL_URL_MAX is: nanopb works out how large a
 * SharedContact can encode to (a node number, a whole `User` with its 32-byte key and 40-byte
 * long name, two flags), base64 costs four characters per three bytes, and the prefix is fixed.
 * A realistic contact is most of this - unlike a channel set there is no eight-of-everything
 * worst case to be far away from, which is what makes the link short enough to read out loud.
 */
#define MESH_CONTACT_URL_MAX                                                                       \
    (sizeof(MESH_CONTACT_URL_PREFIX) + ((meshtastic_SharedContact_size + 2U) / 3U) * 4U)

/*
 * Writes `contact` as a link. Returns the characters written, or 0 when the contact is not one
 * this module will write or the buffer is too small - in which case `out` is left empty,
 * because half a link decodes to a different node or to nothing.
 *
 * What it will not write is a contact with no node number or no public key, which is the same
 * bar mesh_radio_settings_queue_contact() holds the admin verb to: an entry with no key is what
 * the radio would build for itself the moment the node transmitted, so a link carrying one
 * promises a reader something it does not deliver.
 */
size_t mesh_contact_url_encode(const meshtastic_SharedContact *contact, char *out, size_t out_len);

/*
 * Reads a link. Returns false when `text` is not one, or when the payload is not a usable
 * SharedContact.
 *
 * Forgiving about the wrapper and not at all about the payload, exactly as the channel decoder
 * is: any host (a link out of a regional community's own site is the same link), and a bare
 * payload with no URL around it at all - which is what somebody typing one in on a games
 * console's on-screen keyboard will want to do. "Usable" is the node number and the key, for
 * the reason the encoder gives; refusing here is what keeps every caller from checking.
 *
 * Note what this does *not* do: it reports `should_ignore` and `manually_verified` exactly as
 * the sender wrote them, because a wire format's job is to say what the bytes said. Deciding
 * that a stranger does not get to set either of those on this radio is
 * mesh/core/contact_share.h's, and it is made there.
 */
bool mesh_contact_url_decode(const char *text, meshtastic_SharedContact *out);

#ifdef __cplusplus
}
#endif

#endif /* MESH_PROTO_CONTACT_URL_H */
