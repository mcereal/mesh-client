#define _POSIX_C_SOURCE 200809L

/* See mesh/utils/base64.h for why there are two alphabets and two strictnesses. */

#include "mesh/utils/base64.h"

#include <string.h>

static const char k_standard[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char k_url_safe[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t mesh_base64_encode(const uint8_t *data, size_t len, bool url_safe, char *out,
                          size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return 0U;
    }
    out[0] = '\0';
    if (data == NULL && len > 0U) {
        return 0U;
    }
    /* Unpadded spends nothing on the '=' a padded encoding would end with. */
    const size_t tail = len % 3U;
    size_t chars = ((len + 2U) / 3U) * 4U;
    if (url_safe && tail != 0U) {
        chars -= (3U - tail);
    }
    if (chars + 1U > out_len) {
        return 0U;
    }

    const char *alphabet = url_safe ? k_url_safe : k_standard;
    size_t pos = 0U;
    for (size_t i = 0; i < len; i += 3U) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = i + 1U < len ? data[i + 1U] : 0U;
        const uint32_t b2 = i + 2U < len ? data[i + 2U] : 0U;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out[pos++] = alphabet[(triple >> 18) & 0x3FU];
        out[pos++] = alphabet[(triple >> 12) & 0x3FU];
        if (i + 1U < len) {
            out[pos++] = alphabet[(triple >> 6) & 0x3FU];
        } else if (!url_safe) {
            out[pos++] = '=';
        }
        if (i + 2U < len) {
            out[pos++] = alphabet[triple & 0x3FU];
        } else if (!url_safe) {
            out[pos++] = '=';
        }
    }
    out[pos] = '\0';
    return pos;
}

/* A character's value in whichever alphabet it belongs to, or -1. The two differ in exactly two
   places and neither uses the other's pair, so one lookup answers for both. */
static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+' || c == '-') {
        return 62;
    }
    if (c == '/' || c == '_') {
        return 63;
    }
    return -1;
}

bool mesh_base64_decode(const char *text, size_t chars, enum mesh_base64_form form, uint8_t *out,
                        size_t out_cap, size_t *out_len) {
    if (text == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    if (form == MESH_BASE64_PADDED && chars % 4U != 0U) {
        return false;
    }
    /* Padding is counted rather than skipped, so that a '=' anywhere but the tail is left in
       the string for base64_value() to refuse. */
    size_t payload = chars;
    while (payload > 0U && text[payload - 1U] == '=') {
        payload--;
    }
    if (chars - payload > 2U || payload % 4U == 1U) {
        return false;
    }

    size_t len = 0U;
    for (size_t i = 0; i < payload; i += 4U) {
        const size_t group = payload - i < 4U ? payload - i : 4U;
        uint32_t quad = 0U;
        for (size_t j = 0; j < 4U; ++j) {
            int value = 0;
            if (j < group) {
                value = base64_value(text[i + j]);
                if (value < 0) {
                    return false;
                }
            }
            quad = (quad << 6) | (uint32_t)value;
        }
        /* A group of n characters carries n - 1 bytes; the bits below them are the zero
           padding the encoder wrote, and are dropped rather than checked. */
        const size_t bytes = group - 1U;
        if (len + bytes > out_cap) {
            return false;
        }
        const uint8_t triple[3] = {(uint8_t)(quad >> 16), (uint8_t)(quad >> 8), (uint8_t)quad};
        for (size_t j = 0; j < bytes; ++j) {
            out[len++] = triple[j];
        }
    }
    *out_len = len;
    return true;
}
