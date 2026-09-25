#ifndef MESH_UI_FORM_CODEC_H
#define MESH_UI_FORM_CODEC_H

/*
 * A form's values as the text a person types, and back.
 *
 * Three shapes of value turn up in every form that edits something a machine reads: a decimal
 * held as a scaled integer, a 32-bit identifier, and a run of bytes - a key, a secret, a hash.
 * Each is parsed and printed here, both directions side by side, because the two have to stay
 * in step: a value printed one way and parsed another is one that drifts every time a row is
 * saved without being edited, and for a key it is a secret that is silently a different secret.
 *
 * One rule runs through all of it: **a parse refuses rather than guesses.** Trailing rubbish, a
 * fraction finer than the value can hold, a padding character in the wrong place - each is a
 * `false`, never a best reading. The person is looking at the text; the program is the one that
 * cannot tell a typo from an intention.
 *
 * Nothing here allocates, and nothing reads a locale: a decimal point is always '.'.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The most decimal places a value may be held to. Past 1e9 a scaled int64 stops being able to
   hold a whole part anybody would type. */
#define MESH_UI_FORM_DECIMAL_DIGITS_MAX 9U

/*
 * A decimal held as an integer scaled by `held_digits` places, printed to `shown_digits` of them.
 *
 * Narrower than held is rounded, not cut: the digit that decides is the first one dropped. A
 * value between -1 and 0 keeps its sign, which has nowhere else to live once the whole part is
 * zero. Shown wider than held is shown at held.
 */
void mesh_ui_form_decimal_text(int64_t scaled, uint32_t held_digits, uint32_t shown_digits,
                               char *out, size_t out_len);

/*
 * A decimal a person typed, as an integer scaled by `digits` places.
 *
 * Parsed digit by digit rather than through strtod, which is the whole reason it exists: a value
 * held to exactly seven places wants the seventh to survive, and a double rounds it somewhere
 * the person cannot see. Surrounding spaces and a leading sign are allowed; fewer places than
 * held are padded. Refused: an empty or bare-point row, trailing rubbish, a magnitude past
 * `limit_whole`, and a fraction finer than `digits` that is not zeros - a row taking "12.5" as
 * 12 is the same mistake made quietly.
 */
bool mesh_ui_form_decimal_parse(const char *text, uint32_t digits, int64_t limit_whole,
                                int64_t *out_scaled);

/*
 * A 32-bit identifier, written as `marker` and eight lower-case hex digits: "!433d1b2c".
 *
 * Zero is no identifier, and prints as an empty string: it is what an empty row parses to, and a
 * caller reads it as "this slot is unused".
 */
void mesh_ui_form_id_text(uint32_t id, char marker, char *out, size_t out_len);

/*
 * An identifier in whichever spelling somebody has in front of them: `marker` and hex, "0x" and
 * hex, or bare. Bare, a letter means hex and all digits mean decimal - "12345678" is a legal
 * identifier either way, and the hex reading always has the marker available to ask for it, so
 * the ambiguous spelling goes to the one a reader can predict. Guessing hex on the *width* would
 * make eight digits mean something seven and nine do not.
 *
 * An empty or all-space row is 0 and succeeds - an empty slot is not a bad one, and a parse that
 * refused it would make the row unclearable. Refused: a bare marker, a non-digit, and anything
 * past 32 bits.
 */
bool mesh_ui_form_id_parse(const char *text, char marker, uint32_t *out_id);

/* Bytes as lower-case hex, two digits each. Stops at the last whole byte `out` can hold. */
void mesh_ui_form_bytes_hex(const uint8_t *bytes, size_t len, char *out, size_t out_len);

/* Bytes as padded base64 in the standard alphabet - the form a person copies between programs.
   The URL-safe alphabet belongs to a URL, not to a row somebody types into. */
void mesh_ui_form_bytes_base64(const uint8_t *bytes, size_t len, char *out, size_t out_len);

/*
 * Bytes from text that is hex or base64. An empty string is no bytes and succeeds.
 *
 * Hex is tried only when the text is nothing but hex digits *and* is exactly two digits for one
 * of `hex_sizes` (a list of byte counts) - a base64 string made only of hex digits is ambiguous,
 * and the lengths a caller's values actually come in are what settle it. Anything else must be
 * padded base64 with every character accounted for: a mistyped key decodes to a plausible wrong
 * key, and nothing downstream says so.
 */
bool mesh_ui_form_bytes_parse(const char *text, const size_t *hex_sizes, size_t hex_size_count,
                              uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
