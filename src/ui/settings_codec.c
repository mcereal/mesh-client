#define _POSIX_C_SOURCE 200809L

/*
 * Values on the wire <-> text a person types.
 *
 * Coordinates (the radio's 1e-7 degree integers) and channel keys (raw bytes, offered as both
 * hex and the base64 the Meshtastic apps show). Both directions are here so the parse and the
 * format stay in step: a key rendered one way and parsed another is a channel that silently
 * stops decrypting.
 *
 * A leaf - nothing outside the public header in include/mesh/ui/settings.h.
 */

#include "mesh/ui/settings.h"

#include "mesh/utils/text.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mesh_ui_settings_coord_text(int32_t value_i, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    /* Five decimals is about a metre, which is finer than anything a LoRa node reports and
       short enough to type back in on a ten-column keyboard. The sign is carried by the whole
       degrees, so the fraction is always taken from the magnitude. */
    const int32_t whole = value_i / 10000000;
    int32_t fraction = value_i % 10000000;
    if (fraction < 0) {
        fraction = -fraction;
    }
    const char *const sign = (value_i < 0 && whole == 0) ? "-" : "";
    snprintf(out, out_len, "%s%d.%05d", sign, (int)whole, (int)(fraction / 100));
}

bool mesh_ui_settings_coord_parse(const char *text, int32_t limit_degrees, int32_t *out_i) {
    if (text == NULL || out_i == NULL || limit_degrees <= 0) {
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

    /* Parsed as integers rather than through strtod: the wire wants exactly seven decimal
       places, and a double would round the last one somewhere the user cannot see. */
    int64_t whole = 0;
    bool any_digit = false;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p - '0');
        any_digit = true;
        if (whole > 1000) {
            return false; /* far past any coordinate; stop before this can overflow */
        }
        ++p;
    }
    int64_t fraction = 0;
    int digits = 0;
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            if (digits < 7) {
                fraction = fraction * 10 + (*p - '0');
                ++digits;
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
        return false; /* trailing rubbish: "44.6N" is not a coordinate we will guess at */
    }
    for (; digits < 7; ++digits) {
        fraction *= 10;
    }

    int64_t value = whole * 10000000 + fraction;
    if (value > (int64_t)limit_degrees * 10000000) {
        return false;
    }
    *out_i = (int32_t)(negative ? -value : value);
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

static const char k_base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void mesh_ui_settings_key_text(const uint8_t *key, size_t len, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if (key == NULL || (len + 2U) / 3U * 4U + 1U > out_len) {
        return;
    }
    size_t pos = 0U;
    for (size_t i = 0; i < len; i += 3U) {
        const uint32_t b0 = key[i];
        const uint32_t b1 = i + 1U < len ? key[i + 1U] : 0U;
        const uint32_t b2 = i + 2U < len ? key[i + 2U] : 0U;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out[pos++] = k_base64[(triple >> 18) & 0x3FU];
        out[pos++] = k_base64[(triple >> 12) & 0x3FU];
        out[pos++] = i + 1U < len ? k_base64[(triple >> 6) & 0x3FU] : '=';
        out[pos++] = i + 2U < len ? k_base64[triple & 0x3FU] : '=';
    }
    out[pos] = '\0';
}

static int base64_value(char c) {
    const char *at = c != '\0' ? strchr(k_base64, c) : NULL;
    return at != NULL ? (int)(at - k_base64) : -1;
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

static bool parse_base64(const char *text, size_t chars, uint8_t *out, size_t out_cap,
                         size_t *out_len) {
    if (chars % 4U != 0U) {
        return false;
    }
    size_t len = 0U;
    for (size_t i = 0; i < chars; i += 4U) {
        int values[4];
        unsigned pad = 0U;
        for (unsigned j = 0; j < 4U; ++j) {
            const char c = text[i + j];
            if (c == '=') {
                /* Padding only in the last group's last two places. */
                if (i + 4U != chars || j < 2U) {
                    return false;
                }
                pad++;
                values[j] = 0;
                continue;
            }
            if (pad > 0U) {
                return false;
            }
            values[j] = base64_value(c);
            if (values[j] < 0) {
                return false;
            }
        }
        const uint32_t triple = ((uint32_t)values[0] << 18) | ((uint32_t)values[1] << 12) |
                                ((uint32_t)values[2] << 6) | (uint32_t)values[3];
        const unsigned bytes = 3U - pad;
        if (len + bytes > out_cap) {
            return false;
        }
        out[len++] = (uint8_t)(triple >> 16);
        if (bytes > 1U) {
            out[len++] = (uint8_t)(triple >> 8);
        }
        if (bytes > 2U) {
            out[len++] = (uint8_t)triple;
        }
    }
    *out_len = len;
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
    return parse_base64(text, chars, out, out_cap, out_len);
}
