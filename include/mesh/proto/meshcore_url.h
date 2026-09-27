#ifndef MESH_PROTO_MESHCORE_URL_H
#define MESH_PROTO_MESHCORE_URL_H

/*
 * MeshCore's contact and channel links, as the MeshCore apps write them into a QR code.
 *
 * The contact link: one node's key, name and kind.
 *
 * `meshcore://contact/add?name=<name>&public_key=<64 hex>&type=<n>`, from the firmware's
 * docs/qr_codes.md. Unlike Meshtastic's link it is a plain query string rather than a protobuf,
 * so a name is percent-encoded (a space may also be `+`) and the key is hex. It carries no
 * signature and no route: what it gives a radio is a contact to add by key, the same record an
 * advert would have made, with the first message flooding.
 *
 * `src/proto/` for mesh/proto/contact_url.h's reason: text in, text out, no radio.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_MESHCORE_URL_CONTACT_PREFIX "meshcore://contact/add?"
#define MESH_MESHCORE_URL_KEY_LEN 32U
#define MESH_MESHCORE_URL_NAME_LEN 32U

/*
 * The longest contact link this module writes, NUL included: the prefix, a name of 32 bytes
 * each percent-encoded, a 64-character key and a one-digit type.
 */
#define MESH_MESHCORE_CONTACT_URL_MAX                                                              \
    (sizeof(MESH_MESHCORE_URL_CONTACT_PREFIX) + sizeof("name=") - 1U +                             \
     MESH_MESHCORE_URL_NAME_LEN * 3U + sizeof("&public_key=") - 1U +                               \
     MESH_MESHCORE_URL_KEY_LEN * 2U + sizeof("&type=") - 1U + 3U)

struct mesh_meshcore_contact_link {
    uint8_t public_key[MESH_MESHCORE_URL_KEY_LEN];
    char name[MESH_MESHCORE_URL_NAME_LEN + 1U]; /* may be empty */
    uint8_t type; /* 1 companion, 2 repeater, 3 room server, 4 sensor */
};

/*
 * Writes `link` as a contact link. Returns the characters written, or 0 when the type is not
 * one of the four or `out` is too small - in which case `out` is left empty.
 */
size_t mesh_meshcore_contact_url_encode(const struct mesh_meshcore_contact_link *link, char *out,
                                        size_t out_len);

/*
 * Reads a contact link. True only for the `contact/add` path with a whole 64-character key and
 * a type of 1 to 4; the name may be missing. Parameters may come in any order, and one this
 * module does not know is skipped, since the apps add optional ones over time.
 */
bool mesh_meshcore_contact_url_decode(const char *text, struct mesh_meshcore_contact_link *out);

/*
 * MeshCore's channel link: one channel's name and 16-byte secret.
 *
 * `meshcore://channel/add?name=<name>&secret=<32 hex>`, from the same docs/qr_codes.md. One link
 * is one channel, where Meshtastic's is the radio's whole set. The name is at most 31 bytes, as
 * SET_CHANNEL keeps it; the app's optional `region_scope` is skipped.
 */
#define MESH_MESHCORE_URL_CHANNEL_PREFIX "meshcore://channel/add?"
#define MESH_MESHCORE_URL_CHANNEL_NAME_LEN 31U
#define MESH_MESHCORE_URL_SECRET_LEN 16U

#define MESH_MESHCORE_CHANNEL_URL_MAX                                                              \
    (sizeof(MESH_MESHCORE_URL_CHANNEL_PREFIX) + sizeof("name=") - 1U +                             \
     MESH_MESHCORE_URL_CHANNEL_NAME_LEN * 3U + sizeof("&secret=") - 1U +                           \
     MESH_MESHCORE_URL_SECRET_LEN * 2U)

struct mesh_meshcore_channel_link {
    char name[MESH_MESHCORE_URL_CHANNEL_NAME_LEN + 1U];
    uint8_t secret[MESH_MESHCORE_URL_SECRET_LEN];
};

/* Writes `link` as a channel link. Returns the characters written, or 0 for an empty or
   too-long name or an `out` too small - in which case `out` is left empty. */
size_t mesh_meshcore_channel_url_encode(const struct mesh_meshcore_channel_link *link, char *out,
                                        size_t out_len);

/* Reads a channel link: true only for the `channel/add` path with a name and a whole
   32-character secret, parameters in any order and unknown ones skipped. */
bool mesh_meshcore_channel_url_decode(const char *text, struct mesh_meshcore_channel_link *out);

#ifdef __cplusplus
}
#endif

#endif /* MESH_PROTO_MESHCORE_URL_H */
