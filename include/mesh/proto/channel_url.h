#ifndef MESH_PROTO_CHANNEL_URL_H
#define MESH_PROTO_CHANNEL_URL_H

/*
 * The Meshtastic channel URL: a whole channel set as one link.
 *
 * `https://meshtastic.org/e/#<payload>`, where the payload is a `ChannelSet` protobuf in
 * URL-safe base64 with the padding dropped. It is the phone apps' primary onboarding path -
 * somebody shares the link or shows the QR, everybody else joins - and it is the one piece of
 * the apps that this client could not do at all, in either direction, until this existed.
 *
 * It is `src/proto/` rather than `src/core/` because it is a *wire format and nothing else*:
 * bytes in, bytes out, no radio, no queue, no store. What a radio's channel table has to do with
 * a ChannelSet is mesh/core/channel_share.h's question, and it is a different one - that side
 * knows about slots, roles and admin writes, none of which appear on the wire here.
 *
 * ChannelSet is `apponly.proto`, which is upstream's name for the messages that only ever exist
 * between apps. Nothing here goes over the air.
 */

#include "meshtastic/apponly.pb.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The link this client writes, and the one it prefers to read.
 *
 * `meshtastic.org/e/` has been the form since the `ChannelSet` encoding replaced the older
 * single-channel one; the `/d/` links that predate it are still out there on paper and in old
 * screenshots, so the decoder takes those too.
 */
#define MESH_CHANNEL_URL_PREFIX "https://meshtastic.org/e/#"

/*
 * The longest link a ChannelSet can make, NUL included.
 *
 * From the protobuf's own bound rather than from a guess: nanopb works out how large a
 * ChannelSet can encode to (eight slots, a 256-bit key and a twelve-byte name in each, and a
 * whole LoRaConfig), base64 costs four characters per three bytes, and the prefix is fixed. A
 * realistic share - a primary and a secondary, default keys - is a fifth of this.
 */
#define MESH_CHANNEL_URL_MAX                                                                       \
    (sizeof(MESH_CHANNEL_URL_PREFIX) + ((meshtastic_ChannelSet_size + 2U) / 3U) * 4U +             \
     sizeof("?add=true"))

/*
 * Writes `set` as a link. Returns the characters written, or 0 when `set` is empty or the
 * buffer is too small - in which case `out` is left empty, because half a link is a link that
 * decodes to a different channel.
 *
 * `add` writes the `?add=true` the apps use to mean "keep what you have and add these",
 * which is a claim about the *reader's* radio and so is carried beside the set rather than in
 * it. A set this client is sharing is normally not an add: it is what this radio is on.
 */
size_t mesh_channel_url_encode(const meshtastic_ChannelSet *set, bool add, char *out,
                               size_t out_len);

/*
 * Reads a link. Returns false when `text` is not one, or when the payload is not a ChannelSet.
 *
 * Deliberately forgiving about the wrapper and not at all about the payload. It takes either
 * prefix, any host (a link out of a regional community's own site is the same link), a `?add=`
 * tail, and a bare payload with no URL around it at all - which is what somebody typing one in
 * on a games console's on-screen keyboard will want to do. What it will not do is guess: the
 * bytes either decode to a ChannelSet or they do not.
 *
 * `out_add` is optional and reads the `?add=true` tail: true means the sender meant these
 * channels to join what the reader already has.
 */
bool mesh_channel_url_decode(const char *text, meshtastic_ChannelSet *out, bool *out_add);

#ifdef __cplusplus
}
#endif

#endif /* MESH_PROTO_CHANNEL_URL_H */
