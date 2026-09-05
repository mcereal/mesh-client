#include "mesh/utils/text.h"

#include <string.h>

size_t mesh_text_utf8_sequence_len(const uint8_t *bytes, size_t available) {
    if (bytes == NULL || available == 0U) {
        return 0U;
    }

    const uint8_t lead = bytes[0];
    if (lead < 0x80U) {
        return 1U;
    }

    size_t length;
    uint8_t second_min;
    uint8_t second_max;

    if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2U;
        second_min = 0x80U;
        second_max = 0xBFU;
    } else if (lead == 0xE0U) {
        length = 3U; /* E0 80..9F would be an overlong two-byte form */
        second_min = 0xA0U;
        second_max = 0xBFU;
    } else if (lead == 0xEDU) {
        length = 3U; /* ED A0..BF encodes a UTF-16 surrogate, which is not a character */
        second_min = 0x80U;
        second_max = 0x9FU;
    } else if ((lead >= 0xE1U && lead <= 0xECU) || lead == 0xEEU || lead == 0xEFU) {
        length = 3U;
        second_min = 0x80U;
        second_max = 0xBFU;
    } else if (lead == 0xF0U) {
        length = 4U; /* F0 80..8F would be an overlong three-byte form */
        second_min = 0x90U;
        second_max = 0xBFU;
    } else if (lead >= 0xF1U && lead <= 0xF3U) {
        length = 4U;
        second_min = 0x80U;
        second_max = 0xBFU;
    } else if (lead == 0xF4U) {
        length = 4U; /* F4 90.. would be above U+10FFFF */
        second_min = 0x80U;
        second_max = 0x8FU;
    } else {
        /* 0x80-0xBF is a stray continuation; 0xC0/0xC1 are overlong; 0xF5-0xFF are out of range. */
        return 0U;
    }

    if (available < length) {
        return 0U;
    }
    if (bytes[1] < second_min || bytes[1] > second_max) {
        return 0U;
    }
    for (size_t i = 2; i < length; ++i) {
        if (bytes[i] < 0x80U || bytes[i] > 0xBFU) {
            return 0U;
        }
    }

    return length;
}

size_t mesh_text_utf8_next(const char *text, uint32_t *codepoint) {
    if (text == NULL || *text == '\0') {
        if (codepoint != NULL) {
            *codepoint = 0U;
        }
        return 0U;
    }

    const uint8_t *bytes = (const uint8_t *)text;
    /* Only ever four bytes matter, so bound the scan rather than measuring the whole string:
       this runs once per character for every line the framebuffer draws. */
    size_t available = 0;
    while (available < 4U && text[available] != '\0') {
        available++;
    }
    const size_t length = mesh_text_utf8_sequence_len(bytes, available);
    if (length == 0U) {
        /* Consume exactly one byte so a caller looping on the return value cannot stall. */
        if (codepoint != NULL) {
            *codepoint = MESH_TEXT_INVALID_CODEPOINT;
        }
        return 1U;
    }

    uint32_t value;
    switch (length) {
    case 1U:
        value = bytes[0];
        break;
    case 2U:
        value = (uint32_t)(bytes[0] & 0x1FU) << 6 | (uint32_t)(bytes[1] & 0x3FU);
        break;
    case 3U:
        value = (uint32_t)(bytes[0] & 0x0FU) << 12 | (uint32_t)(bytes[1] & 0x3FU) << 6 |
                (uint32_t)(bytes[2] & 0x3FU);
        break;
    default:
        value = (uint32_t)(bytes[0] & 0x07U) << 18 | (uint32_t)(bytes[1] & 0x3FU) << 12 |
                (uint32_t)(bytes[2] & 0x3FU) << 6 | (uint32_t)(bytes[3] & 0x3FU);
        break;
    }

    if (codepoint != NULL) {
        *codepoint = value;
    }
    return length;
}

size_t mesh_text_utf8_length(const char *text) {
    if (text == NULL) {
        return 0U;
    }

    size_t chars = 0;
    size_t offset = 0;
    for (;;) {
        const size_t step = mesh_text_utf8_next(&text[offset], NULL);
        if (step == 0U) {
            break;
        }
        offset += step;
        chars++;
    }
    return chars;
}

size_t mesh_text_utf8_offset(const char *text, size_t index) {
    if (text == NULL) {
        return 0U;
    }

    size_t offset = 0;
    for (size_t i = 0; i < index; ++i) {
        const size_t step = mesh_text_utf8_next(&text[offset], NULL);
        if (step == 0U) {
            break;
        }
        offset += step;
    }
    return offset;
}

void mesh_text_utf8_truncate(char *text, size_t max_chars) {
    if (text == NULL) {
        return;
    }
    const size_t offset = mesh_text_utf8_offset(text, max_chars);
    text[offset] = '\0';
}

void mesh_text_sanitise(const uint8_t *payload, size_t len, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (payload == NULL) {
        out[0] = '\0';
        return;
    }

    size_t written = 0;
    size_t i = 0;
    while (i < len) {
        const uint8_t byte = payload[i];
        if (byte == '\0') {
            /* Embedded NUL: treat it as the end of the text rather than truncating silently
               later in a str* call. */
            break;
        }

        if (byte < 0x80U) {
            if (written + 1U > out_len - 1U) {
                break;
            }
            if (byte == '\t' || byte == '\n' || byte == '\r') {
                out[written++] = ' ';
            } else if (byte < 0x20U || byte == 0x7FU) {
                out[written++] = '?';
            } else {
                out[written++] = (char)byte;
            }
            i += 1U;
            continue;
        }

        const size_t sequence = mesh_text_utf8_sequence_len(&payload[i], len - i);
        if (sequence == 0U) {
            if (written + 1U > out_len - 1U) {
                break;
            }
            out[written++] = '?';
            i += 1U;
            continue;
        }

        /* Never split a character across the truncation boundary. */
        if (written + sequence > out_len - 1U) {
            break;
        }
        memcpy(&out[written], &payload[i], sequence);
        written += sequence;
        i += sequence;
    }

    out[written] = '\0';
}

void mesh_text_sanitise_str(const char *in, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (in == NULL) {
        out[0] = '\0';
        return;
    }
    mesh_text_sanitise((const uint8_t *)in, strlen(in), out, out_len);
}
