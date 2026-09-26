#ifndef MESH_PROTO_MESHCORE_URL_H
#define MESH_PROTO_MESHCORE_URL_H

/*
 * MeshCore's contact link: one node's key, name and kind as the MeshCore apps write it into a
 * QR code.
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

#ifdef __cplusplus
}
#endif

#endif /* MESH_PROTO_MESHCORE_URL_H */
