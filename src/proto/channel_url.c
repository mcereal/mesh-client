#define _POSIX_C_SOURCE 200809L

/* See mesh/proto/channel_url.h. Bytes in, bytes out; nothing here knows what a radio is. */

#include "mesh/proto/channel_url.h"

#include "mesh/utils/base64.h"

#include <pb_decode.h>
#include <pb_encode.h>
#include <string.h>

/* The `#` is what separates the link from its payload in both spellings. Kept as the fragment
   marker rather than matching the whole prefix, because the host in front of it is not ours to
   have an opinion about - community sites mirror the page, and the payload is the same. */
static const char *payload_of(const char *text) {
    const char *hash = strrchr(text, '#');
    if (hash != NULL) {
        return hash + 1;
    }
    /* No fragment at all: a bare payload, which is what somebody typing one in will produce
       rather than spelling out twenty-six characters of URL on an on-screen keyboard. */
    return strchr(text, '/') == NULL ? text : NULL;
}

size_t mesh_channel_url_encode(const meshtastic_ChannelSet *set, bool add, char *out,
                               size_t out_len) {
    if (set == NULL || out == NULL || out_len == 0U) {
        return 0U;
    }
    out[0] = '\0';
    if (set->settings_count == 0U) {
        return 0U; /* a link to no channels is not a link */
    }

    uint8_t encoded[meshtastic_ChannelSet_size];
    pb_ostream_t stream = pb_ostream_from_buffer(encoded, sizeof encoded);
    if (!pb_encode(&stream, meshtastic_ChannelSet_fields, set)) {
        return 0U;
    }

    const size_t prefix_len = strlen(MESH_CHANNEL_URL_PREFIX);
    if (prefix_len >= out_len) {
        return 0U;
    }
    memcpy(out, MESH_CHANNEL_URL_PREFIX, prefix_len);
    const size_t payload = mesh_base64_encode(encoded, stream.bytes_written, true, out + prefix_len,
                                              out_len - prefix_len);
    if (payload == 0U) {
        out[0] = '\0';
        return 0U;
    }
    size_t len = prefix_len + payload;
    if (add) {
        static const char k_add[] = "?add=true";
        if (len + sizeof k_add > out_len) {
            out[0] = '\0';
            return 0U;
        }
        memcpy(out + len, k_add, sizeof k_add);
        len += sizeof k_add - 1U;
    }
    return len;
}

bool mesh_channel_url_decode(const char *text, meshtastic_ChannelSet *out, bool *out_add) {
    if (text == NULL || out == NULL) {
        return false;
    }
    const char *payload = payload_of(text);
    if (payload == NULL || payload[0] == '\0') {
        return false;
    }

    /* The query tail, if there is one, is the sender's claim about the reader's radio rather
       than part of the set - so it comes off before the base64 and is answered separately. */
    size_t chars = strcspn(payload, "?&");
    const char *query = payload + chars;
    if (out_add != NULL) {
        *out_add = strstr(query, "add=true") != NULL;
    }

    uint8_t decoded[meshtastic_ChannelSet_size];
    size_t len = 0U;
    if (!mesh_base64_decode(payload, chars, MESH_BASE64_ANY, decoded, sizeof decoded, &len)) {
        return false;
    }

    *out = (meshtastic_ChannelSet)meshtastic_ChannelSet_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(decoded, len);
    if (!pb_decode(&stream, meshtastic_ChannelSet_fields, out)) {
        return false;
    }
    /* A set with no channels in it decodes cleanly from an empty payload and means nothing;
       refusing it here is what keeps every caller from having to check. */
    return out->settings_count > 0U;
}
