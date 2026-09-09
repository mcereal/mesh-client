#include "mesh/utils/json.h"

#include <string.h>

static void json_skip_space(struct mesh_json *json) {
    while (json->cursor < json->end) {
        const char c = *json->cursor;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return;
        }
        json->cursor++;
    }
}

/* The byte the cursor is on, or '\0' at the end - so every test below can be written as a
   comparison without a bounds check beside it. */
static char json_peek(struct mesh_json *json) {
    json_skip_space(json);
    return json->cursor < json->end ? *json->cursor : '\0';
}

static bool json_take(struct mesh_json *json, char expected) {
    if (json_peek(json) != expected) {
        return false;
    }
    json->cursor++;
    return true;
}

/*
 * Steps the cursor past a string, which is the one thing every other walk here needs to get
 * right: a brace inside a string is not a brace, and that is the whole difference between this
 * reader and a scanner. Assumes the opening quote has been consumed.
 */
static bool json_skip_string_body(struct mesh_json *json) {
    while (json->cursor < json->end) {
        const char c = *json->cursor++;
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (json->cursor >= json->end) {
                return false;
            }
            json->cursor++;
        }
    }
    return false;
}

/* Writes `value` as UTF-8 into `out` if it fits whole, and says how many bytes it took. A
   partial character is never written: a truncated string still has to be valid text. */
static size_t json_encode_utf8(uint32_t value, char *out, size_t room) {
    if (value < 0x80U) {
        if (room < 1U) {
            return 0U;
        }
        out[0] = (char)value;
        return 1U;
    }
    if (value < 0x800U) {
        if (room < 2U) {
            return 0U;
        }
        out[0] = (char)(0xC0U | (value >> 6));
        out[1] = (char)(0x80U | (value & 0x3FU));
        return 2U;
    }
    if (room < 3U) {
        return 0U;
    }
    out[0] = (char)(0xE0U | (value >> 12));
    out[1] = (char)(0x80U | ((value >> 6) & 0x3FU));
    out[2] = (char)(0x80U | (value & 0x3FU));
    return 3U;
}

static bool json_hex4(const char *from, uint32_t *out) {
    uint32_t value = 0U;
    for (size_t i = 0; i < 4U; ++i) {
        const char c = from[i];
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= (uint32_t)(c - 'a') + 10U;
        } else if (c >= 'A' && c <= 'F') {
            value |= (uint32_t)(c - 'A') + 10U;
        } else {
            return false;
        }
    }
    *out = value;
    return true;
}

/*
 * Reads a string into `out`, unescaping as it goes. Assumes the opening quote has been
 * consumed. `out` may be NULL, which reads the string for its length and throws it away.
 */
static bool json_read_string_body(struct mesh_json *json, char *out, size_t out_len) {
    size_t written = 0U;
    /* Leave room for the terminator, so every append below is a plain bounds test. */
    const size_t room = (out != NULL && out_len > 0U) ? out_len - 1U : 0U;
    while (json->cursor < json->end) {
        const char c = *json->cursor++;
        if (c == '"') {
            if (out != NULL && out_len > 0U) {
                out[written] = '\0';
            }
            return true;
        }
        char decoded[4];
        size_t decoded_len = 0U;
        if (c != '\\') {
            decoded[0] = c;
            decoded_len = 1U;
        } else {
            if (json->cursor >= json->end) {
                return false;
            }
            const char escape = *json->cursor++;
            switch (escape) {
            case 'n':
                decoded[0] = '\n';
                decoded_len = 1U;
                break;
            case 't':
                decoded[0] = '\t';
                decoded_len = 1U;
                break;
            case 'r':
                decoded[0] = '\r';
                decoded_len = 1U;
                break;
            case 'b':
                decoded[0] = '\b';
                decoded_len = 1U;
                break;
            case 'f':
                decoded[0] = '\f';
                decoded_len = 1U;
                break;
            case 'u': {
                if ((size_t)(json->end - json->cursor) < 4U) {
                    return false;
                }
                uint32_t value = 0U;
                if (!json_hex4(json->cursor, &value)) {
                    return false;
                }
                json->cursor += 4;
                /* Half of a surrogate pair is not a character. Pairing them would mean
                   carrying state across an escape for a code point no field here holds, so it
                   becomes the same '?' a decoder gives up with. */
                if (value >= 0xD800U && value <= 0xDFFFU) {
                    value = (uint32_t)'?';
                }
                decoded_len = json_encode_utf8(value, decoded, sizeof decoded);
                break;
            }
            default:
                /* Includes the two that stand for themselves, '"' and '\\', and anything the
                   document invented - passed through rather than refused. */
                decoded[0] = escape;
                decoded_len = 1U;
                break;
            }
        }
        if (written + decoded_len <= room) {
            memcpy(out + written, decoded, decoded_len);
            written += decoded_len;
        }
        /* Past the buffer the read continues without writing: the cursor has to end up after
           the string whatever the caller had room for. */
    }
    return false;
}

void mesh_json_init(struct mesh_json *json, const char *text, size_t len) {
    if (json == NULL) {
        return;
    }
    if (text == NULL) {
        json->cursor = NULL;
        json->end = NULL;
        return;
    }
    json->cursor = text;
    json->end = text + (len > 0U ? len : strlen(text));
}

bool mesh_json_enter_object(struct mesh_json *json) {
    return json != NULL && json->cursor != NULL && json_take(json, '{');
}

bool mesh_json_enter_array(struct mesh_json *json) {
    return json != NULL && json->cursor != NULL && json_take(json, '[');
}

bool mesh_json_next_key(struct mesh_json *json, char *out, size_t out_len) {
    if (json == NULL || json->cursor == NULL) {
        return false;
    }
    if (out != NULL && out_len > 0U) {
        out[0] = '\0';
    }
    /* A comma before the next key, or nothing before the first. */
    (void)json_take(json, ',');
    if (json_take(json, '}')) {
        return false;
    }
    if (!json_take(json, '"')) {
        return false;
    }
    if (!json_read_string_body(json, out, out_len)) {
        return false;
    }
    return json_take(json, ':');
}

bool mesh_json_next_element(struct mesh_json *json) {
    if (json == NULL || json->cursor == NULL) {
        return false;
    }
    (void)json_take(json, ',');
    if (json_take(json, ']')) {
        return false;
    }
    /* Anything else is an element, and the cursor is already on it. An empty document ends
       the walk rather than reporting one more. */
    return json_peek(json) != '\0';
}

bool mesh_json_read_string(struct mesh_json *json, char *out, size_t out_len) {
    if (json == NULL || json->cursor == NULL || json_peek(json) != '"') {
        return false;
    }
    json->cursor++;
    return json_read_string_body(json, out, out_len);
}

bool mesh_json_read_u64(struct mesh_json *json, uint64_t *out) {
    if (json == NULL || json->cursor == NULL) {
        return false;
    }
    const char first = json_peek(json);
    if (first < '0' || first > '9') {
        return false;
    }
    uint64_t value = 0U;
    while (json->cursor < json->end && *json->cursor >= '0' && *json->cursor <= '9') {
        const uint64_t digit = (uint64_t)(*json->cursor - '0');
        if (value > (UINT64_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        json->cursor++;
    }
    if (out != NULL) {
        *out = value;
    }
    return true;
}

bool mesh_json_read_bool(struct mesh_json *json, bool *out) {
    if (json == NULL || json->cursor == NULL) {
        return false;
    }
    const char first = json_peek(json);
    const size_t left = (size_t)(json->end - json->cursor);
    if (first == 't' && left >= 4U && memcmp(json->cursor, "true", 4U) == 0) {
        json->cursor += 4;
        if (out != NULL) {
            *out = true;
        }
        return true;
    }
    if (first == 'f' && left >= 5U && memcmp(json->cursor, "false", 5U) == 0) {
        json->cursor += 5;
        if (out != NULL) {
            *out = false;
        }
        return true;
    }
    return false;
}

bool mesh_json_skip_value(struct mesh_json *json) {
    if (json == NULL || json->cursor == NULL) {
        return false;
    }
    const char first = json_peek(json);
    if (first == '\0') {
        return false;
    }
    if (first == '"') {
        json->cursor++;
        return json_skip_string_body(json);
    }
    if (first != '{' && first != '[') {
        /* A number, or one of the three literals. Ends at whatever closes or separates it. */
        while (json->cursor < json->end) {
            const char c = *json->cursor;
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' ||
                c == '\r') {
                break;
            }
            json->cursor++;
        }
        return true;
    }

    /* Counted rather than recursed, so a deeply nested document costs a number and not the
       stack of the one thread this client has. */
    size_t depth = 0U;
    while (json->cursor < json->end) {
        const char c = *json->cursor++;
        if (c == '"') {
            if (!json_skip_string_body(json)) {
                return false;
            }
            continue;
        }
        if (c == '{' || c == '[') {
            depth++;
            if (depth > MESH_JSON_MAX_DEPTH) {
                return false;
            }
            continue;
        }
        if (c == '}' || c == ']') {
            depth--;
            if (depth == 0U) {
                return true;
            }
        }
    }
    return false;
}

bool mesh_json_object_find(struct mesh_json *json, const char *key) {
    if (json == NULL || key == NULL || !mesh_json_enter_object(json)) {
        return false;
    }
    char name[64];
    while (mesh_json_next_key(json, name, sizeof name)) {
        if (strcmp(name, key) == 0) {
            return true;
        }
        if (!mesh_json_skip_value(json)) {
            return false;
        }
    }
    return false;
}
