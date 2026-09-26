#include "mesh/core/meshcore.h"

#include <errno.h>
#include <string.h>

/* Every field is packed little-endian, whatever the host is. */
static uint32_t mesh_meshcore_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static int32_t mesh_meshcore_i32(const uint8_t *bytes) { return (int32_t)mesh_meshcore_u32(bytes); }

static void mesh_meshcore_put_u32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8U) & 0xFFU);
    out[2] = (uint8_t)((value >> 16U) & 0xFFU);
    out[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

/* A fixed-width string field: NUL-padded when short, and not terminated at all when full. */
static void mesh_meshcore_copy_str(char *out, size_t out_len, const uint8_t *field,
                                   size_t field_len) {
    size_t n = 0U;
    while (n < field_len && n + 1U < out_len && field[n] != 0U) {
        out[n] = (char)field[n];
        ++n;
    }
    out[n] = '\0';
}

int mesh_meshcore_decode_self_info(const uint8_t *frame, size_t len,
                                   struct mesh_meshcore_self_info *out) {
    /* code, type, power, max power, key, lat, lon, four policy bytes, freq, bw, sf, cr */
    enum { FIXED = 1 + 3 + 32 + 8 + 4 + 8 + 2 };
    if (frame == NULL || out == NULL || len < FIXED || frame[0] != MESH_MESHCORE_RESP_SELF_INFO) {
        return -EBADMSG;
    }
    memset(out, 0, sizeof *out);
    size_t i = 1U;
    out->adv_type = frame[i++];
    out->tx_power_dbm = frame[i++];
    out->max_tx_power_dbm = frame[i++];
    memcpy(out->public_key, frame + i, MESH_MESHCORE_PUBKEY_LEN);
    i += MESH_MESHCORE_PUBKEY_LEN;
    out->latitude_e6 = mesh_meshcore_i32(frame + i);
    i += 4U;
    out->longitude_e6 = mesh_meshcore_i32(frame + i);
    i += 4U;
    out->multi_acks = frame[i++];
    out->advert_loc_policy = frame[i++];
    out->telemetry_modes = frame[i++];
    out->manual_add_contacts = frame[i++];
    out->frequency_khz = mesh_meshcore_u32(frame + i);
    i += 4U;
    out->bandwidth_hz = mesh_meshcore_u32(frame + i);
    i += 4U;
    out->spreading_factor = frame[i++];
    out->coding_rate = frame[i++];
    /* The name runs to the end of the frame, unterminated. */
    mesh_meshcore_copy_str(out->name, sizeof out->name, frame + i, len - i);
    return 0;
}

int mesh_meshcore_decode_device_info(const uint8_t *frame, size_t len,
                                     struct mesh_meshcore_device_info *out) {
    /* code, version, contacts/2, channels, pin, date 12, model 40, version 20 */
    enum { FIXED = 1 + 3 + 4 + 12 + 40 + 20 };
    if (frame == NULL || out == NULL || len < 2U || frame[0] != MESH_MESHCORE_RESP_DEVICE_INFO) {
        return -EBADMSG;
    }
    memset(out, 0, sizeof *out);
    out->firmware_version = frame[1];
    /* Firmware before v3 of the app protocol stops after the version byte. */
    if (len < FIXED) {
        return 0;
    }
    size_t i = 2U;
    out->max_contacts = (uint16_t)(frame[i++] * 2U);
    out->max_channels = frame[i++];
    out->ble_pin = mesh_meshcore_u32(frame + i);
    i += 4U;
    mesh_meshcore_copy_str(out->build_date, sizeof out->build_date, frame + i, 12U);
    i += 12U;
    mesh_meshcore_copy_str(out->model, sizeof out->model, frame + i, 40U);
    i += 40U;
    mesh_meshcore_copy_str(out->version, sizeof out->version, frame + i, 20U);
    return 0;
}

int mesh_meshcore_decode_contact(const uint8_t *frame, size_t len,
                                 struct mesh_meshcore_contact *out) {
    /* code, key, type, flags, path len, path 64, name 32, last advert, lat, lon, lastmod */
    enum { FIXED = 1 + 32 + 3 + 64 + 32 + 4, FULL = FIXED + 12 };
    if (frame == NULL || out == NULL || len < FIXED ||
        (frame[0] != MESH_MESHCORE_RESP_CONTACT && frame[0] != MESH_MESHCORE_PUSH_NEW_ADVERT)) {
        return -EBADMSG;
    }
    memset(out, 0, sizeof *out);
    size_t i = 1U;
    memcpy(out->public_key, frame + i, MESH_MESHCORE_PUBKEY_LEN);
    i += MESH_MESHCORE_PUBKEY_LEN;
    out->type = frame[i++];
    out->flags = frame[i++];
    out->out_path_len = frame[i++];
    i += MESH_MESHCORE_PATH_MAX; /* the route itself; the hop count above is all a screen shows */
    mesh_meshcore_copy_str(out->name, sizeof out->name, frame + i, MESH_MESHCORE_NAME_LEN);
    i += MESH_MESHCORE_NAME_LEN;
    out->last_advert = mesh_meshcore_u32(frame + i);
    i += 4U;
    if (len >= FULL) {
        out->latitude_e6 = mesh_meshcore_i32(frame + i);
        out->longitude_e6 = mesh_meshcore_i32(frame + i + 4U);
        out->lastmod = mesh_meshcore_u32(frame + i + 8U);
    }
    return 0;
}

int mesh_meshcore_decode_message(const uint8_t *frame, size_t len,
                                 struct mesh_meshcore_message *out) {
    if (frame == NULL || out == NULL || len < 1U) {
        return -EBADMSG;
    }
    memset(out, 0, sizeof *out);
    size_t i = 1U;
    switch (frame[0]) {
    case MESH_MESHCORE_RESP_CONTACT_MSG_RECV_V3:
    case MESH_MESHCORE_RESP_CHANNEL_MSG_RECV_V3:
        if (len < 4U) {
            return -EBADMSG;
        }
        out->has_snr = true;
        out->snr_q4 = (int8_t)frame[1];
        i = 4U; /* two reserved bytes */
        break;
    case MESH_MESHCORE_RESP_CONTACT_MSG_RECV:
    case MESH_MESHCORE_RESP_CHANNEL_MSG_RECV:
        break;
    default:
        return -EBADMSG;
    }
    out->channel = frame[0] == MESH_MESHCORE_RESP_CHANNEL_MSG_RECV ||
                   frame[0] == MESH_MESHCORE_RESP_CHANNEL_MSG_RECV_V3;

    /* prefix or channel, path length, text type, timestamp */
    const size_t header = (out->channel ? 1U : MESH_MESHCORE_PREFIX_LEN) + 1U + 1U + 4U;
    if (len < i + header) {
        return -EBADMSG;
    }
    if (out->channel) {
        out->channel_index = frame[i++];
    } else {
        memcpy(out->sender_prefix, frame + i, MESH_MESHCORE_PREFIX_LEN);
        i += MESH_MESHCORE_PREFIX_LEN;
    }
    out->path_len = frame[i++];
    out->txt_type = frame[i++];
    out->timestamp = mesh_meshcore_u32(frame + i);
    i += 4U;
    if (out->txt_type == MESH_MESHCORE_TXT_SIGNED_PLAIN) {
        if (len < i + 4U) {
            return -EBADMSG;
        }
        out->has_author = true;
        memcpy(out->author_prefix, frame + i, 4U);
        i += 4U;
    }
    out->text = frame + i;
    out->text_len = len - i;
    /* The firmware copies strlen() bytes, but a NUL inside is still not text. */
    const void *nul = memchr(out->text, 0, out->text_len);
    if (nul != NULL) {
        out->text_len = (size_t)((const uint8_t *)nul - out->text);
    }
    return 0;
}

int mesh_meshcore_decode_sent(const uint8_t *frame, size_t len, struct mesh_meshcore_sent *out) {
    if (frame == NULL || out == NULL || len < 10U || frame[0] != MESH_MESHCORE_RESP_SENT) {
        return -EBADMSG;
    }
    out->flood = frame[1] != 0U;
    out->expected_ack = mesh_meshcore_u32(frame + 2);
    out->timeout_ms = mesh_meshcore_u32(frame + 6);
    return 0;
}

int mesh_meshcore_decode_channel(const uint8_t *frame, size_t len,
                                 struct mesh_meshcore_channel *out) {
    if (frame == NULL || out == NULL || len < 2U + MESH_MESHCORE_NAME_LEN + 16U ||
        frame[0] != MESH_MESHCORE_RESP_CHANNEL_INFO) {
        return -EBADMSG;
    }
    memset(out, 0, sizeof *out);
    out->index = frame[1];
    mesh_meshcore_copy_str(out->name, sizeof out->name, frame + 2, MESH_MESHCORE_NAME_LEN);
    memcpy(out->secret, frame + 2 + MESH_MESHCORE_NAME_LEN, MESH_MESHCORE_SECRET_LEN);
    return 0;
}

int mesh_meshcore_encode_app_start(const char *app_name, uint8_t *out, size_t out_len) {
    const size_t name_len = app_name != NULL ? strlen(app_name) : 0U;
    const size_t total = 8U + name_len; /* code, seven reserved bytes, the name unterminated */
    if (out == NULL) {
        return -EINVAL;
    }
    if (total > MESH_MESHCORE_MAX_FRAME) {
        return -EMSGSIZE;
    }
    if (out_len < total) {
        return -ENOSPC;
    }
    memset(out, 0, 8U);
    out[0] = MESH_MESHCORE_CMD_APP_START;
    if (name_len > 0U) {
        memcpy(out + 8, app_name, name_len);
    }
    return (int)total;
}

int mesh_meshcore_encode_device_query(uint8_t version, uint8_t *out, size_t out_len) {
    return mesh_meshcore_encode_byte(MESH_MESHCORE_CMD_DEVICE_QUERY, version, out, out_len);
}

int mesh_meshcore_encode_u32(uint8_t cmd, uint32_t value, uint8_t *out, size_t out_len) {
    if (out == NULL) {
        return -EINVAL;
    }
    if (out_len < 5U) {
        return -ENOSPC;
    }
    out[0] = cmd;
    mesh_meshcore_put_u32(out + 1, value);
    return 5;
}

int mesh_meshcore_encode_byte(uint8_t cmd, uint8_t value, uint8_t *out, size_t out_len) {
    if (out == NULL) {
        return -EINVAL;
    }
    if (out_len < 2U) {
        return -ENOSPC;
    }
    out[0] = cmd;
    out[1] = value;
    return 2;
}

int mesh_meshcore_encode_key(uint8_t cmd, const uint8_t key[MESH_MESHCORE_PUBKEY_LEN], uint8_t *out,
                             size_t out_len) {
    if (out == NULL || key == NULL) {
        return -EINVAL;
    }
    if (out_len < 1U + MESH_MESHCORE_PUBKEY_LEN) {
        return -ENOSPC;
    }
    out[0] = cmd;
    memcpy(out + 1, key, MESH_MESHCORE_PUBKEY_LEN);
    return (int)(1U + MESH_MESHCORE_PUBKEY_LEN);
}

int mesh_meshcore_encode_text(const uint8_t prefix[MESH_MESHCORE_PREFIX_LEN], uint8_t attempt,
                              uint32_t timestamp, const char *text, uint8_t *out, size_t out_len) {
    if (prefix == NULL || text == NULL || out == NULL) {
        return -EINVAL;
    }
    const size_t text_len = strlen(text);
    const size_t total = 1U + 1U + 1U + 4U + MESH_MESHCORE_PREFIX_LEN + text_len;
    if (text_len > MESH_MESHCORE_TEXT_MAX || total > MESH_MESHCORE_MAX_FRAME) {
        return -EMSGSIZE;
    }
    if (out_len < total) {
        return -ENOSPC;
    }
    size_t i = 0U;
    out[i++] = MESH_MESHCORE_CMD_SEND_TXT_MSG;
    out[i++] = MESH_MESHCORE_TXT_PLAIN;
    out[i++] = attempt;
    mesh_meshcore_put_u32(out + i, timestamp);
    i += 4U;
    memcpy(out + i, prefix, MESH_MESHCORE_PREFIX_LEN);
    i += MESH_MESHCORE_PREFIX_LEN;
    memcpy(out + i, text, text_len);
    return (int)total;
}

int mesh_meshcore_encode_channel_text(uint8_t channel, uint32_t timestamp, const char *text,
                                      uint8_t *out, size_t out_len) {
    if (text == NULL || out == NULL) {
        return -EINVAL;
    }
    const size_t text_len = strlen(text);
    const size_t total = 1U + 1U + 1U + 4U + text_len;
    if (text_len > MESH_MESHCORE_TEXT_MAX || total > MESH_MESHCORE_MAX_FRAME) {
        return -EMSGSIZE;
    }
    if (out_len < total) {
        return -ENOSPC;
    }
    size_t i = 0U;
    out[i++] = MESH_MESHCORE_CMD_SEND_CHANNEL_TXT_MSG;
    out[i++] = MESH_MESHCORE_TXT_PLAIN;
    out[i++] = channel;
    mesh_meshcore_put_u32(out + i, timestamp);
    i += 4U;
    memcpy(out + i, text, text_len);
    return (int)total;
}

uint32_t mesh_meshcore_node_id(const uint8_t *key, size_t key_len) {
    if (key == NULL || key_len < 4U) {
        return 0U;
    }
    uint32_t id = ((uint32_t)key[0] << 24U) | ((uint32_t)key[1] << 16U) | ((uint32_t)key[2] << 8U) |
                  (uint32_t)key[3];
    if (id == 0U || id == 0xFFFFFFFFU) {
        id ^= 1U;
    }
    return id;
}

int mesh_meshcore_encode_name(const char *name, uint8_t *out, size_t out_len) {
    if (name == NULL || out == NULL || name[0] == '\0') {
        return -EINVAL;
    }
    const size_t len = strlen(name);
    if (len > MESH_MESHCORE_NAME_LEN) {
        return -EMSGSIZE;
    }
    if (out_len < 1U + len) {
        return -ENOSPC;
    }
    out[0] = MESH_MESHCORE_CMD_SET_ADVERT_NAME;
    memcpy(out + 1, name, len);
    return (int)(1U + len);
}

int mesh_meshcore_encode_radio_params(uint32_t frequency_khz, uint32_t bandwidth_hz,
                                      uint8_t spreading_factor, uint8_t coding_rate, uint8_t *out,
                                      size_t out_len) {
    if (out == NULL) {
        return -EINVAL;
    }
    if (out_len < 11U) {
        return -ENOSPC;
    }
    out[0] = MESH_MESHCORE_CMD_SET_RADIO_PARAMS;
    mesh_meshcore_put_u32(out + 1, frequency_khz);
    mesh_meshcore_put_u32(out + 5, bandwidth_hz);
    out[9] = spreading_factor;
    out[10] = coding_rate;
    return 11;
}

int mesh_meshcore_encode_latlon(int32_t latitude_e6, int32_t longitude_e6, uint8_t *out,
                                size_t out_len) {
    if (out == NULL) {
        return -EINVAL;
    }
    if (out_len < 9U) {
        return -ENOSPC;
    }
    out[0] = MESH_MESHCORE_CMD_SET_ADVERT_LATLON;
    mesh_meshcore_put_u32(out + 1, (uint32_t)latitude_e6);
    mesh_meshcore_put_u32(out + 5, (uint32_t)longitude_e6);
    return 9;
}

int mesh_meshcore_encode_set_channel(uint8_t index, const char *name,
                                     const uint8_t secret[MESH_MESHCORE_SECRET_LEN], uint8_t *out,
                                     size_t out_len) {
    const size_t total = 2U + MESH_MESHCORE_NAME_LEN + MESH_MESHCORE_SECRET_LEN;
    if (out == NULL || name == NULL || secret == NULL) {
        return -EINVAL;
    }
    const size_t name_len = strlen(name);
    if (name_len >= MESH_MESHCORE_NAME_LEN) {
        return -EMSGSIZE;
    }
    if (out_len < total) {
        return -ENOSPC;
    }
    memset(out, 0, total);
    out[0] = MESH_MESHCORE_CMD_SET_CHANNEL;
    out[1] = index;
    memcpy(out + 2, name, name_len);
    memcpy(out + 2 + MESH_MESHCORE_NAME_LEN, secret, MESH_MESHCORE_SECRET_LEN);
    return (int)total;
}

int mesh_meshcore_encode_contact(const struct mesh_meshcore_contact *contact, uint8_t *out,
                                 size_t out_len) {
    const size_t total =
        1U + MESH_MESHCORE_PUBKEY_LEN + 3U + MESH_MESHCORE_PATH_MAX + MESH_MESHCORE_NAME_LEN + 12U;
    if (contact == NULL || out == NULL) {
        return -EINVAL;
    }
    if (out_len < total) {
        return -ENOSPC;
    }
    memset(out, 0, total);
    size_t i = 0U;
    out[i++] = MESH_MESHCORE_CMD_ADD_UPDATE_CONTACT;
    memcpy(out + i, contact->public_key, MESH_MESHCORE_PUBKEY_LEN);
    i += MESH_MESHCORE_PUBKEY_LEN;
    out[i++] = contact->type;
    out[i++] = contact->flags;
    out[i++] = contact->out_path_len;
    i += MESH_MESHCORE_PATH_MAX; /* no route carried: the radio learns one */
    const size_t name_len = strnlen(contact->name, MESH_MESHCORE_NAME_LEN);
    memcpy(out + i, contact->name, name_len);
    i += MESH_MESHCORE_NAME_LEN;
    mesh_meshcore_put_u32(out + i, contact->last_advert);
    i += 4U;
    mesh_meshcore_put_u32(out + i, (uint32_t)contact->latitude_e6);
    i += 4U;
    mesh_meshcore_put_u32(out + i, (uint32_t)contact->longitude_e6);
    i += 4U;
    return (int)i;
}

int mesh_meshcore_encode_reboot(uint8_t *out, size_t out_len) {
    static const char k_word[] = "reboot";
    if (out == NULL) {
        return -EINVAL;
    }
    if (out_len < sizeof k_word) {
        return -ENOSPC;
    }
    out[0] = MESH_MESHCORE_CMD_REBOOT;
    memcpy(out + 1, k_word, sizeof k_word - 1U);
    return (int)sizeof k_word;
}
