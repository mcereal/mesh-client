#ifndef MESH_UTILS_BASE64_H
#define MESH_UTILS_BASE64_H

/*
 * Base64, in the two alphabets this client meets and the two strictnesses it reads them at.
 *
 * The standard alphabet (RFC 4648 §4, `+/` with `=` padding) is what a channel key is typed and
 * shown in, beside its hex form. The URL-safe one (§5, `-_`, padding dropped) is what a
 * Meshtastic channel URL carries, because the payload sits in a fragment where `+` and `/`
 * would have to be escaped and `=` reads as a delimiter.
 *
 * The decoder takes *either* alphabet whichever form the caller last encoded in: the two differ
 * in exactly two characters and neither uses the other's pair, so accepting both rejects
 * nothing real. What the caller does choose is how forgiving to be about padding, and that is
 * not a style question - it is the difference between two jobs:
 *
 * - MESH_BASE64_PADDED is for a value somebody *typed into a field*. A key is a fixed number
 *   of bytes, a mistyped one decodes to a plausible wrong key, and the radio cannot tell us we
 *   got it wrong - it just stops talking to the mesh. Every character has to be accounted for.
 * - MESH_BASE64_ANY is for a payload out of a *URL*, where the padding may or may not have
 *   survived whatever passed the link along, and the bytes underneath are a protobuf that
 *   fails loudly if they are wrong.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Characters a padded encoding of `n` bytes needs, NUL included. An unpadded one is never
   longer, so this is the buffer size for either. */
#define MESH_BASE64_ENCODED_MAX(n) ((((n) + 2U) / 3U) * 4U + 1U)

/* Bytes a decoding of `chars` characters can yield. An upper bound: padding and a short final
   group both make the answer smaller. */
#define MESH_BASE64_DECODED_MAX(chars) ((((chars) + 3U) / 4U) * 3U)

/* How much a decoder insists on. See the header note: this is the job, not the taste. */
enum mesh_base64_form {
    MESH_BASE64_PADDED = 0, /* whole groups of four, `=` only in the last group's last two */
    MESH_BASE64_ANY,        /* padding optional; a short final group carries what it carries */
};

/*
 * Encodes `len` bytes into `out`.
 *
 * `url_safe` picks the alphabet and, with it, the padding: the URL-safe form is written
 * unpadded, which is what every Meshtastic client produces and the only form that survives a
 * URL fragment untouched. Returns the number of characters written (never counting the NUL),
 * or 0 when the buffer is too small - in which case `out` is left empty rather than cut, since
 * half a base64 string decodes to plausible nonsense.
 */
size_t mesh_base64_encode(const uint8_t *data, size_t len, bool url_safe, char *out,
                          size_t out_len);

/*
 * Decodes `chars` characters of `text` into `out`.
 *
 * Rejects anything outside the two alphabets - whitespace included - and anything `form`
 * refuses. A final group of one character is refused by both: it carries no whole byte, so it
 * is a truncated string rather than a short one. Returns false and writes nothing on any of
 * those, or when the result would not fit; `*out_len` is the byte count on success.
 */
bool mesh_base64_decode(const char *text, size_t chars, enum mesh_base64_form form, uint8_t *out,
                        size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UTILS_BASE64_H */
