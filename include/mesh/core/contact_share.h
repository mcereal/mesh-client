#ifndef MESH_CORE_CONTACT_SHARE_H
#define MESH_CORE_CONTACT_SHARE_H

/*
 * The two directions of contact sharing, between a radio and a `SharedContact`.
 *
 * mesh/proto/contact_url.h turns a SharedContact into a link and back; this is the half that
 * knows what a radio is, and it is separate for mesh/core/channel_share.h's reason: the link is
 * the same message whatever is holding it, and *which record on this radio it is built from,
 * and which fields of an incoming one this radio will honour* are facts about this radio and
 * this client's queue.
 *
 * Coming in is the direction that matters here, and it is the gap the admin verb alone could
 * not close. `MESH_ADMIN_ADD_CONTACT` has been wired since key trust landed, but every contact
 * it could be given came from the roster - which means from a node that had already
 * transmitted, and a node that has transmitted is one the radio could have kept for itself. A
 * link is the only way to obtain a contact for a node that has *never* been heard: it carries
 * the public key ahead of the node, so the first direct message to it can be encrypted instead
 * of waiting on a NodeInfo that may be hours away.
 *
 * Going out is the cheap half and the one a camera is not needed for: the Brick can never scan
 * a code, but it can show one, and a phone beside it adds this radio in a press.
 */

#include "mesh/core/radio_settings.h"
#include "meshtastic/admin.pb.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Whether the radio has told us enough about itself to be shared: its owner record, and a
 * public key to go in it.
 *
 * The gate on the outgoing direction, and asked separately because the *row* needs it too - a
 * row offering to show a code before the owner reply has landed is a row that would open an
 * empty screen. Unlike a channel table this is one reply rather than nine, so the window is
 * short; it is still a window, and a radio with PKC turned off never leaves it at all.
 */
bool mesh_contact_share_settled(const struct mesh_radio_settings *settings);

/*
 * Builds this radio's own record as a contact: its node number, its owner, and its public key.
 * Returns false when the radio has not finished answering (mesh_contact_share_settled).
 *
 * Two fields are deliberately *not* carried, and they are the two a link could otherwise use to
 * make a claim about the reader's radio rather than about ours:
 *
 *   `should_ignore` says "drop this node's packets before reading them", which is a decision
 *   about the reader's own radio with its own verb to make it. A link that could set it would
 *   be a way to talk a radio into going deaf.
 *
 *   `manually_verified` sets the bit that means "a person checked this key out of band". We
 *   cannot assert that on the reader's behalf - the whole point of the ceremony in
 *   src/core/session/key_verification.c is that it happens between two people over a second channel
 * - so a code claiming it would be laundering trust through a picture on a screen.
 *
 * Both are left false here and dropped again on the way in (mesh_contact_share_queue_import),
 * which is belt and braces on purpose: the two ends are written by different people and only
 * one of them is ours.
 */
bool mesh_contact_share_build(const struct mesh_radio_settings *settings,
                              meshtastic_SharedContact *out);

/* The same thing as a link, through mesh_contact_url_encode(). Returns the characters written,
   0 when there is nothing to share or the buffer is too small. */
size_t mesh_contact_share_url(const struct mesh_radio_settings *settings, char *out,
                              size_t out_len);

/*
 * Queues the `add_contact` write for a contact that arrived in a link.
 *
 * A thin layer over mesh_radio_settings_queue_contact() and worth its own function for what it
 * does before calling it: `should_ignore` and `manually_verified` are cleared, whatever the
 * link said. See mesh_contact_share_build() for why those two and not the rest - the short
 * version is that everything else in a contact is a statement about the *sender's* node, which
 * is theirs to make, and those two are instructions to the *reader's* radio, which are not.
 *
 * Returns the number of requests queued, or a negative errno: -EINVAL for a NULL argument or a
 * contact with no node number or key, -ENOSPC when the queue is full.
 */
int mesh_contact_share_queue_import(struct mesh_radio_settings *settings,
                                    const meshtastic_SharedContact *contact);

#ifdef __cplusplus
}
#endif

#endif /* MESH_CORE_CONTACT_SHARE_H */
