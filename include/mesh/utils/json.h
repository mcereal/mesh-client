#pragma once

/*
 * Just enough JSON to walk a document whose shape you already know.
 *
 * A cursor over the text and nothing else: no allocation, no tree, no ownership. You enter an
 * object, ask for its keys in the order they appear, read the value you wanted and skip the
 * ones you did not. Everything it can answer is a value copied into a buffer you named, so a
 * document can be read without ever holding a second copy of it.
 *
 * Why this exists next to the scanner in src/core/updater.c, which also reads JSON: they answer
 * different questions. That one hunts for `"key":` at any depth in a document three levels deep
 * and shallow enough that the first match is the right one, and nothing it finds is trusted
 * without a second check. This one has to read a 150 KB release index whose every entry carries
 * a page of release notes written by whoever merged the pull request - text that contains
 * quotes, braces and, sooner or later, a `"zip_url":` of its own. A scanner would find that one.
 * So this walks structure instead: a string is skipped as a string, and a key is only a key when
 * it is one.
 *
 * Malformed input is a false return and a cursor left where it was, never a read past the end -
 * this reads bytes off the network, and `make fuzz` exists for exactly this shape of code.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How deep a skipped value may nest before the reader gives up.
 *
 * Skipping is iterative and counts rather than recurses, so depth costs nothing but a number;
 * the limit is here so a document that is a megabyte of open brackets is refused rather than
 * walked. Upstream's deepest is four.
 */
#define MESH_JSON_MAX_DEPTH 32U

struct mesh_json {
    const char *cursor;
    const char *end;
};

/* `len` may be 0 for a NUL-terminated string. */
void mesh_json_init(struct mesh_json *json, const char *text, size_t len);

/*
 * Steps into the object or array the cursor is on. False when it is on something else, which
 * is how a caller tells "this document is not the shape I was told" from "this key is absent".
 */
bool mesh_json_enter_object(struct mesh_json *json);
bool mesh_json_enter_array(struct mesh_json *json);

/*
 * Reads the next key of the object being walked and leaves the cursor on its value. False at
 * the closing brace, which it consumes - so a loop over the keys ends with the cursor after
 * the object, ready for whatever follows it.
 *
 * The caller must read or skip the value before asking for the next key.
 */
bool mesh_json_next_key(struct mesh_json *json, char *out, size_t out_len);

/*
 * True when the array being walked has another element, leaving the cursor on it. False at the
 * closing bracket, which it consumes.
 */
bool mesh_json_next_element(struct mesh_json *json);

/*
 * Reads the value the cursor is on and steps past it. A type that does not match is not
 * consumed, so a caller that guessed wrong can still skip it.
 *
 * Strings are unescaped, \uXXXX included; a lone surrogate becomes '?' rather than a broken
 * sequence, because what comes out of here goes on to be measured in cells by src/utils/text.c.
 * A string longer than the buffer is truncated at a whole character and still consumed - the
 * cursor's job is to stay in step with the document, not with the caller's storage.
 */
bool mesh_json_read_string(struct mesh_json *json, char *out, size_t out_len);
bool mesh_json_read_u64(struct mesh_json *json, uint64_t *out);
bool mesh_json_read_bool(struct mesh_json *json, bool *out);

/* Steps past whatever the cursor is on, however deeply nested. False on malformed input or on
   a document nested deeper than MESH_JSON_MAX_DEPTH. */
bool mesh_json_skip_value(struct mesh_json *json);

/*
 * Walks the object the cursor is on to the value of `key`, and leaves the cursor there.
 *
 * The common case in one call, and the one place order matters: it walks forward only, so a
 * caller reading several keys out of one object asks for them in the order the document lists
 * them - or keeps a copy of the cursor (the struct is a value; copying it is the idiom) and
 * searches again from there.
 */
bool mesh_json_object_find(struct mesh_json *json, const char *key);

#ifdef __cplusplus
}
#endif
