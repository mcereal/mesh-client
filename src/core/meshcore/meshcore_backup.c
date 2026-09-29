#include "mesh/core/meshcore_backup.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * The SELF section, little-endian and packed, in this order:
 *
 *   public key 32, name 32 (NUL-padded), frequency kHz u32, bandwidth Hz u32, spreading factor,
 *   coding rate, TX power dBm, latitude e6 i32, longitude e6 i32, manual-add, telemetry modes,
 *   advert location policy, multi-acks, advert type
 *
 * A later build may append fields; a reader takes the first MC_SELF_LEN bytes and leaves the
 * rest. Moving or narrowing one is a new tag, never an edit to this one.
 */
#define MC_SELF_LEN 88U
#define MC_CHANNEL_LEN (1U + MESH_MESHCORE_NAME_LEN + MESH_MESHCORE_SECRET_LEN)
/* RESP_CONTACT: code, key, type, flags, path length, path, name, advert stamp, lat, lon,
   lastmod. */
#define MC_CONTACT_LEN                                                                             \
    (1U + MESH_MESHCORE_PUBKEY_LEN + 3U + MESH_MESHCORE_PATH_MAX + MESH_MESHCORE_NAME_LEN + 16U)

/* A radio with a full book still fits in one backup; a capture never ends in -ENOSPC. */
_Static_assert(MESH_MESHCORE_CONTACTS_MAX *MC_CONTACT_LEN +
                       MESH_MESHCORE_CHANNELS_KEPT * MC_CHANNEL_LEN + MC_SELF_LEN + 4U <=
                   MESH_RADIO_BACKUP_PAYLOAD_MAX,
               "a full contact book must fit in a backup's payload");
_Static_assert(MESH_MESHCORE_CONTACTS_MAX + MESH_MESHCORE_CHANNELS_KEPT + 2U <=
                   MESH_RADIO_BACKUP_SECTIONS_MAX,
               "a full contact book must fit in a backup's sections");

static void mc_put_u32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
}

static uint32_t mc_u32(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8U) | ((uint32_t)in[2] << 16U) |
           ((uint32_t)in[3] << 24U);
}

/* A name field of `width` bytes, NUL-padded, from a string that may be shorter or longer. */
static void mc_put_name(uint8_t *out, const char *name, size_t width) {
    memset(out, 0, width);
    memcpy(out, name, strnlen(name, width));
}

static void mc_name(char *out, size_t out_len, const uint8_t *in, size_t width) {
    const size_t len = strnlen((const char *)in, width) < out_len - 1U
                           ? strnlen((const char *)in, width)
                           : out_len - 1U;
    memcpy(out, in, len);
    out[len] = '\0';
}

bool mesh_meshcore_backup_ready(const struct mesh_meshcore *meshcore) {
    return mesh_meshcore_ready(meshcore) && meshcore->has_self && meshcore->has_device &&
           meshcore->self_node != 0U;
}

static int mc_add_self(const struct mesh_meshcore_self_info *self,
                       struct mesh_radio_backup *backup) {
    uint8_t out[MC_SELF_LEN];
    size_t i = 0U;
    memcpy(out + i, self->public_key, MESH_MESHCORE_PUBKEY_LEN);
    i += MESH_MESHCORE_PUBKEY_LEN;
    mc_put_name(out + i, self->name, MESH_MESHCORE_NAME_LEN);
    i += MESH_MESHCORE_NAME_LEN;
    mc_put_u32(out + i, self->frequency_khz);
    i += 4U;
    mc_put_u32(out + i, self->bandwidth_hz);
    i += 4U;
    out[i++] = self->spreading_factor;
    out[i++] = self->coding_rate;
    out[i++] = self->tx_power_dbm;
    mc_put_u32(out + i, (uint32_t)self->latitude_e6);
    i += 4U;
    mc_put_u32(out + i, (uint32_t)self->longitude_e6);
    i += 4U;
    out[i++] = self->manual_add_contacts;
    out[i++] = self->telemetry_modes;
    out[i++] = self->advert_loc_policy;
    out[i++] = self->multi_acks;
    out[i++] = self->adv_type;
    return mesh_radio_backup_add(backup, MESH_MESHCORE_BACKUP_SELF, out, i);
}

static int mc_add_channel(const struct mesh_meshcore_channel *channel,
                          struct mesh_radio_backup *backup) {
    uint8_t out[MC_CHANNEL_LEN];
    out[0] = channel->index;
    mc_put_name(out + 1, channel->name, MESH_MESHCORE_NAME_LEN);
    memcpy(out + 1 + MESH_MESHCORE_NAME_LEN, channel->secret, MESH_MESHCORE_SECRET_LEN);
    return mesh_radio_backup_add(backup, MESH_MESHCORE_BACKUP_CHANNEL, out, sizeof out);
}

/* The record as the radio sends it: the add command's body under RESP_CONTACT's code, with
   lastmod on the end - which is exactly what mesh_meshcore_decode_contact() reads. */
static int mc_add_contact(const struct mesh_meshcore_contact *contact,
                          struct mesh_radio_backup *backup) {
    uint8_t out[MESH_MESHCORE_MAX_FRAME];
    const int len = mesh_meshcore_encode_contact(contact, out, sizeof out);
    if (len < 0 || (size_t)len + 4U != MC_CONTACT_LEN) {
        return -EINVAL;
    }
    out[0] = MESH_MESHCORE_RESP_CONTACT;
    mc_put_u32(out + len, contact->lastmod);
    return mesh_radio_backup_add(backup, MESH_MESHCORE_BACKUP_CONTACT, out, MC_CONTACT_LEN);
}

static bool mc_channel_used(const struct mesh_meshcore_channel *channel) {
    static const uint8_t zero[MESH_MESHCORE_SECRET_LEN] = {0};
    return channel->name[0] != '\0' || memcmp(channel->secret, zero, sizeof zero) != 0;
}

static void mc_header(const struct mesh_meshcore *meshcore,
                      struct mesh_radio_backup_header *header) {
    const struct mesh_meshcore_self_info *self = &meshcore->self;
    header->protocol = MESH_RADIO_BACKUP_MESHCORE;
    header->node_id = meshcore->self_node;
    snprintf(header->name, sizeof header->name, "%s", self->name);
    snprintf(header->model, sizeof header->model, "%s", meshcore->device.model);
    snprintf(header->firmware, sizeof header->firmware, "%s", meshcore->device.version);
    header->has_radio = true;
    header->frequency_khz = self->frequency_khz;
    header->bandwidth_hz = self->bandwidth_hz;
    header->spreading_factor = self->spreading_factor;
    header->coding_rate = self->coding_rate;
    header->tx_power_dbm = (int8_t)self->tx_power_dbm;
    for (size_t i = 0; i < MESH_MESHCORE_CHANNELS_KEPT && i < MESH_RADIO_BACKUP_CHANNELS; ++i) {
        if (meshcore->has_channel[i] && mc_channel_used(&meshcore->channels[i])) {
            snprintf(header->channel_names[i], sizeof header->channel_names[i], "%s",
                     meshcore->channels[i].name);
            header->channel_count = (uint8_t)(i + 1U);
        }
    }
    header->has_contacts = true;
    header->contacts = (uint32_t)meshcore->contact_count;
}

int mesh_meshcore_backup_capture(const struct mesh_meshcore *meshcore,
                                 struct mesh_radio_backup *backup) {
    if (backup == NULL) {
        return -EINVAL;
    }
    mesh_radio_backup_reset(backup);
    if (!mesh_meshcore_backup_ready(meshcore)) {
        return -EAGAIN;
    }
    /* A book that dropped a record is a radio with contacts this client does not hold, and a
       backup of it would say "complete" about a list with holes in it. */
    if (meshcore->contacts_unkept > 0U) {
        return -EOVERFLOW;
    }
    mc_header(meshcore, &backup->header);

    int result = mc_add_self(&meshcore->self, backup);
    if (result == 0) {
        uint8_t pin[4];
        mc_put_u32(pin, meshcore->device.ble_pin);
        result = mesh_radio_backup_add(backup, MESH_MESHCORE_BACKUP_PIN, pin, sizeof pin);
    }
    /* Every slot the walk read, used or not: an empty slot restored is a slot cleared. */
    for (size_t i = 0; result == 0 && i < MESH_MESHCORE_CHANNELS_KEPT; ++i) {
        if (meshcore->has_channel[i]) {
            result = mc_add_channel(&meshcore->channels[i], backup);
        }
    }
    for (size_t i = 0; result == 0 && i < meshcore->contact_count; ++i) {
        result = mc_add_contact(&meshcore->contacts[i], backup);
    }
    if (result != 0) {
        mesh_radio_backup_reset(backup);
    }
    return result;
}

/* ---- reading back -------------------------------------------------------------------------- */

static bool mc_read_self(const uint8_t *in, size_t len, struct mesh_meshcore_self_info *self) {
    if (len < MC_SELF_LEN) {
        return false;
    }
    memset(self, 0, sizeof *self);
    size_t i = 0U;
    memcpy(self->public_key, in + i, MESH_MESHCORE_PUBKEY_LEN);
    i += MESH_MESHCORE_PUBKEY_LEN;
    mc_name(self->name, sizeof self->name, in + i, MESH_MESHCORE_NAME_LEN);
    i += MESH_MESHCORE_NAME_LEN;
    self->frequency_khz = mc_u32(in + i);
    i += 4U;
    self->bandwidth_hz = mc_u32(in + i);
    i += 4U;
    self->spreading_factor = in[i++];
    self->coding_rate = in[i++];
    self->tx_power_dbm = in[i++];
    self->latitude_e6 = (int32_t)mc_u32(in + i);
    i += 4U;
    self->longitude_e6 = (int32_t)mc_u32(in + i);
    i += 4U;
    self->manual_add_contacts = in[i++];
    self->telemetry_modes = in[i++];
    self->advert_loc_policy = in[i++];
    self->multi_acks = in[i++];
    self->adv_type = in[i++];
    return true;
}

int mesh_meshcore_backup_read(const struct mesh_radio_backup *backup,
                              struct mesh_meshcore_backup_contents *out) {
    if (backup == NULL || out == NULL) {
        return -EINVAL;
    }
    if (backup->header.protocol != MESH_RADIO_BACKUP_MESHCORE) {
        return -EPROTO;
    }
    memset(out, 0, sizeof *out);
    const uint8_t *data = NULL;
    const struct mesh_radio_backup_section *section = NULL;
    for (size_t i = 0; (section = mesh_radio_backup_section_at(backup, i, &data)) != NULL; ++i) {
        bool ok = true;
        switch (section->tag) {
        case MESH_MESHCORE_BACKUP_SELF:
            ok = mc_read_self(data, section->len, &out->self);
            out->has_self = ok;
            break;
        case MESH_MESHCORE_BACKUP_PIN:
            ok = section->len >= 4U;
            if (ok) {
                out->ble_pin = mc_u32(data);
                out->has_pin = true;
            }
            break;
        case MESH_MESHCORE_BACKUP_CHANNEL: {
            ok = section->len >= MC_CHANNEL_LEN && data[0] < MESH_MESHCORE_CHANNELS_KEPT;
            if (ok) {
                struct mesh_meshcore_channel *channel = &out->channels[data[0]];
                channel->index = data[0];
                mc_name(channel->name, sizeof channel->name, data + 1, MESH_MESHCORE_NAME_LEN);
                memcpy(channel->secret, data + 1 + MESH_MESHCORE_NAME_LEN,
                       MESH_MESHCORE_SECRET_LEN);
                out->has_channel[data[0]] = true;
            }
            break;
        }
        case MESH_MESHCORE_BACKUP_CONTACT:
            if (out->contact_count < MESH_MESHCORE_CONTACTS_MAX) {
                ok = section->len >= MC_CONTACT_LEN &&
                     mesh_meshcore_decode_contact(data, section->len,
                                                  &out->contacts[out->contact_count]) == 0;
                out->contact_count += ok ? 1U : 0U;
            }
            break;
        default:
            break; /* a section a later build added */
        }
        if (!ok) {
            memset(out, 0, sizeof *out);
            return -EBADMSG;
        }
    }
    return 0;
}
