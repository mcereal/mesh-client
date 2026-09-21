#ifndef MESH_UI_STORE_FIELDS_H
#define MESH_UI_STORE_FIELDS_H

/*
 * The other half of the cache format: the comma-separated value, read as a list of typed
 * destinations rather than as a printf format string beside a hand-counted field total.
 *
 * store_keys.h spells a line's key once, in a table both halves of the format share. This
 * spells a line's *value* once, in the same spirit. A loader case used to be a block of
 * `unsigned int` scratch variables, an sscanf whose format had to agree with them, a literal
 * count that had to agree with both, and a run of `(bool)(x != 0U)` casts copying the scratch
 * into the record. Three of those four could drift apart without the compiler noticing, and
 * one of them - the count - is the thing the format's compatibility rules turn on.
 *
 * Here the field list *is* the format, the destination's type picks the conversion, and the
 * total comes off the array. See docs/ui.md for the cache as a whole.
 *
 * Stricter than the sscanf() it replaced, deliberately, exactly as
 * mesh_ui_store_key_lookup() is stricter than the one *it* replaced. The cache is a text file
 * on a card the user can edit, so it is an ingress like the air is:
 *
 *   - A number too wide for its destination fails rather than wrapping or truncating. `%u`
 *     into an `unsigned int` and a cast down to `uint8_t` turned a battery level of 300 into
 *     44; a value that does not fit now drops the record instead of dressing up as a
 *     plausible reading. This is also what retired the loader's widest-scan-then-bound dance
 *     for a position: `%d` on a coordinate past INT32_MAX is undefined, and glibc's answer is
 *     0 - a fix in the Gulf of Guinea. An out-of-range coordinate is now simply a field that
 *     did not read.
 *   - A negative number in an unsigned field fails rather than wrapping. glibc's `%u` reads
 *     "-1" as 4294967295.
 *   - A token with trailing rubbish fails rather than yielding its leading digits.
 *
 * None of that can reject a line this client wrote: every writer hands the value a number
 * already inside the destination's range.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a token is converted into. One per destination type the cache actually stores; the
   macro below picks the right one, so nothing outside this header names these. */
enum mesh_ui_store_field_type {
    MESH_UI_STORE_FIELD_TYPE_U32 = 0,
    MESH_UI_STORE_FIELD_TYPE_U16,
    MESH_UI_STORE_FIELD_TYPE_U8,
    MESH_UI_STORE_FIELD_TYPE_I32,
    MESH_UI_STORE_FIELD_TYPE_I16,
    MESH_UI_STORE_FIELD_TYPE_BOOL,
    MESH_UI_STORE_FIELD_TYPE_F32,
};

/* One field of a value: where it goes, and how to get it there. */
struct mesh_ui_store_field {
    enum mesh_ui_store_field_type type;
    void *out;
};

/*
 * A field, named by its destination.
 *
 * `MESH_UI_STORE_FIELD(&metrics.battery_level)` is a `uint8_t` field because `battery_level`
 * is a `uint8_t`, and there is no second place to keep that in step. A destination whose type
 * the cache does not store is a compile error naming this line rather than a silent write of
 * the wrong width through a `void *` - which is the whole reason the type is not written out
 * at the call site.
 *
 * A macro rather than the small static helper the style guide asks for, for the reason
 * INKWELL_ARRAY_LEN is one: the type differs at every call site, and C17 has no other way to
 * dispatch on it. The associations are the exact-width typedefs, so a target where one of them
 * is spelled differently - `uint32_t` as `unsigned long` - fails to compile here rather than
 * anywhere subtler.
 */
#define MESH_UI_STORE_FIELD(ptr)                                                                   \
    ((struct mesh_ui_store_field){_Generic((ptr),                                                  \
                                  uint32_t *: MESH_UI_STORE_FIELD_TYPE_U32,                        \
                                  uint16_t *: MESH_UI_STORE_FIELD_TYPE_U16,                        \
                                  uint8_t *: MESH_UI_STORE_FIELD_TYPE_U8,                          \
                                  int32_t *: MESH_UI_STORE_FIELD_TYPE_I32,                         \
                                  int16_t *: MESH_UI_STORE_FIELD_TYPE_I16,                         \
                                  bool *: MESH_UI_STORE_FIELD_TYPE_BOOL,                           \
                                  float *: MESH_UI_STORE_FIELD_TYPE_F32),                          \
                                  (ptr)})

/*
 * Read a comma-separated value into `count` fields, and say how many arrived.
 *
 * Fields are filled left to right and the walk stops at the first token that is missing or
 * does not convert, so the return is the number of *leading* fields that read - which is what
 * sscanf's return meant, and what the format's compatibility rules are already written
 * against. A caller that needs the whole line compares against the array length; one reading a
 * line that grew a field on the end compares against the length it had before. Fields past the
 * return are left exactly as the caller set them, which is how an absent trailing field keeps
 * its default.
 *
 * Extra tokens past the last field are ignored, as sscanf ignored them.
 */
size_t mesh_ui_store_fields_read(const char *value, const struct mesh_ui_store_field *fields,
                                 size_t count);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UI_STORE_FIELDS_H */
