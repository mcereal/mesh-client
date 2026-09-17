#define _POSIX_C_SOURCE 200809L

/*
 * Values on the wire <-> text a person types.
 *
 * Decimals (a coordinate's 1e-7 degrees, a frequency's megahertz), node numbers, and channel
 * keys (raw bytes, offered as both hex and the base64 the Meshtastic apps show). Both
 * directions are here so the parse and the format stay in step: a key rendered one way and
 * parsed another is a channel that silently stops decrypting.
 *
 * A leaf - nothing outside the public header in include/mesh/ui/settings.h.
 */

#include "mesh/ui/settings.h"

#include "mesh/utils/base64.h"
#include "mesh/utils/text.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Ten to the power of `digits`, for digits a field table can plausibly ask for.
 *
 * A table rather than a loop because both directions want it as a constant and there are nine
 * of them: past 1e9 an int64 scaled value stops being able to hold a whole number anybody
 * would type.
 */
static int64_t decimal_scale(uint32_t digits) {
    static const int64_t k_powers[] = {1,      10,      100,      1000,      10000,
                                       100000, 1000000, 10000000, 100000000, 1000000000};
    return digits < (sizeof k_powers / sizeof k_powers[0]) ? k_powers[digits] : k_powers[9];
}

void mesh_ui_settings_decimal_text(int64_t scaled, uint32_t held_digits, uint32_t shown_digits,
                                   char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (shown_digits > held_digits) {
        shown_digits = held_digits;
    }
    /* Rounded off rather than truncated at the shown width: a coordinate held to seven places
       and shown to five is a metre either way, and the digit that decides which is the first
       one dropped. */
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

/*
 * A decimal a person typed, as an integer scaled by `digits` places.
 *
 * Parsed digit by digit rather than through strtod, which is the whole reason it exists: a
 * coordinate wants exactly seven decimal places and a frequency four, and a double rounds the
 * last of them somewhere the user cannot see. Everything this rejects it rejects outright -
 * there is no half-understood reading of "44.6N" worth guessing at.
 */
bool mesh_ui_settings_decimal_parse(const char *text, uint32_t digits, int64_t limit_whole,
                                    int64_t *out_scaled) {
    if (text == NULL || out_scaled == NULL || limit_whole <= 0 || digits > 9U) {
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
                /* Finer than the field can hold, and a digit that means something. Refused
                   rather than dropped: a slot row taking "12.5" as 12, or a frequency taking a
                   tenth of a hertz it cannot carry, is the silent kind of wrong this whole file
                   is written to avoid. Trailing zeros are not that and are allowed through. */
                return false;
            }
            any_digit = true;
            ++p;
        }
    }
    if (!any_digit) {
        /* A bare "." or "-." got past the first check and would otherwise read as zero, which
           the (0, 0) guard downstream cannot catch when the other coordinate is real. */
        return false;
    }
    while (*p == ' ') {
        ++p;
    }
    if (*p != '\0') {
        return false; /* trailing rubbish: "44.6N" is not a number we will guess at */
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

/*
 * Coordinates: the same decimal, at the seven places the wire wants.
 *
 * Five are shown. That is about a metre, which is finer than anything a LoRa node reports and
 * short enough to type back in on a ten-column keyboard.
 */
void mesh_ui_settings_coord_text(int32_t value_i, char *out, size_t out_len) {
    mesh_ui_settings_decimal_text(value_i, MESH_UI_COORD_DIGITS, 5U, out, out_len);
}

bool mesh_ui_settings_coord_parse(const char *text, int32_t limit_degrees, int32_t *out_i) {
    int64_t scaled = 0;
    if (out_i == NULL || !mesh_ui_settings_decimal_parse(text, MESH_UI_COORD_DIGITS,
                                                         (int64_t)limit_degrees, &scaled)) {
        return false;
    }
    *out_i = (int32_t)scaled;
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

/*
 * A node number, as the "!433d1b2c" the apps and the logs both write.
 *
 * The bare eight hex digits are taken too, and so is a plain decimal - somebody reading a
 * number off another screen should not have to know which of the three they are looking at.
 * Zero is not a node number: it is what an empty row parses to, and the caller reads it as
 * "this slot is unused" rather than as an address.
 */
void mesh_ui_settings_node_id_text(uint32_t node_id, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (node_id == 0U) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_len, "!%08x", (unsigned)node_id);
}

bool mesh_ui_settings_node_id_parse(const char *text, uint32_t *out_id) {
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
    if (*p == '!') {
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
         * "12345678" is a legal node number read either way, and guessing hex on a width - as
         * this first did - makes eight digits mean something seven and nine do not, which is
         * not a rule anybody could predict and quietly ignores a node nobody named. The hex
         * reading always has a spelling available: "!12345678" is how this client, the apps and
         * the logs all write one, so nothing is lost by asking for the marker.
         */
        bool all_digits = true;
        for (const char *q = p; q < end; ++q) {
            all_digits = all_digits && *q >= '0' && *q <= '9';
        }
        hex = !all_digits;
    }
    if (p == end) {
        return false; /* a bare "!" names nothing */
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

void mesh_ui_settings_key_hex(const uint8_t *key, size_t len, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if (key == NULL) {
        return;
    }
    size_t pos = 0U;
    for (size_t i = 0; i < len && pos + 3U <= out_len; ++i) {
        snprintf(out + pos, out_len - pos, "%02x", key[i]);
        pos += 2U;
    }
}

void mesh_ui_settings_key_text(const uint8_t *key, size_t len, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if (key == NULL) {
        return;
    }
    /* The standard alphabet, padded: this is the form the Meshtastic apps show a key in and the
       form mesh_ui_settings_key_parse() reads back. The URL-safe one belongs to a channel URL
       and nowhere near a field somebody types into. */
    (void)mesh_base64_encode(key, len, false, out, out_len);
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

bool mesh_ui_settings_key_parse(const char *text, uint8_t *out, size_t out_cap, size_t *out_len) {
    if (text == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    const size_t chars = strlen(text);
    if (chars == 0U) {
        *out_len = 0U;
        return true;
    }
    /* Hex first: a base64 string made only of hex digits is ambiguous, and hex is what the
       firmware logs show. Only the three key sizes are hex; anything else is base64. */
    bool all_hex = true;
    for (size_t i = 0; i < chars; ++i) {
        if (hex_nibble(text[i]) < 0) {
            all_hex = false;
            break;
        }
    }
    if (all_hex && (chars == 2U || chars == 32U || chars == 64U)) {
        return parse_hex(text, chars, out, out_cap, out_len);
    }
    /* Padded, and every character accounted for: a key is a fixed number of bytes, a mistyped
       one decodes to a plausible wrong key, and nothing on the wire says so - the radio simply
       stops hearing the mesh. See mesh/utils/base64.h. */
    return mesh_base64_decode(text, chars, MESH_BASE64_PADDED, out, out_cap, out_len);
}
