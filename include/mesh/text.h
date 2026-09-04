#ifndef MESH_TEXT_H
#define MESH_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * UTF-8 helpers shared by everything that touches text from the radio.
 *
 * Names and message bodies are chosen by whoever owns the node at the other end, so they are
 * untrusted bytes that end up in a framebuffer, a log line and a JSON document. Meshtastic's
 * own conventions make multi-byte text the norm rather than the exception: `User.short_name` is
 * `char[5]`, sized for exactly one four-byte emoji plus its NUL.
 */

/* What a decoder yields for a byte that is not part of a well-formed sequence. */
#define MESH_TEXT_INVALID_CODEPOINT 0xFFFDU

/*
 * Length of the well-formed UTF-8 sequence starting at `bytes`, or 0 when the bytes there are
 * not one. Rejects overlong forms, UTF-16 surrogates and anything above U+10FFFF, per RFC 3629.
 *
 * A lenient decoder is not good enough here: JSON text must be valid UTF-8, so a malformed byte
 * copied through to --status --json would produce a document that standards-compliant parsers
 * reject - and the bytes come from the radio, where any node can choose them.
 */
size_t mesh_text_utf8_sequence_len(const uint8_t *bytes, size_t available);

/*
 * Decode the character starting at `text` (NUL-terminated). Returns the number of bytes it
 * occupies and stores its codepoint in `codepoint`, or returns 0 at the end of the string.
 * A malformed byte consumes one byte and yields MESH_TEXT_INVALID_CODEPOINT, so a caller that
 * loops on the return value always makes progress.
 */
size_t mesh_text_utf8_next(const char *text, uint32_t *codepoint);

/* Number of characters (not bytes) in a NUL-terminated string, counting each malformed byte
   as one. This is what layout code wants: the font draws one glyph per character. */
size_t mesh_text_utf8_length(const char *text);

/* Byte offset of character `index` in `text`, or the offset of the NUL if the string is
   shorter. Use it to truncate without splitting a character in half. */
size_t mesh_text_utf8_offset(const char *text, size_t index);

/* Truncate `text` in place to at most `max_chars` characters. */
void mesh_text_utf8_truncate(char *text, size_t max_chars);

/*
 * Copy `len` bytes of untrusted text into `out`, folding C0 controls and DEL away and replacing
 * any byte that is not part of a well-formed UTF-8 sequence with '?', so no downstream consumer
 * has to re-check it. Well-formed multi-byte characters are copied through whole and are never
 * split by the truncation boundary. `out` is always NUL-terminated.
 */
void mesh_text_sanitise(const uint8_t *payload, size_t len, char *out, size_t out_len);

/* mesh_text_sanitise for a NUL-terminated source. `in` may be NULL, which yields "". */
void mesh_text_sanitise_str(const char *in, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* MESH_TEXT_H */
