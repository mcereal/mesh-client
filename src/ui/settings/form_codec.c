#define _POSIX_C_SOURCE 200809L

/*
 * A form's values as text and back: decimals, identifiers and bytes. Both directions are here
 * so the parse and the print stay in step - see mesh/ui/form_codec.h.
 */

#include "mesh/ui/form_codec.h"

#include "inkwell/codec/base64.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * Ten to the power of `digits`, for as many places as a value may be held to.
 *
 * A table rather than a loop because both directions want it as a constant and there are ten
 * of them: past 1e9 an int64 scaled value stops being able to hold a whole number anybody
 * would type.
 */
static int64_t decimal_scale(uint32_t digits) {
    static const int64_t k_powers[] = {1,      10,      100,      1000,      10000,
                                       100000, 1000000, 10000000, 100000000, 1000000000};
    return digits < (sizeof k_powers / sizeof k_powers[0]) ? k_powers[digits] : k_powers[9];
}

void mesh_ui_form_decimal_text(int64_t scaled, uint32_t held_digits, uint32_t shown_digits,
                               char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (shown_digits > held_digits) {
        shown_digits = held_digits;
    }
    /* Rounded off rather than truncated at the shown width: the digit that decides which way the
       last shown one goes is the first one dropped. */
    const int64_t drop = decimal_scale(held_digits - shown_digits);
    const bool negative = scaled < 0;
    int64_t magnitude = negative ? -scaled : scaled;
    magnitude = (magnitude + drop / 2) / drop;
    const int64_t unit = decimal_scale(shown_digits);
    const int64_t whole = magnitude / unit;
    const int64_t fraction = magnitude % unit;
    const char *const sign = negative ? "-" : "";
    if (shown_digits == 0U) {
        snprintf(out, out_len, "%s%lld", sign, (long long)whole);
    } else {
        snprintf(out, out_len, "%s%lld.%0*lld", sign, (long long)whole, (int)shown_digits,
                 (long long)fraction);
    }
}

bool mesh_ui_form_decimal_parse(const char *text, uint32_t digits, int64_t limit_whole,
                                int64_t *out_scaled) {
    if (text == NULL || out_scaled == NULL || limit_whole <= 0 ||
        digits > MESH_UI_FORM_DECIMAL_DIGITS_MAX) {
        return false;
    }
    const char *p = text;
    while (*p == ' ') {
        ++p;
    }
    bool negative = false;
    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        ++p;
    }
    if (*p != '.' && (*p < '0' || *p > '9')) {
        return false; /* empty, or something that is not a number at all */
    }

    int64_t whole = 0;
    bool any_digit = false;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p - '0');
        any_digit = true;
        if (whole > limit_whole) {
            return false; /* past the field's range; stop before this can overflow */
        }
        ++p;
    }
    int64_t fraction = 0;
    uint32_t seen = 0U;
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            if (seen < digits) {
                fraction = fraction * 10 + (*p - '0');
                ++seen;
            } else if (*p != '0') {
                /* Finer than the value can hold, and a digit that means something. Refused
                   rather than dropped: a whole-number row taking "12.5" as 12 is the silent kind
                   of wrong this whole file is written to avoid. Trailing zeros are not that and
                   are allowed through. */
                return false;
            }
            any_digit = true;
            ++p;
        }
    }
    if (!any_digit) {
        /* A bare "." or "-." got past the first check and would otherwise read as zero - a real
           value, and one no caller could tell from a typed "0". */
        return false;
    }
    while (*p == ' ') {
        ++p;
    }
    if (*p != '\0') {
        return false; /* trailing rubbish: "44.6N" is not a number to guess at */
    }
    for (; seen < digits; ++seen) {
        fraction *= 10;
    }

    const int64_t value = whole * decimal_scale(digits) + fraction;
    if (value > limit_whole * decimal_scale(digits)) {
        return false;
    }
    *out_scaled = negative ? -value : value;
    return true;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

void mesh_ui_form_id_text(uint32_t id, char marker, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (id == 0U) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_len, "%c%08x", marker, (unsigned)id);
}

bool mesh_ui_form_id_parse(const char *text, char marker, uint32_t *out_id) {
    if (text == NULL || out_id == NULL) {
        return false;
    }
    const char *p = text;
    while (*p == ' ') {
        ++p;
    }
    const char *end = p + strlen(p);
    while (end > p && end[-1] == ' ') {
        --end;
    }
    if (p == end) {
        *out_id = 0U; /* an empty row is an empty slot, not a bad one */
        return true;
    }

    bool hex = false;
    if (*p == marker) {
        hex = true;
        ++p;
    } else if (end - p > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        hex = true;
        p += 2;
    } else {
        /*
         * No marker, so the spelling has to decide, and the only rule that can be stated in one
         * line is: a letter means hex, all digits mean decimal.
         *
         * "12345678" is a legal identifier read either way, and guessing hex on a width makes
         * eight digits mean something seven and nine do not, which is not a rule anybody could
         * predict. The hex reading always has a spelling available - the marker - so nothing is
         * lost by asking for it.
         */
        bool all_digits = true;
        for (const char *q = p; q < end; ++q) {
            all_digits = all_digits && *q >= '0' && *q <= '9';
        }
        hex = !all_digits;
    }
    if (p == end) {
        return false; /* a bare marker names nothing */
    }

    uint64_t value = 0U;
    for (; p < end; ++p) {
        const int digit = hex ? hex_nibble(*p) : (*p >= '0' && *p <= '9' ? *p - '0' : -1);
        if (digit < 0) {
            return false;
        }
        value = value * (hex ? 16U : 10U) + (uint64_t)digit;
        if (value > 0xFFFFFFFFU) {
            return false;
        }
    }
    *out_id = (uint32_t)value;
    return true;
}

void mesh_ui_form_bytes_hex(const uint8_t *bytes, size_t len, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if (bytes == NULL) {
        return;
    }
    size_t pos = 0U;
    for (size_t i = 0; i < len && pos + 3U <= out_len; ++i) {
        snprintf(out + pos, out_len - pos, "%02x", bytes[i]);
        pos += 2U;
    }
}

void mesh_ui_form_bytes_base64(const uint8_t *bytes, size_t len, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if (bytes == NULL) {
        return;
    }
    /* The standard alphabet, padded: the form mesh_ui_form_bytes_parse() reads back. The
       URL-safe one belongs to a URL and nowhere near a field somebody types into. */
    (void)inkwell_base64_encode(bytes, len, false, out, out_len);
}

static bool parse_hex(const char *text, size_t digits, uint8_t *out, size_t out_cap,
                      size_t *out_len) {
    if (digits % 2U != 0U || digits / 2U > out_cap) {
        return false;
    }
    for (size_t i = 0; i < digits; i += 2U) {
        const int hi = hex_nibble(text[i]);
        const int lo = hex_nibble(text[i + 1U]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i / 2U] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = digits / 2U;
    return true;
}

bool mesh_ui_form_bytes_parse(const char *text, const size_t *hex_sizes, size_t hex_size_count,
                              uint8_t *out, size_t out_cap, size_t *out_len) {
    if (text == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    const size_t chars = strlen(text);
    if (chars == 0U) {
        *out_len = 0U;
        return true;
    }
    /* Hex first, but only at a size the caller's values come in: a base64 string made only of
       hex digits is ambiguous, and those sizes are what settle it. Anything else is base64. */
    bool all_hex = true;
    for (size_t i = 0; i < chars; ++i) {
        if (hex_nibble(text[i]) < 0) {
            all_hex = false;
            break;
        }
    }
    if (all_hex && hex_sizes != NULL) {
        for (size_t i = 0; i < hex_size_count; ++i) {
            if (chars == hex_sizes[i] * 2U) {
                return parse_hex(text, chars, out, out_cap, out_len);
            }
        }
    }
    /* Padded, and every character accounted for: a key is a fixed number of bytes, a mistyped
       one decodes to a plausible wrong key, and nothing downstream says so. See
       inkwell/codec/base64.h. */
    return inkwell_base64_decode(text, chars, INKWELL_BASE64_PADDED, out, out_cap, out_len);
}
