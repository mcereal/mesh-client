/* See mesh/proto/meshcore_url.h. */

#include "mesh/proto/meshcore_url.h"

#include <string.h>

static const char k_hex[] = "0123456789abcdef";

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* RFC 3986's unreserved characters go as they are; a space is `+`, as the apps write it. */
static bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~';
}

/* Percent-encodes up to `max` bytes of `name` onto buf at *n. */
static void encode_name(const char *name, size_t max, char *buf, size_t *n) {
    const size_t name_len = strnlen(name, max);
    for (size_t i = 0; i < name_len; ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (unreserved(c)) {
            buf[(*n)++] = (char)c;
        } else if (c == ' ') {
            buf[(*n)++] = '+';
        } else {
            buf[(*n)++] = '%';
            buf[(*n)++] = k_hex[c >> 4U];
            buf[(*n)++] = k_hex[c & 0x0FU];
        }
    }
}

static void encode_hex(const uint8_t *bytes, size_t len, char *buf, size_t *n) {
    for (size_t i = 0; i < len; ++i) {
        buf[(*n)++] = k_hex[bytes[i] >> 4U];
        buf[(*n)++] = k_hex[bytes[i] & 0x0FU];
    }
}

/* Exactly 2 * len hex characters into bytes. */
static bool decode_hex(const char *value, size_t value_len, uint8_t *bytes, size_t len) {
    if (value_len != len * 2U) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        const int hi = hex_value(value[2U * i]);
        const int lo = hex_value(value[2U * i + 1U]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* The query after `prefix`, or NULL: the scheme and host are compared without regard to case,
   as a URL's are. */
static const char *after_prefix(const char *text, const char *prefix) {
    const size_t prefix_len = strlen(prefix);
    if (strlen(text) < prefix_len) {
        return NULL;
    }
    for (size_t i = 0; i < prefix_len; ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != prefix[i]) {
            return NULL;
        }
    }
    return text + prefix_len;
}

size_t mesh_meshcore_contact_url_encode(const struct mesh_meshcore_contact_link *link, char *out,
                                        size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return 0U;
    }
    out[0] = '\0';
    if (link == NULL || link->type < 1U || link->type > 4U) {
        return 0U;
    }
    char buf[MESH_MESHCORE_CONTACT_URL_MAX];
    size_t n = 0U;
    const char *head = MESH_MESHCORE_URL_CONTACT_PREFIX "name=";
    memcpy(buf, head, strlen(head));
    n += strlen(head);
    encode_name(link->name, MESH_MESHCORE_URL_NAME_LEN, buf, &n);
    const char *key = "&public_key=";
    memcpy(buf + n, key, strlen(key));
    n += strlen(key);
    encode_hex(link->public_key, MESH_MESHCORE_URL_KEY_LEN, buf, &n);
    const char *type = "&type=";
    memcpy(buf + n, type, strlen(type));
    n += strlen(type);
    buf[n++] = (char)('0' + link->type);
    if (n + 1U > out_len) {
        return 0U;
    }
    memcpy(out, buf, n);
    out[n] = '\0';
    return n;
}

/* Percent-decodes [value, end) into out; false for a bad escape or one too many bytes. */
static bool decode_name(const char *value, const char *end, char *out, size_t out_len) {
    size_t n = 0U;
    for (const char *p = value; p < end; ++p) {
        char c = *p;
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (end - p < 3) {
                return false;
            }
            const int hi = hex_value(p[1]);
            const int lo = hex_value(p[2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            c = (char)((hi << 4) | lo);
            p += 2;
        }
        if (c == '\0' || n + 1U >= out_len) {
            return false;
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return true;
}

bool mesh_meshcore_contact_url_decode(const char *text, struct mesh_meshcore_contact_link *out) {
    if (text == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    const char *p = after_prefix(text, MESH_MESHCORE_URL_CONTACT_PREFIX);
    if (p == NULL) {
        return false;
    }
    bool have_key = false;
    bool have_type = false;
    while (*p != '\0') {
        const char *amp = strchr(p, '&');
        const char *end = amp != NULL ? amp : p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq != NULL) {
            const size_t name_len = (size_t)(eq - p);
            const char *value = eq + 1;
            const size_t value_len = (size_t)(end - value);
            if (name_len == 4U && memcmp(p, "name", 4U) == 0) {
                if (!decode_name(value, end, out->name, sizeof out->name)) {
                    return false;
                }
            } else if (name_len == 10U && memcmp(p, "public_key", 10U) == 0) {
                if (!decode_hex(value, value_len, out->public_key, MESH_MESHCORE_URL_KEY_LEN)) {
                    return false;
                }
                have_key = true;
            } else if (name_len == 4U && memcmp(p, "type", 4U) == 0) {
                if (value_len != 1U || value[0] < '1' || value[0] > '4') {
                    return false;
                }
                out->type = (uint8_t)(value[0] - '0');
                have_type = true;
            }
        }
        p = amp != NULL ? amp + 1 : end;
    }
    return have_key && have_type;
}

size_t mesh_meshcore_channel_url_encode(const struct mesh_meshcore_channel_link *link, char *out,
                                        size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return 0U;
    }
    out[0] = '\0';
    if (link == NULL || link->name[0] == '\0' ||
        strnlen(link->name, sizeof link->name) > MESH_MESHCORE_URL_CHANNEL_NAME_LEN) {
        return 0U;
    }
    char buf[MESH_MESHCORE_CHANNEL_URL_MAX];
    size_t n = 0U;
    const char *head = MESH_MESHCORE_URL_CHANNEL_PREFIX "name=";
    memcpy(buf, head, strlen(head));
    n += strlen(head);
    encode_name(link->name, MESH_MESHCORE_URL_CHANNEL_NAME_LEN, buf, &n);
    const char *secret = "&secret=";
    memcpy(buf + n, secret, strlen(secret));
    n += strlen(secret);
    encode_hex(link->secret, MESH_MESHCORE_URL_SECRET_LEN, buf, &n);
    if (n + 1U > out_len) {
        return 0U;
    }
    memcpy(out, buf, n);
    out[n] = '\0';
    return n;
}

bool mesh_meshcore_channel_url_decode(const char *text, struct mesh_meshcore_channel_link *out) {
    if (text == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    const char *p = after_prefix(text, MESH_MESHCORE_URL_CHANNEL_PREFIX);
    if (p == NULL) {
        return false;
    }
    bool have_secret = false;
    while (*p != '\0') {
        const char *amp = strchr(p, '&');
        const char *end = amp != NULL ? amp : p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq != NULL) {
            const size_t name_len = (size_t)(eq - p);
            const char *value = eq + 1;
            if (name_len == 4U && memcmp(p, "name", 4U) == 0) {
                if (!decode_name(value, end, out->name, sizeof out->name)) {
                    return false;
                }
            } else if (name_len == 6U && memcmp(p, "secret", 6U) == 0) {
                if (!decode_hex(value, (size_t)(end - value), out->secret,
                                MESH_MESHCORE_URL_SECRET_LEN)) {
                    return false;
                }
                have_secret = true;
            }
        }
        p = amp != NULL ? amp + 1 : end;
    }
    /* A slot with no name is an unused one on the radio, so a link without one names nothing
       it could hold. */
    return have_secret && out->name[0] != '\0';
}
