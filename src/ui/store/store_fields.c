/*
 * Reading one cache line's value into typed destinations. See store_fields.h for why the field
 * list replaced a format string, and for the strictness this adds on the way.
 */

#include "mesh/ui/store_fields.h"

#include <errno.h>
#include <stdlib.h>

/* Where this token ends: the next comma, or the end of the value. */
static const char *field_token_end(const char *start) {
    const char *stop = start;
    while (*stop != '\0' && *stop != ',') {
        ++stop;
    }
    return stop;
}

/*
 * One unsigned number that fits `limit`, and nothing else in the token.
 *
 * The leading-digit test is load-bearing rather than defensive: strtoull accepts a sign and
 * negates, so without it "-1" would arrive as UINT64_MAX and pass every range check below it
 * by wrapping into range. Checked for ERANGE the way mesh_env_parse_int() checks it, because
 * where `unsigned long long` is the widest type the target has, saturation is the only thing
 * that can still tell a number at the limit from one far past it.
 */
static bool field_unsigned(const char *start, const char *stop, uint64_t limit, uint64_t *out) {
    if (start == stop || *start < '0' || *start > '9') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(start, &end, 10);
    if (end != stop || errno == ERANGE || (uint64_t)value > limit) {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

/* The same, between two bounds, for a field that can be negative. */
static bool field_signed(const char *start, const char *stop, int64_t low, int64_t high,
                         int64_t *out) {
    if (start == stop) {
        return false;
    }
    if (*start != '-' && *start != '+' && (*start < '0' || *start > '9')) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const long long value = strtoll(start, &end, 10);
    if (end != stop || errno == ERANGE || (int64_t)value < low || (int64_t)value > high) {
        return false;
    }
    *out = (int64_t)value;
    return true;
}

/*
 * One real number, as the writer's "%f" spelled it.
 *
 * ERANGE is not a failure here, unlike above: strtod saturates to an infinity the same way
 * sscanf's "%lf" did, and a reading that overflowed a double on the way out is already not a
 * reading. What is rejected is a token that is not wholly a number.
 */
static bool field_real(const char *start, const char *stop, float *out) {
    if (start == stop || *start == ' ' || *start == '\t') {
        return false;
    }
    char *end = NULL;
    const double value = strtod(start, &end);
    if (end != stop) {
        return false;
    }
    *out = (float)value;
    return true;
}

static bool field_read(const struct mesh_ui_store_field *field, const char *start,
                       const char *stop) {
    if (field->out == NULL) {
        return false;
    }

    uint64_t unsigned_value = 0U;
    int64_t signed_value = 0;
    switch (field->type) {
    case MESH_UI_STORE_FIELD_TYPE_U32:
        if (!field_unsigned(start, stop, UINT32_MAX, &unsigned_value)) {
            return false;
        }
        *(uint32_t *)field->out = (uint32_t)unsigned_value;
        return true;
    case MESH_UI_STORE_FIELD_TYPE_U16:
        if (!field_unsigned(start, stop, UINT16_MAX, &unsigned_value)) {
            return false;
        }
        *(uint16_t *)field->out = (uint16_t)unsigned_value;
        return true;
    case MESH_UI_STORE_FIELD_TYPE_U8:
        if (!field_unsigned(start, stop, UINT8_MAX, &unsigned_value)) {
            return false;
        }
        *(uint8_t *)field->out = (uint8_t)unsigned_value;
        return true;
    case MESH_UI_STORE_FIELD_TYPE_I32:
        if (!field_signed(start, stop, INT32_MIN, INT32_MAX, &signed_value)) {
            return false;
        }
        *(int32_t *)field->out = (int32_t)signed_value;
        return true;
    case MESH_UI_STORE_FIELD_TYPE_I16:
        if (!field_signed(start, stop, INT16_MIN, INT16_MAX, &signed_value)) {
            return false;
        }
        *(int16_t *)field->out = (int16_t)signed_value;
        return true;
    case MESH_UI_STORE_FIELD_TYPE_BOOL:
        /* Written as 0 or 1, and read as "anything that is not 0" - so a hand-edited 2 is
           still true rather than a dropped record. The width is the widest the token can
           carry, because the question is only whether it is zero. */
        if (!field_unsigned(start, stop, UINT64_MAX, &unsigned_value)) {
            return false;
        }
        *(bool *)field->out = (unsigned_value != 0U);
        return true;
    case MESH_UI_STORE_FIELD_TYPE_F32:
        return field_real(start, stop, (float *)field->out);
    }
    return false;
}

size_t mesh_ui_store_fields_read(const char *value, const struct mesh_ui_store_field *fields,
                                 size_t count) {
    if (value == NULL || fields == NULL) {
        return 0U;
    }

    const char *cursor = value;
    size_t read = 0U;
    while (read < count) {
        const char *stop = field_token_end(cursor);
        if (!field_read(&fields[read], cursor, stop)) {
            break;
        }
        ++read;
        if (*stop == '\0') {
            break;
        }
        cursor = stop + 1;
    }
    return read;
}
